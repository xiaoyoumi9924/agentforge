// ---------------------------------------------------------------------------
//  L5 · main.cpp —— AgentLoop 接入 + 四个工具 + 观察者 + --selftest
//
//  本讲把 L4 的"临时 for 循环"正式抽象成 AgentLoop：
//    · 循环内核在 agent_loop.{hpp,cpp}，main 只做组合根
//    · 上下文组装在 context_budget.{hpp,cpp}（分层 + 预算 + 降级链）
//    · 四个工具：Read / Write / Edit / Bash（Bash 退出码必须进结果）
//    · 观察者：LoopHooks 把流式增量/工具事件/通知/权限询问抛给 UI
//    · --selftest：不联网、不花钱的进程内自检
//
//  用法：
//      set OPENAI_API_KEY=sk-...
//      agent.exe "mock_target.txt 里有几个函数？"
//      agent.exe --selftest
//      set OPENAI_BASE_URL=http://127.0.0.1:8789/v1  （本地/假服务端，无需密钥）
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#  ifndef _CRT_SECURE_NO_WARNINGS
#    define _CRT_SECURE_NO_WARNINGS
#  endif
#endif

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  pragma comment(lib, "shell32.lib")
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agent_loop.hpp"
#include "config.hpp"
#include "context_budget.hpp"
#include "logging.hpp"
#include "model_provider.hpp"
#include "repo_index.hpp"
#include "sandbox.hpp"
#include "secret.hpp"
#include "session_store.hpp"
#include "tool_registry.hpp"
#include "tools.hpp"
#include "types.hpp"

using namespace af;

namespace {

// ============================================================================
//  Windows 编码两个坑（与 L3/L4 同理，逐讲复用）
// ============================================================================

#ifdef _WIN32
std::string envUtf8(const char* key, const std::string& def) {
    std::wstring wkey;
    for (const char* p = key; *p; ++p) wkey.push_back(wchar_t(unsigned char(*p)));
    const wchar_t* w = _wgetenv(wkey.c_str());
    if (!w || !*w) return def;
    const int sz = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return def;
    std::string s(std::size_t(sz - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), sz, nullptr, nullptr);
    return s;
}

std::vector<std::string> winUtf8Args() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return out;
    for (int i = 0; i < n; ++i) {
        int sz = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        if (sz <= 1) { out.emplace_back(); continue; }
        std::string s(std::size_t(sz - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), sz, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    LocalFree(w);
    return out;
}
#else
std::string envUtf8(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? sanitizeUtf8(v) : def;
}
#endif

// ============================================================================
//  端口 ④ 的终端实现
// ============================================================================
class ConsoleUI : public IUIPort {
public:
    bool confirm(const std::string& prompt) override {
        std::cout << "\n[权限] " << prompt << "\n  允许? [y/N] " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
    }
    void notice(const std::string& msg) override {
        std::cout << "[通知] " << msg << "\n" << std::flush;
    }
};

// 四个工具（Read/Write/Edit/Bash）已抽到 src/tools.hpp / tools.cpp，CLI 与 Qt GUI 共用。

// ============================================================================
//  渲染：ChatChunk → 终端（穷尽 variant 分支，没有兜底）
// ============================================================================
struct Renderer {
    void operator()(const ChatChunk& c) {
        std::visit(overloaded{
            [&](const TextDelta& d) { std::cout << d.text << std::flush; },
            [&](const ThinkingDelta& d) {
                std::cout << "\033[2m" << d.text << "\033[0m" << std::flush;
            },
            [&](const ToolCallStart& s) {
                std::cout << "\n[模型请求工具] " << s.name << "\n" << std::flush;
            },
            [&](const ToolCallDelta&) {},
            [&](const ToolCallDone& t) {
                std::cout << "[参数] " << t.args.dump() << "\n" << std::flush;
            },
            [&](const UsageChunk&) {},
            [&](const ErrorChunk& e) {
                std::cerr << "\n[流内错误] " << e.message
                          << (e.retryable ? "（可重试）" : "") << "\n";
            },
            [&](const DoneChunk&) {},
        }, c);
    }
};

// ============================================================================
//  --selftest：进程内自检（不联网、不花钱）
//
//  覆盖：预算分配、上下文测量、消息守卫（孤儿 tool 修复）、
//        AgentLoop 收敛、重复调用检测、权限门 deny 优先
// ============================================================================
class ScriptedProvider : public IModelProvider {
public:
    explicit ScriptedProvider(std::vector<ChatResponse> script)
        : script_(std::move(script)) {}

    std::string_view id() const override { return "scripted"; }
    int  contextWindow() const override { return 200'000; }
    bool supportsTools() const override { return true; }
    bool supportsStreaming() const override { return true; }
    bool supportsPromptCaching() const override { return true; }

    ChatResponse chat(const ChatRequest& req) override {
        return script_.empty() ? ChatResponse{} : script_.front();
    }

    ChatResponse stream(const ChatRequest&,
                        const std::function<bool(const ChatChunk&)>& onChunk) override {
        (void)onChunk;
        if (script_.empty()) return {};
        ChatResponse r = script_.front();
        script_.erase(script_.begin());
        return r;
    }

    int countTokens(std::string_view text) override {
        return int((text.size() + 3) / 4);
    }

private:
    std::vector<ChatResponse> script_;
};

ChatResponse makeToolCallResponse(const std::string& id, const std::string& name,
                                  const json& args) {
    ChatResponse r;
    r.message.role = Role::Assistant;
    ToolCall tc;
    tc.id   = id;
    tc.name = name;
    tc.args = args;
    r.message.toolCalls.push_back(std::move(tc));
    r.message.finishReason = FinishReason::ToolUse;
    r.finishReason = FinishReason::ToolUse;
    r.usage.inputTokens  = 40;
    r.usage.outputTokens = 8;
    return r;
}

ChatResponse makeFinalResponse(const std::string& text) {
    ChatResponse r;
    r.message.role    = Role::Assistant;
    r.message.content = text;
    r.usage.inputTokens  = 60;
    r.usage.outputTokens = 20;
    return r;
}

int runSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const std::string& name, const std::string& detail = {}) {
        if (ok) ++pass; else ++fail;
        std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << name;
        if (!detail.empty()) std::cout << "  —— " << detail;
        std::cout << "\n";
    };

    std::cout << "================================================\n"
                 "  L5 自检（不联网、不花钱）\n"
                 "================================================\n\n";

    // ---- T1 预算分配 ----
    std::cout << "[T1 预算分配]\n";
    {
        BudgetConfig b;
        const int avail = b.available();
        const int fixed = b.forLayer(Layer::L0_System) + b.forLayer(Layer::L1_Project) +
                          b.forLayer(Layer::L2_RepoMap) + b.forLayer(Layer::L3_Retrieved) +
                          b.forLayer(Layer::L4_ActiveFiles);
        const int hist = b.forLayer(Layer::L5_History) - fixed;
        check(fixed + hist == avail, "各层配额之和 == 可用量",
              "fixed=" + std::to_string(fixed) + " history=" + std::to_string(hist) +
              " available=" + std::to_string(avail));
        check(hist > 0, "历史层预算为正数");
    }

    // ---- T2 上下文测量 ----
    std::cout << "\n[T2 上下文测量]\n";
    {
        auto ui = std::make_shared<ConsoleUI>();
        BudgetConfig b;
        ContextAssembler asm_(b, ui);
        asm_.setSystem("你是一个编码 Agent。");
        asm_.setProjectKnowledge("# 项目知识\n使用 C++20 / MSVC。");
        asm_.setRepoMap("src/main.cpp\nsrc/agent_loop.cpp");

        Turn t;
        t.input = "读一下 main.cpp";
        Message f;
        f.role = Role::User;
        f.content = "int main() { return 0; }";
        t.activeFiles.push_back(f);
        Message rc;
        rc.role    = Role::User;
        rc.content = "src/main.cpp:1-3\nint main() { return 0; }";
        t.retrieved.push_back(rc);

        auto u = asm_.measure(t);
        check(u.l0 > 0 && u.l1 > 0 && u.l2 > 0, "L0/L1/L2 均有占用",
              "l0=" + std::to_string(u.l0) + " l1=" + std::to_string(u.l1) +
              " l2=" + std::to_string(u.l2));
        check(u.l3 > 0 && u.l4 > 0 && u.l6 > 0, "L3/L4/L6 均有占用");

        std::vector<ToolDef> defs(2);
        defs[0].name = "Read";
        defs[1].name = "Write";
        asm_.setTools(defs);
        ChatRequest req = asm_.assemble(t);
        check(req.tools.size() == 2 && req.tools[0].name == "Read", "工具表顺序稳定");
        check(!req.messages.empty() && req.messages.back().role == Role::User &&
                  req.messages.back().content == t.input,
              "L6 用户输入永远在最后");
        check(req.system.has_value() && !req.system->empty(), "system 已组装");
    }

    // ---- T3 消息守卫：孤儿 tool 被移除 ----
    std::cout << "\n[T3 消息守卫（孤儿 tool）]\n";
    {
        auto ui = std::make_shared<ConsoleUI>();
        ContextAssembler asm_(BudgetConfig{}, ui);
        ToolRegistry reg;
        registerReadTool(reg);
        std::string notices;
        ScriptedProvider sp({makeFinalResponse("没有任何工具需要调用")});
        AgentLoop loop(sp, reg, asm_);

        Turn t;
        t.input = "修复历史测试";
        Message orphan;
        orphan.role       = Role::Tool;
        orphan.toolCallId = "call_missing";
        orphan.content    = "孤儿结果";
        t.history.push_back(orphan);   // 非法历史：tool 没有对应的 assistant

        LoopHooks hooks;
        hooks.onNotice = [&notices](const std::string& m) { notices += m; };
        const LoopOutcome o = loop.run(t, hooks);
        check(o == LoopOutcome::Completed, "非法历史下循环不崩",
              std::string("outcome=") + std::string(toString(o)));
        std::size_t toolMsgs = 0;
        for (const auto& m : t.history) if (m.role == Role::Tool) ++toolMsgs;
        check(toolMsgs == 0, "守卫移除了孤儿 tool 消息",
              "剩余 tool 消息=" + std::to_string(toolMsgs));
        check(!notices.empty(), "守卫发出了修复通知", notices);
    }

    // ---- T4 AgentLoop 收敛 + 工具执行 ----
    std::cout << "\n[T4 AgentLoop 收敛]\n";
    {
        const std::string target = "af_selftest_target.txt";
        {
            std::ofstream ofs(target, std::ios::binary | std::ios::trunc);
            ofs << "int add(int a, int b) { return a + b; }\n";
        }

        auto ui = std::make_shared<ConsoleUI>();
        ContextAssembler asm_(BudgetConfig{}, ui);
        asm_.setSystem("你是一个编码 Agent。");
        ToolRegistry reg;
        registerReadTool(reg);
        asm_.setTools(reg.defs());

        ScriptedProvider sp({
            makeToolCallResponse("call_1", "Read", json{{"path", target}}),
            makeFinalResponse("文件里有一个函数 add。"),
        });
        AgentLoop loop(sp, reg, asm_);

        Turn t;
        t.input = "af_selftest_target.txt 里有哪些函数？";
        LoopHooks hooks;
        std::string notices;
        hooks.onNotice = [&notices](const std::string& m) { notices += m; };
        const LoopOutcome o = loop.run(t, hooks);
        check(o == LoopOutcome::Completed, "两轮后收敛（模型不再要工具）",
              std::string("outcome=") + std::string(toString(o)));
        check(t.finalText.find("add") != std::string::npos, "最终回答包含工具读到的事实",
              "finalText=" + t.finalText);

        bool sawTool = false, sawResult = false;
        for (const auto& m : t.history) {
            if (m.hasToolCalls()) sawTool = true;
            if (m.role == Role::Tool) sawResult = true;
            if (sawTool && sawResult) break;
        }
        check(sawTool && sawResult, "assistant.tool_calls 与 tool 结果都写回历史");

        std::size_t toolMsgs = 0;
        for (const auto& m : t.history) if (m.role == Role::Tool) ++toolMsgs;
        check(toolMsgs == 1, "恰好一次工具执行",
              "tool 消息=" + std::to_string(toolMsgs));
        check(loop.spentTokens() > 0, "token 记账生效",
              "spent=" + std::to_string(loop.spentTokens()));
        std::remove(target.c_str());
    }

    // ---- T5 重复调用检测（不阻断，但给警告）----
    std::cout << "\n[T5 重复调用检测]\n";
    {
        auto ui = std::make_shared<ConsoleUI>();
        ContextAssembler asm_(BudgetConfig{}, ui);
        ToolRegistry reg;
        registerReadTool(reg);
        asm_.setTools(reg.defs());

        // 连续三次相同调用 → 检测到重复（注意脚本只有 3 次，之后返回空响应）
        ScriptedProvider sp({
            makeToolCallResponse("call_a", "Read", json{{"path", "x.txt"}}),
            makeToolCallResponse("call_b", "Read", json{{"path", "x.txt"}}),
            makeToolCallResponse("call_c", "Read", json{{"path", "x.txt"}}),
        });
        LoopLimits lim;
        lim.maxSteps = 3;
        lim.maxRepeatedCalls = 2;
        AgentLoop loop(sp, reg, asm_, lim);

        Turn t;
        t.input = "重复调用测试";
        std::string notices;
        LoopHooks hooks;
        hooks.onNotice = [&notices](const std::string& m) { notices += m; };
        const LoopOutcome o = loop.run(t, hooks);
        check(notices.find("重复调用") != std::string::npos, "重复调用给出警告",
              notices.empty() ? "无通知" : notices);
        (void)o;
    }

    // ---- T6 权限门：deny 永远赢 ----
    std::cout << "\n[T6 权限门 deny 优先]\n";
    {
        ToolRegistry reg;
        std::vector<PermissionRule> rules{
            {DecisionKind::Allow, "*", "全部放行"},          // 故意放最前
            {DecisionKind::Deny,  "Bash(rm -rf *)", "危险删除"},
        };
        reg.setGate(std::make_shared<DefaultPermissionGate>(std::move(rules)));
        registerBashTool(reg, nullptr, {});

        ToolCall call;
        call.id   = "call_rm";
        call.name = "Bash";
        call.args = json{{"command", "rm -rf /"}};
        NoUI ui;
        const ToolResult tr = reg.dispatch(call, ui);
        check(tr.isError, "deny 规则覆盖了 allow *（deny 永远赢）");
        check(tr.content.find("被策略拒绝") != std::string::npos, "拒绝原因回喂给模型", tr.content);
    }

    // ---- T7 沙箱：能力自检 + 退出码 + 超时强杀 ----
    std::cout << "\n[T7 沙箱（Job Object）]\n";
    {
        SandboxConfig scfg;
        scfg.workspace = ".";
        auto sb = createSandbox(scfg);

        std::string report;
        check(sb->selfCheck(report), "沙箱能力自检可用");
        check(report.find("Job Object") != std::string::npos, "如实报告实现方式（Job Object）");
        check(report.find("未强制") != std::string::npos, "如实报告能力边界（未强制项）");

        const ExecResult ok = sb->exec("echo sandbox-ok", ".", std::chrono::seconds(10));
        check(ok.exitCode == 0 && ok.stdoutOut.find("sandbox-ok") != std::string::npos,
              "正常命令执行并捕获输出", "exit=" + std::to_string(ok.exitCode));

        const ExecResult bad = sb->exec("exit 3", ".", std::chrono::seconds(10));
        check(bad.exitCode == 3, "退出码进结果", "exit=" + std::to_string(bad.exitCode));

        const ExecResult slow = sb->exec("ping -n 5 127.0.0.1 >nul", ".",
                                         std::chrono::milliseconds(300));
        check(slow.timedOut, "超时命令被强杀（进程树）");
        check(slow.blockedBySandbox, "超时被判定为沙箱拦截（区别于命令错误）");
    }

    // ---- T8 写路径围栏（纵深防御）----
    std::cout << "\n[T8 写路径围栏]\n";
    {
        ToolRegistry reg;
        registerWriteTool(reg, ".");          // 只允许写当前目录
        NoUI ui;

        ToolCall outCall;
        outCall.id   = "call_out";
        outCall.name = "Write";
        outCall.args = json{{"path", "..\\af_jail_test.txt"}, {"content", "x"}};
        const ToolResult tr = reg.dispatch(outCall, ui);
        check(tr.isError && tr.content.find("越界") != std::string::npos,
              "工作目录外的写被拒绝", tr.content);

        ToolCall inCall;
        inCall.id   = "call_in";
        inCall.name = "Write";
        inCall.args = json{{"path", "af_jail_ok.txt"}, {"content", "ok"}};
        const ToolResult okTr = reg.dispatch(inCall, ui);
        check(!okTr.isError, "工作目录内的写被允许", okTr.content);
        std::remove("af_jail_ok.txt");
    }

    // ---- T9 仓库索引（词法级符号 + 依赖边 + 检索）----
    std::cout << "\n[T9 仓库索引]\n";
    {
        namespace fsx = std::filesystem;
        const std::string dir = "af_idx_selftest";
        std::error_code ec;
        fsx::remove_all(dir, ec);
        fsx::create_directories(dir, ec);

        {
            std::ofstream f(dir + "/calc.hpp");
            f << "#pragma once\nclass Calc {\npublic:\n    int add(int a, int b);\n    int mul(int a, int b);\n};\n";
        }
        {
            std::ofstream f(dir + "/calc.cpp");
            f << "#include \"calc.hpp\"\nint Calc::add(int a, int b) { return a + b; }\nint Calc::mul(int a, int b) { return a * b; }\n";
        }
        {
            std::ofstream f(dir + "/app.cpp");
            f << "#include \"calc.hpp\"\nint main() { Calc c; return c.add(1, 2); }\n";
        }

        RepoIndex idx;
        check(idx.build(dir), "索引构建成功");
        check(idx.stats().find("3 个文件") != std::string::npos, "统计正确", idx.stats());

        const auto hits = idx.retrieve("add", 3);
        bool found = false;
        for (const auto& h : hits)
            if (h.path == "calc.cpp" || h.path == "calc.hpp") found = true;
        check(found, "检索到 add 的定义片段",
              hits.empty() ? "无命中" : hits.front().path);

        const auto edges = idx.dependencyEdges();
        bool hasEdge = false;
        for (const auto& e : edges)
            if (e == "calc.cpp -> calc.hpp") hasEdge = true;
        check(hasEdge, "依赖边：#include 被索引");

        fsx::remove_all(dir, ec);
    }

    // ---- T10 会话持久化（SQLite）----
    std::cout << "\n[T10 会话持久化]\n";
    {
        const std::string dbPath = "af_selftest_session.db";
        std::remove(dbPath.c_str());

        SessionStore store;
        check(store.open(dbPath), "SQLite 打开并建表", store.lastError());

        std::vector<Message> msgs;
        Message u;
        u.role = Role::User; u.content = "你好";
        Message a;
        a.role = Role::Assistant; a.content = "hi";
        ToolCall tc;
        tc.id = "call_1"; tc.name = "Read"; tc.args = json{{"path", "x"}};
        a.toolCalls.push_back(tc);
        a.finishReason = FinishReason::ToolUse;
        Message t;
        t.role = Role::Tool; t.toolCallId = "call_1"; t.content = "文件内容";
        msgs = {u, a, t};

        check(store.appendMessages("s1", msgs, 0), "写入会话消息", store.lastError());
        const auto back = store.loadHistory("s1");
        check(back.size() == 3, "读回 3 条消息",
              "读回=" + std::to_string(back.size()));
        check(back.size() == 3 && back[1].toolCalls.size() == 1 &&
                  back[2].toolCallId == "call_1",
              "工具字段（tool_calls / tool_call_id）完整");
        const auto list = store.listSessions();
        check(!list.empty() && list[0].first == "s1", "会话列表");

        store.close();
        std::remove(dbPath.c_str());
    }

    // ---- T11 密钥加密（DPAPI）----
    std::cout << "\n[T11 DPAPI 密钥]\n";
    {
        const std::string f = "af_selftest_secret.bin";
        std::remove(f.c_str());

        check(saveSecretFile(f, "sk-test-123"), "DPAPI 加密保存");
        check(loadSecretFile(f) == "sk-test-123", "DPAPI 解密还原");

        std::ifstream ifs(f, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        check(content.find("sk-test-123") == std::string::npos,
              "密钥文件里没有明文（只存密文）");
        check(unprotectSecret(content) == "sk-test-123", "密文可被正确解密");

        clearSecretFile(f);
    }

    // ---- T12 配置：文件 → 环境变量覆盖 ----
    std::cout << "\n[T12 配置加载]\n";
    {
        namespace fsx2 = std::filesystem;
        const std::string dir = "af_cfg_selftest";
        std::error_code ec;
        fsx2::remove_all(dir, ec);
        fsx2::create_directories(dir, ec);
        {
            std::ofstream f(dir + "/af_config.json");
            f << R"({"provider":{"model":"cfg-model","baseUrl":"http://cfg/v1"}})";
        }

        const AppConfig c1 = loadConfig(dir);
        check(c1.provider.model == "cfg-model", "配置文件生效",
              c1.provider.model);

        _putenv_s("AF_MODEL", "env-model");
        const AppConfig c2 = loadConfig(dir);
        check(c2.provider.model == "env-model", "环境变量覆盖配置文件",
              c2.provider.model);
        _putenv_s("AF_MODEL", "");

        fsx2::remove_all(dir, ec);
    }

    std::cout << "\n================================================\n";
    std::cout << "  结果：" << pass << " 通过 / " << fail << " 失败\n";
    std::cout << "================================================\n";
    return fail == 0 ? 0 : 1;
}

}  // namespace

// ============================================================================
//  main
// ============================================================================
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::vector<std::string> args;
#ifdef _WIN32
    args = winUtf8Args();
#else
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
#endif

    // ---- --config / --set-key / --clear-key / --sessions：配置与持久化元命令 ----
    if (args.size() >= 2 && args[1] == "--config") {
        std::cout << configToJson(loadConfig(".")) << "\n";
        return 0;
    }
    if (args.size() >= 3 && args[1] == "--set-key") {
        AppConfig c = loadConfig(".");
        const bool ok = saveSecretFile(c.workspace + "/" + c.secretFile, sanitizeUtf8(args[2]));
        std::cout << (ok ? "已用 DPAPI 加密保存密钥" : "保存失败") << "\n";
        return ok ? 0 : 1;
    }
    if (args.size() >= 2 && args[1] == "--clear-key") {
        AppConfig c = loadConfig(".");
        clearSecretFile(c.workspace + "/" + c.secretFile);
        std::cout << "密钥文件已清除\n";
        return 0;
    }
    if (args.size() >= 2 && args[1] == "--sessions") {
        SessionStore store;
        if (!store.open(".af-sessions/sessions.db")) {
            std::cerr << "无法打开会话库: " << store.lastError() << "\n";
            return 1;
        }
        for (const auto& [id, ts] : store.listSessions())
            std::cout << id << "  " << ts << "\n";
        return 0;
    }

    // ---- --index-stats / --index <query>：索引检查与检索演示 ----
    if (args.size() >= 2 && args[1] == "--index-stats") {
        RepoIndex idx;
        if (!idx.build(".")) { std::cerr << "当前目录没有可索引的源码\n"; return 1; }
        std::cout << idx.stats() << "\n";
        for (const auto& e : idx.dependencyEdges()) std::cout << "  " << e << "\n";
        return 0;
    }
    if (args.size() >= 3 && args[1] == "--index") {
        RepoIndex idx;
        if (!idx.build(".")) { std::cerr << "当前目录没有可索引的源码\n"; return 1; }
        const std::string q = sanitizeUtf8(args[2]);
        const auto chunks = idx.retrieve(q, 5, 40);
        for (const auto& c : chunks)
            std::cout << "== " << c.path << ":" << c.startLine << "-" << c.endLine
                      << " score=" << c.score << " ==\n" << c.content << "\n";
        return chunks.empty() ? 2 : 0;
    }

    // ---- --sandbox-check：沙箱能力自检 ----
    if (args.size() >= 2 && args[1] == "--sandbox-check") {
        SandboxConfig scfg;
        scfg.workspace = ".";
        auto sb = createSandbox(scfg);
        std::string report;
        const bool ok = sb->selfCheck(report);
        std::cout << report << "\n";
        return ok ? 0 : 1;
    }

    // ---- --selftest：进程内自检 ----
    if (args.size() >= 2 && (args[1] == "--selftest" || args[1] == "-t")) {
        return runSelfTest();
    }

    if (args.size() < 2) {
        std::cerr << "用法: agent.exe \"<任务描述>\"\n"
                     "      agent.exe --selftest | --sandbox-check | --config\n"
                     "      agent.exe --index-stats | --index \"<query>\"\n"
                     "      agent.exe --set-key <key> | --clear-key | --sessions\n"
                     "      agent.exe --resume <session> \"<任务描述>\"\n";
        return 1;
    }

    // ---- 任务与配置（模块 ⑩：文件 + 环境变量 + DPAPI 密钥）----
    std::string task;
    std::string sessionOverride;
    if (args[1] == "--resume" && args.size() >= 4) {
        sessionOverride = args[2];
        task = sanitizeUtf8(args[3]);
    } else {
        task = sanitizeUtf8(args[1]);
    }

    AppConfig appCfg = loadConfig(".");
    if (!sessionOverride.empty()) appCfg.sessionId = sessionOverride;
    ProviderConfig& pc = appCfg.provider;

    const bool isLocal = pc.baseUrl.find("127.0.0.1") != std::string::npos ||
                         pc.baseUrl.find("localhost") != std::string::npos;
    if (pc.apiKey.empty() && !isLocal) {
        std::cerr << "请设置 OPENAI_API_KEY 或运行 agent.exe --set-key <key>\n";
        return 1;
    }
    std::cerr << "[配置] model=" << pc.model << " base=" << pc.baseUrl
              << " session=" << appCfg.sessionId << "\n";

    // ---- 结构化日志 ----
    Logger::instance().setFile(appCfg.logPath);
    Logger::instance().info("main", "start",
                            json{{"model", pc.model}, {"task", task},
                                 {"session", appCfg.sessionId}});

    // ---- 索引（模块 ③）：词法级符号 + 依赖 + 仓库地图 ----
    RepoIndex index;
    if (index.build(appCfg.workspace)) {
        std::cerr << "[索引] " << index.stats() << "\n";
        Logger::instance().info("index", "built",
                                json{{"files", index.files().size()}});
    } else {
        std::cerr << "[索引] 当前目录没有可索引的源码\n";
    }

    // ---- 工具表 + 沙箱 + 权限门 + UI ----
    ToolRegistry registry;
    registerReadTool(registry);
    registerWriteTool(registry, appCfg.workspace);
    registerEditTool(registry, appCfg.workspace);
    SandboxConfig sandboxCfg = appCfg.sandbox;
    sandboxCfg.workspace = appCfg.workspace;
    auto sandbox = createSandbox(sandboxCfg);
    registerBashTool(registry, sandbox, appCfg.workspace);

    if (!appCfg.pluginPath.empty()) {
        std::string err;
        if (registry.loadPlugin(appCfg.pluginPath, err))
            std::cerr << "[插件] 已加载 " << appCfg.pluginPath << "\n";
        else
            std::cerr << "[插件] 加载失败: " << err << "\n";
    }

    {
        std::vector<PermissionRule> rules{
            {DecisionKind::Deny,  "Bash(rm -rf *)", "删除类命令一律拒绝"},
            {DecisionKind::Allow, "Read",           "只读操作直接放行"},
        };
        // fallback = Ask：Write / Edit / Bash 等都要人批准（fail-closed）
        registry.setGate(std::make_shared<DefaultPermissionGate>(std::move(rules)));
    }

    auto ui = std::make_shared<ConsoleUI>();

    // ---- 上下文组装（分层 + 预算 + 降级链）----
    ContextAssembler assembler(appCfg.budget, ui);
    assembler.setSystem(
        "你是一个编码 Agent。原则：\n"
        "1. 需要文件内容时必须**调用工具去读**，不要凭猜测回答。\n"
        "2. 工具报错是信息不是失败 —— 读它，然后换方法。\n"
        "3. 回答要给出依据（哪个文件、哪几行）。");
    assembler.setTools(registry.defs());
    assembler.setRepoMap(index.repoMap());

    // ---- 长期记忆（模块 ⑫ 的 L1 层）：CLAUDE.md / MEMORY.md 注入项目知识 ----
    for (const char* mem : {"CLAUDE.md", "MEMORY.md"}) {
        std::ifstream ifs((appCfg.workspace + "/" + mem).c_str(), std::ios::binary);
        if (!ifs) continue;
        std::ostringstream os;
        os << ifs.rdbuf();
        assembler.setProjectKnowledge(os.str());
        std::cerr << "[记忆] 已加载 " << mem << "（" << os.str().size() << " 字节）\n";
        break;
    }

    // ---- 起循环 ----
    auto provider = createProvider(pc);
    AgentLoop loop(*provider, registry, assembler, appCfg.limits);

    Turn turn;
    turn.input = task;

    // ---- 会话恢复（模块 ⑫）：从 SQLite 读回本会话历史 ----
    SessionStore store;
    {
        std::error_code ec;
        std::filesystem::create_directories(".af-sessions", ec);   // SQLite 不会建目录
    }
    const bool sessionOk = store.open(".af-sessions/sessions.db");
    std::size_t historyStart = 0;
    if (sessionOk) {
        store.ensureSession(appCfg.sessionId, task.substr(0, 40));
        turn.history = store.loadHistory(appCfg.sessionId);
        historyStart = turn.history.size();
        if (!turn.history.empty())
            std::cerr << "[恢复] 会话 " << appCfg.sessionId << " 历史 "
                      << turn.history.size() << " 条\n";
    } else {
        std::cerr << "[会话] 无法打开会话库: " << store.lastError() << "\n";
    }

    // ---- 检索（W1 上下文闭环的检索端）：把最相关的代码放进 L3 ----
    if (!index.empty()) {
        const auto chunks = index.retrieve(task, 3, 40);
        for (const auto& c : chunks) {
            Message m;
            m.role = Role::User;
            m.content = "[检索命中] " + c.path + ":" + std::to_string(c.startLine) + "-" +
                        std::to_string(c.endLine) + "\n" + c.content;
            turn.retrieved.push_back(std::move(m));
        }
        if (!chunks.empty())
            std::cerr << "[检索] 命中 " << chunks.size() << " 个片段\n";
    }

    Renderer renderer;
    std::size_t toolCalls = 0;
    LoopHooks hooks;
    hooks.onChunk = [&renderer](const ChatChunk& c) { renderer(c); };
    hooks.onToolStart = [&toolCalls](const ToolCall& c) {
        ++toolCalls;
        Logger::instance().info("loop", "tool-start",
                                json{{"name", c.name}, {"args", c.args.dump()}});
        std::cout << "\n[执行工具] " << c.name << " " << c.args.dump() << "\n" << std::flush;
    };
    hooks.onToolDone = [](const ToolResult& r) {
        std::string preview = r.content.substr(0, 300);
        if (r.content.size() > 300) preview += " ...";
        std::cout << (r.isError ? "[工具失败] " : "[工具返回] ") << preview << "\n\n"
                  << std::flush;
    };
    hooks.onNotice = [ui](const std::string& m) { ui->notice(m); };
    hooks.confirm = [ui](const std::string& prompt) { return ui->confirm(prompt); };
    hooks.onError  = [](const std::string& e) {
        Logger::instance().error("loop", "provider-error", json{{"message", e}});
        std::cerr << "\n[错误] " << e << "\n";
    };

    const LoopOutcome outcome = loop.run(turn, hooks);

    Logger::instance().info("main", "done",
                            json{{"outcome", std::string(toString(outcome))},
                                 {"toolCalls", toolCalls},
                                 {"spent", loop.spentTokens()},
                                 {"history", turn.history.size()}});

    std::cout << "\n===== " << toString(outcome)
              << " | toolCalls=" << toolCalls
              << " | spent=" << loop.spentTokens() << " tokens"
              << " | history=" << turn.history.size() << " msgs =====\n";

    // ---- 会话保存（模块 ⑫）：增量保存本轮新增消息 ----
    if (sessionOk) {
        if (store.appendMessages(appCfg.sessionId, turn.history, historyStart))
            std::cerr << "[会话] 已保存 " << (turn.history.size() - historyStart)
                      << " 条消息到会话库\n";
        else
            std::cerr << "[会话] 保存失败: " << store.lastError() << "\n";
    }

    if (outcome == LoopOutcome::Completed && !turn.finalText.empty()) {
        std::cout << "\n[最终回答]\n" << turn.finalText << "\n";
    }
    return outcome == LoopOutcome::Completed ? 0 : 2;
}



