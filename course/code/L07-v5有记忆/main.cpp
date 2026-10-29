// ---------------------------------------------------------------------------
//  L7 · main.cpp —— v5 组合根（Composition Root）：把"记忆"接上去
//
//  v4（L6）的 main 把刹车接完了。本讲 main 的变化仍然**只有接线**，
//  但多了两个"选择题"，它们都要靠环境变量来做（这是 I-14 的临时口径）：
//
//    ① 上下文窗口多大（AF_CONTEXT_WINDOW）—— 决定压缩什么时候触发。
//       验证脚本正是靠它把"50 个文件"压缩到几百 token 的窗口里去演示。
//    ② 摘要器用谁（AF_COMPACT = llm | local | off）——
//         llm   ：调模型写交接摘要（真实路径，多花一次请求）
//         local ：本地抽取式摘要（零成本、确定、可离线验证）
//         off   ：完全不压缩（回到 L6 的行为）
//
//  ★ 为什么"摘要器"必须可换：因为**验证**。
//    如果摘要只能走模型，那么"压缩"这条最重要的链路就永远需要一个
//    API Key 才能测 —— 参考实现的验证报告里那句
//    "压缩：有实现，但没在真实长会话里跑过"，根因就在这里。
//    可换的摘要器让压缩在**没有网络**时也能被逐级验证。
//
//  用法：
//      agent.exe "把 work_target.txt 里的 add 改名为 sum，然后跑测试确认"
//      agent.exe --selftest
//      agent.exe --sandbox-info
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
#  include <shellapi.h>          // CommandLineToArgvW
#  pragma comment(lib, "shell32.lib")
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agent_loop.hpp"
#include "compaction.hpp"
#include "context_budget.hpp"
#include "model_provider.hpp"
#include "sandbox.hpp"
#include "tool_registry.hpp"
#include "types.hpp"

namespace fs = std::filesystem;
using namespace af;

namespace {

// ---------------------------------------------------------------------------
//  闸门 6 的"源头"：一个全局的取消信号
//
//  ★ 为什么是全局的：Windows 的 Ctrl-C 处理函数是 C 回调，
//    **不能捕获局部变量**（没有 this/闭包）。所以取消信号必须能全局访问。
//    这不是"随手用全局变量"，而是平台约束下的必然形状。
// ---------------------------------------------------------------------------
std::stop_source g_stop;

#ifdef _WIN32
BOOL WINAPI onConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        g_stop.request_stop();
        // ★ 返回 TRUE = "我处理了，别立刻杀我"。
        //   返回 FALSE 的话进程被就地终止，循环没机会收尾
        //   （已经写出去的文件、已经产生的 token 都白费）。
        return TRUE;
    }
    return FALSE;
}
#endif

std::string envUtf8(const char* key, const std::string& def);

// ★ K-7 又一次：`fs::path::string()` 在 Windows 上返回的是 **ANSI 代码页**
//   字节（中文机 = GBK），不是 UTF-8。把它当 UTF-8 传下去，路径就废了。
//   必须走 `u8string()`。
std::string pathUtf8(const fs::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

#ifdef _WIN32
// Windows 的 argv 是 ANSI 代码页（中文机 = GBK），不是 UTF-8。
// 直接塞进 JSON 会让 nlohmann 的 dump() 抛 type_error.316，而报错指不到根因。
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
#else
std::string envUtf8(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? sanitizeUtf8(v) : def;
}
#endif

int envInt(const char* key, int def) {
    const std::string s = envUtf8(key, "");
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

// ---------------------------------------------------------------------------
//  端口 ④ 的朴素实现：终端
// ---------------------------------------------------------------------------
class ConsoleUI : public IUIPort {
public:
    bool confirm(const std::string& prompt) override {
        std::cout << "\n[权限] " << prompt << "\n  允许? [y/N] " << std::flush;
        std::string line;
        // ★ 拿不到输入 → 返回 false。没有明确批准就不执行（fail-closed）。
        if (!std::getline(std::cin, line)) return false;
        return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
    }
    void notice(const std::string& msg) override {
        std::cout << "[通知] " << msg << "\n" << std::flush;
    }
};

// ===========================================================================
//  内置工具：Read / Write / Edit / Bash
// ===========================================================================

std::string readFileWithLineNumbers(const std::string& path, int maxLines = 2000) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream os;
    std::string line;
    int n = 0;
    while (std::getline(ifs, line) && n < maxLines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        os << ++n << "\t" << line << "\n";
    }
    return os.str();
}

void registerReadTool(ToolRegistry& reg) {
    ToolDef def;
    def.name = "Read";
    def.description =
        "读取文件内容，返回带行号的文本。用于查看代码、配置或日志。"
        "不要用它读超过 2000 行的大文件 —— 先用搜索类工具定位范围。";
    def.inputSchema = json::object({
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"},
                                  {"description", "文件的绝对路径或相对路径"}}}}},
        {"required", json::array({"path"})},
    });
    def.risk = RiskLevel::Read;
    def.persistThresholdChars = SIZE_MAX;     // 豁免落盘（正确性要求，见 L5）

    reg.registerBuiltin(def, [](const json& args) -> ToolResult {
        const std::string path = args.value("path", std::string());
        if (path.empty()) return ToolResult::error("", "path 不能为空");
        std::string content = readFileWithLineNumbers(path);
        if (content.empty()) return ToolResult::error("", "无法读取文件（不存在或为空）: " + path);
        return ToolResult::ok("", std::move(content));
    });
}

void registerWriteTool(ToolRegistry& reg) {
    ToolDef def;
    def.name = "Write";
    def.description =
        "把完整内容写入文件（覆盖已存在的文件）。用于新建文件或整体重写小文件。"
        "★ 覆盖现有文件之前必须先 Read 确认内容，否则你会丢掉没读到的部分。";
    def.inputSchema = json::object({
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"}}},
                        {"content", {{"type", "string"}}}}},
        {"required", json::array({"path", "content"})},
    });
    def.risk = RiskLevel::Write;

    reg.registerBuiltin(def, [](const json& args) -> ToolResult {
        const std::string path    = args.value("path", std::string());
        const std::string content = args.value("content", std::string());
        if (path.empty()) return ToolResult::error("", "path 不能为空");
        std::error_code ec;
        if (fs::path(path).has_parent_path())
            fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) return ToolResult::error("", "无法写入文件: " + path);
        ofs.write(content.data(), std::streamsize(content.size()));
        if (!ofs) return ToolResult::error("", "写入失败（磁盘满或权限不足）: " + path);
        return ToolResult::ok("", "已写入 " + std::to_string(content.size()) +
                                   " 字节到 " + path);
    });
}

void registerEditTool(ToolRegistry& reg) {
    ToolDef def;
    def.name = "Edit";
    def.description =
        "用精确的字符串替换修改文件：把 old_str 换成 new_str。"
        "old_str 必须在文件中唯一出现，否则会失败 —— 这时请带上更多上下文重试。"
        "★ 这是最省 token 的改法，优先用它而不是整文件重写。";
    def.inputSchema = json::object({
        {"type", "object"},
        {"properties", {{"path",    {{"type", "string"}}},
                        {"old_str", {{"type", "string"},
                                     {"description", "要被替换的原文，必须在文件中唯一"}}},
                        {"new_str", {{"type", "string"}}}}},
        {"required", json::array({"path", "old_str", "new_str"})},
    });
    def.risk = RiskLevel::Write;

    reg.registerBuiltin(def, [](const json& args) -> ToolResult {
        const std::string path   = args.value("path", std::string());
        const std::string oldStr = args.value("old_str", std::string());
        const std::string newStr = args.value("new_str", std::string());
        if (path.empty() || oldStr.empty())
            return ToolResult::error("", "path / old_str 不能为空");

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return ToolResult::error("", "无法打开文件: " + path);
        std::ostringstream os; os << ifs.rdbuf();
        std::string src = os.str();

        const std::size_t pos = src.find(oldStr);
        if (pos == std::string::npos)
            return ToolResult::error("", "old_str 未找到。请用 Read 重新确认文件内容。");
        if (src.find(oldStr, pos + 1) != std::string::npos)
            return ToolResult::error("",
                "old_str 出现多次，不够唯一。请把它周围更多的行一起写进 old_str。");

        src.replace(pos, oldStr.size(), newStr);
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) return ToolResult::error("", "无法写回文件: " + path);
        ofs.write(src.data(), std::streamsize(src.size()));
        return ToolResult::ok("", "已修改 " + path + "（替换 " +
                                   std::to_string(oldStr.size()) + " → " +
                                   std::to_string(newStr.size()) + " 字符）");
    });
}

// ---------------------------------------------------------------------------
//  Bash —— 本讲**唯一的"实现替换"**
//
//  ★ 注意下面这行注释有多重要：
//    lambda 的**签名 `(const json&) -> ToolResult` 和它的位置都没变**，
//    变的只是它内部从 `_popen` 换成了 `ISandbox::exec()`。
//    这就是端口与适配器（ADR-4）兑现的样子 ——
//    "换掉执行环境"不需要改动调用方，甚至不需要改动工具表。
//
//  L5 的版本是裸 `_popen`：没有网络隔离、没有写权限限制、没有超时。
//  L5 的讲义把它写成**遗留 I-12**，本讲关闭。
// ---------------------------------------------------------------------------
void registerBashTool(ToolRegistry& reg, std::shared_ptr<ISandbox> sandbox,
                      int timeoutMs) {
    ToolDef def;
    def.name = "Bash";
    def.description =
        "执行一条 shell 命令：跑测试、构建、查找、查看文件。"
        "命令的退出码会一并返回 —— 非 0 表示命令自己失败了，不是工具坏了。"
        "★ 命令在沙箱内执行：默认禁网、限进程数与内存、超时会连同子进程一起强杀。"
        "被沙箱拦截时会明确告诉你原因 —— 那种情况下重试没有意义。";
    def.inputSchema = json::object({
        {"type", "object"},
        {"properties", {{"command", {{"type", "string"},
                                     {"description", "要执行的命令，例如 findstr /C:sum a.txt"}}}}},
        {"required", json::array({"command"})},
    });
    def.risk = RiskLevel::Exec;
    def.timeoutMs = timeoutMs;

    reg.registerBuiltin(def, [sandbox, timeoutMs](const json& args) -> ToolResult {
        const std::string cmd = args.value("command", std::string());
        if (cmd.empty()) return ToolResult::error("", "command 不能为空");
        if (!sandbox) return ToolResult::error("", "沙箱未装配（拒绝执行，fail-closed）");

        std::error_code ec;
        const std::string cwd = pathUtf8(fs::current_path(ec));

        ExecResult er = sandbox->exec(cmd, cwd, Ms(timeoutMs));

        // ★ 被拦 / 超时，都必须说清楚"这不是命令写错了"
        if (er.blockedBySandbox || er.timedOut)
            return ToolResult::error("", sandboxBlockedMessage(er));

        std::string out = er.stdoutOut;
        if (!er.stderrOut.empty()) out += "\n[stderr] " + er.stderrOut;
        // ★ 退出码必须进结果。把"非 0"当成功返回空输出，
        //   会让模型得到"文件里没有"这个**错误结论**。
        if (er.exitCode != 0)
            return ToolResult::error("", out + "\n[exit code " +
                                          std::to_string(er.exitCode) + "]");
        if (out.empty()) out = "(命令成功，无输出)";
        return ToolResult::ok("", std::move(out));
    });
}

void registerBuiltinTools(ToolRegistry& reg, std::shared_ptr<ISandbox> sandbox,
                          int cmdTimeoutMs) {
    registerReadTool(reg);
    registerWriteTool(reg);
    registerEditTool(reg);
    registerBashTool(reg, std::move(sandbox), cmdTimeoutMs);
}

// ===========================================================================
//  L7 自检用的脚手架
// ===========================================================================

// 造一段"读了 N 个文件"的历史：user(需求) + N × [assistant(Read) + tool(结果)]
std::vector<Message> makeReadHistory(const std::string& requirement, int n,
                                     std::size_t charsPerResult) {
    std::vector<Message> h;
    Message u;
    u.role    = Role::User;
    u.content = requirement;
    h.push_back(std::move(u));

    for (int i = 0; i < n; ++i) {
        Message a;
        a.role = Role::Assistant;
        a.toolCalls.push_back(ToolCall{"call_read_" + std::to_string(i), "Read",
                                       json{{"path", "src/file_" + std::to_string(i) + ".cpp"}}});
        h.push_back(std::move(a));

        Message t;
        t.role       = Role::Tool;
        t.toolCallId = "call_read_" + std::to_string(i);
        t.content    = "1\t// file_" + std::to_string(i) + ".cpp\n" +
                       std::string(charsPerResult, 'x');
        h.push_back(std::move(t));
    }
    return h;
}

// 一个"总是失败"的摘要器 —— 用来验证 CR-2：失败是返回值，不是异常。
struct ThrowingSummarizer final : ISummarizer {
    SummaryResult summarize(const SummarizeRequest&) override {
        throw std::runtime_error("模拟：摘要服务不可用");
    }
};

// 数一数一段历史里有多少**孤儿 tool_result**（引用的 id 找不到对应 assistant）
int countOrphanToolResults(const std::vector<Message>& h) {
    int orphan = 0;
    std::set<std::string> waiting;
    for (const auto& m : h) {
        if (m.role == Role::Assistant && !m.toolCalls.empty()) {
            waiting.clear();
            for (const auto& tc : m.toolCalls) waiting.insert(tc.id);
        } else if (m.isToolResult()) {
            if (waiting.count(m.toolCallId)) waiting.erase(m.toolCallId);
            else ++orphan;
        } else {
            waiting.clear();
        }
    }
    return orphan;
}

// 一套"验证压缩"的通用配置。
// ★ 这里把前五层的分额全部置 0，于是 L5（历史）拿到**整个窗口** ——
//   这是为了让用例的预算可预期（生产默认是 27%，见 BudgetConfig）。
CompactionConfig compactCfgForTest(int window) {
    CompactionConfig cc;
    cc.budget.contextWindow   = window;
    cc.budget.reserveForReply = 0;
    cc.budget.maxOutputTokens = 0;
    cc.budget.sysFrac = cc.budget.projectFrac = cc.budget.repoMapFrac = 0.0;
    cc.budget.retrievedFrac = cc.budget.activeFrac = 0.0;
    cc.keepRecentTokens   = 300;
    cc.foldThresholdChars = 1'000'000;   // 默认关掉 L2（要测 L2 的用例自己开）
    cc.perMessageCapDiv   = 1;           // 默认关掉 L1（cap = 整个历史预算）
    cc.maxSummaryChars    = 4'000;
    return cc;
}

// ===========================================================================
//  --selftest：单元级自检（G8 消息合法性 + 组装三规矩 + L7 压缩级联）
// ===========================================================================
int runSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) ++pass; else ++fail;
        std::cout << "[自检] " << (ok ? "PASS" : "FAIL") << "  " << name << "\n";
    };

    auto user = [](std::string s) {
        Message m; m.role = Role::User; m.content = std::move(s); return m;
    };
    auto assistantWithTools = [](std::vector<std::string> ids) {
        Message m; m.role = Role::Assistant;
        for (auto& id : ids) m.toolCalls.push_back(ToolCall{id, "Read", json::object()});
        return m;
    };
    auto toolResult = [](std::string id) {
        Message m; m.role = Role::Tool; m.toolCallId = std::move(id); m.content = "ok";
        return m;
    };
    auto countTool = [](const ChatRequest& r) {
        int n = 0;
        for (const auto& m : r.messages) if (m.role == Role::Tool) ++n;
        return n;
    };

    auto ui = std::make_shared<ConsoleUI>();
    ContextAssembler asm_(BudgetConfig{}, ui);
    asm_.setSystem("你是编码 Agent。");
    asm_.setTools({ToolDef{.name = "Read"}, ToolDef{.name = "Write"},
                   ToolDef{.name = "Edit"}, ToolDef{.name = "Bash"}});

    std::cout << "===== L6 自检：消息合法性（G8）+ 组装三规矩 =====\n";

    {
        Turn t; t.input = "任务";
        t.history = { user("你好"), toolResult("call_x") };
        check(countTool(asm_.assemble(t)) == 0, "I-1 孤儿 tool 消息被丢弃（否则服务端 400）");
    }
    {
        Turn t; t.input = "任务";
        t.history = { user("你好"),
                      assistantWithTools({"call_a", "call_b"}),
                      toolResult("call_a"), toolResult("call_b") };
        check(countTool(asm_.assemble(t)) == 2,
              "I-2 并行 tool_call 的两条结果都保留（按 id 集合配对，不是看邻居）");
    }
    {
        Turn t; t.input = "任务";
        t.history = { user("你好"),
                      assistantWithTools({"call_a"}),
                      user("[系统] 模型请求失败，请继续"),
                      toolResult("call_a") };
        check(countTool(asm_.assemble(t)) == 0,
              "I-3 配对窗口被打断后，迟到的 tool 结果被丢弃");
    }
    {
        Turn t; t.input = "这是用户的原始任务";
        t.history = { user("旧消息") };
        const ChatRequest r = asm_.assemble(t);
        check(!r.messages.empty() && r.messages.back().role == Role::User &&
              r.messages.back().content == t.input,
              "I-4 L6 用户输入永远在 messages 末尾且完整");
    }
    {
        Turn t; t.input = "任务";
        const ChatRequest r = asm_.assemble(t);
        check(r.tools.size() == 4 && r.tools[0].name == "Read" &&
              r.tools[3].name == "Bash",
              "I-5 工具表原样下发，顺序与注册顺序一致（顺序即缓存）");
    }
    {
        Turn t; t.input = "任务";
        const ChatRequest r = asm_.assemble(t);
        check(r.system.has_value() && !r.system->empty(),
              "I-6 L0 系统提示非空（缓存前缀，字节级稳定）");
    }

    // =======================================================================
    //  ★ L7：预算、降级链、压缩级联、落盘
    // =======================================================================
    std::cout << "\n===== L7 自检：预算与降级链 / 压缩级联 / 落盘 =====\n";

    // ---- ① 降级链：全文 → 签名 → 符号名 → 首行 → 丢弃 ----
    {
        std::string code;
        for (int i = 0; i < 120; ++i) {
            code += "int func_" + std::to_string(i) + "(int a, int b) {\n";
            code += "    return a + b;  // 实现细节，应当被降级掉\n";
            code += "}\n";
        }
        const int tFull  = ContextAssembler::estimateTokens(code);
        const int tSig   = ContextAssembler::estimateTokens(
                               ContextAssembler::extractSignatures(code));
        const int tNames = ContextAssembler::estimateTokens(
                               ContextAssembler::extractSymbolNames(code));
        check(tFull > tSig && tSig > tNames,
              "I-7 降级链是单调的：全文 > 签名 > 符号名（每一级都更省）");

        Degrade d = Degrade::Full;
        ContextAssembler::fitTo(code, tFull - 1, d);
        check(d == Degrade::Signatures,
              "I-8 全文放不下 → 降到「签名」级");

        d = Degrade::Full;
        ContextAssembler::fitTo(code, tSig - 1, d);
        check(d == Degrade::SymbolNames,
              "I-9 签名仍放不下 → 降到「符号名」级");

        d = Degrade::Full;
        const std::string s3 = ContextAssembler::fitTo(code, tNames - 1, d);
        check((d == Degrade::PathOnly || d == Degrade::Drop) &&
                  ContextAssembler::estimateTokens(s3) <= std::max(1, tNames - 1),
              "I-10 ★ 符号名仍放不下 → 「首行」或「丢弃」，绝不返回半个函数");

        d = Degrade::Full;
        ContextAssembler::fitTo(code, 1, d);
        check(d == Degrade::PathOnly || d == Degrade::Drop,
              "I-11 预算=1 时也绝不返回残缺内容（宁可没有，不给半截）");
    }

    // ---- ② 压缩级联：L3 摘要 + 切点铁律 + 锚点 ----
    {
        CompactionConfig cc = compactCfgForTest(2'500);
        auto comp = std::make_shared<Compactor>(
            cc, std::make_shared<ExtractiveSummarizer>(cc.maxSummaryChars));

        const std::string kGoal = "把 build 脚本里的 optimize 开关改名为 release";
        auto hist = makeReadHistory(kGoal, 6, 3'000);
        const std::size_t sizeBefore = hist.size();
        const int tokBefore = Compactor::estimateHistoryTokens(hist);
        const int budget    = cc.budget.forLayer(Layer::L5_History);

        CompactionOutcome out = comp->compact(hist, tokBefore);

        check(out.triggered && tokBefore > budget,
              "I-12 历史超预算 → 压缩被触发");

        bool hasL3 = false;
        for (const auto& e : out.events)
            if (e.level == 3 && !e.summary.empty()) hasL3 = true;
        check(hasL3, "I-13 L3 摘要被执行，并留下了交接摘要文本");

        check(hist.size() < sizeBefore && out.tokensAfter < out.tokensBefore,
              "I-14 压缩真的让历史变短了（条数与 token 双降）");

        check(countOrphanToolResults(hist) == 0,
              "I-15 ★ 切点铁律：压缩后没有任何孤儿 tool_result");

        check(!hist.empty() && hist[0].role == Role::User && hist[0].content == kGoal,
              "I-16 ★ 锚点保留：最初的需求在压缩后仍在 history[0]");

        bool pathKept = false;
        for (const auto& m : hist)
            if (m.content.find("src/file_0.cpp") != std::string::npos) pathKept = true;
        check(pathKept, "I-17 ★ 摘要保住了「读过哪些文件」（交接口诀：路径不能丢）");

        Turn t;
        t.input   = "继续";
        t.history = hist;
        const ChatRequest rq = asm_.assemble(t);
        check(!rq.messages.empty() && rq.messages[0].content == kGoal,
              "I-18 组装后请求里仍然带着最初的需求（压缩没有让它失忆）");
    }

    // ---- ③ L1 单条降级：一条**老**消息顶掉整个预算 ----
    //
    // ★ 场景必须让"大消息落在**老历史**里"，不能把它摆在尾巴上 ——
    //   这正是 K-19 的教训（见 compaction.cpp 的 L1 注释）：
    //   尾巴上的 tool_result 是模型**正在用**的东西，降级它 → 模型"咦这文件怎么
    //   只剩函数名" → 重读同一个文件 → 闸门 5 判重复调用 → 整个 turn 判死。
    //   所以本用例先读一个 40K 的大文件，再补若干次小读，把它推进"老历史"。
    //
    //   （真实链路里 40K 的工具结果根本进不了上下文 —— L0 已把它落盘，只留 ~2KB，
    //     见 I-22/I-23。这里直接构造消息，是为了**单独**测 L1 这一级。）
    {
        CompactionConfig cc = compactCfgForTest(4'000);
        cc.perMessageCapDiv = 2;            // 打开 L1（cap = 预算/2 = 2000）

        auto hist = makeReadHistory("读一个大文件，再顺手看几个小文件", 1, 40'000);
        for (int i = 0; i < 6; ++i) {       // 追加 6 次小读，把大读挤进老历史
            Message a;
            a.role = Role::Assistant;
            a.toolCalls.push_back(ToolCall{"call_small_" + std::to_string(i), "Read",
                                           json{{"path", "src/small_" + std::to_string(i) + ".cpp"}}});
            hist.push_back(std::move(a));
            Message t;
            t.role       = Role::Tool;
            t.toolCallId = "call_small_" + std::to_string(i);
            t.content    = std::string(800, 'y');
            hist.push_back(std::move(t));
        }

        auto comp = std::make_shared<Compactor>(
            cc, std::make_shared<ExtractiveSummarizer>(cc.maxSummaryChars));
        const std::size_t nBefore = hist.size();
        const int tokBefore = Compactor::estimateHistoryTokens(hist);
        CompactionOutcome out = comp->compact(hist, tokBefore);

        bool hasL1 = false, deleted = false;
        for (const auto& e : out.events) {
            if (e.level == 1) hasL1 = true;
            if (e.level >= 3) deleted = true;   // L3/L4 才动消息条数
        }
        // "不靠删消息"必须真验证：条数不变 且 没有走到 L3/L4。
        check(hasL1 && out.tokensAfter < tokBefore &&
              hist.size() == nBefore && !deleted,
              "I-19 L1 单条降级：单条超大消息被降级链压下来（不靠删消息）");
    }

    // ---- ④ 摘要失败 → 降级，不抛异常（CR-2） ----
    {
        CompactionConfig cc = compactCfgForTest(2'500);
        auto comp = std::make_shared<Compactor>(cc, std::make_shared<ThrowingSummarizer>());
        auto hist = makeReadHistory("需求", 6, 3'000);
        bool threw = false;
        CompactionOutcome out;
        try {
            out = comp->compact(hist, Compactor::estimateHistoryTokens(hist));
        } catch (...) {
            threw = true;
        }
        check(!threw && out.triggered,
              "I-20 ★ 摘要失败时降级而不是抛异常（失败是返回值，CR-2）");
        check(countOrphanToolResults(hist) == 0,
              "I-21 摘要失败降级到 L4 之后，历史仍然合法");
    }

    // ---- ⑤ L0 落盘：60K 字符 → 上下文只增 ~2KB ----
    {
        ToolDef bash;                          // Bash 的默认阈值 50'000
        bash.name = "Bash";
        ToolResult small = maybePersistLargeResult(
            ToolResult::ok("call_big", std::string(60'000, 'z')), bash, ".af-dump-selftest");
        check(small.persisted && !small.persistedPath.empty(),
              "I-22 ★ L0 落盘：60K 字符的工具结果被写进磁盘");
        check(small.content.size() < 2'500,
              "I-23 ★ L0 落盘：上下文里只留 ~2KB 预览（原本 60K）");

        // ★ K-21：中文内容触发截断时，绝不能切出非法 UTF-8
        //   （20000 个汉字 = 60000 字节；第 2000 字节几乎必然落在某个字中间）
        std::string zh;
        zh.reserve(60'000);
        for (int i = 0; i < 5'000; ++i) zh += "中文测试";
        ToolResult zs = maybePersistLargeResult(
            ToolResult::ok("call_zh", zh), bash, ".af-dump-selftest");
        check(zs.persisted && isValidUtf8(zs.content),
              "I-24 ★ 按字节截断后仍是合法 UTF-8（中文不会切出半截字符）");

        ToolDef rd;
        rd.name = "Read";
        rd.persistThresholdChars = SIZE_MAX;    // 豁免
        ToolResult r2 = maybePersistLargeResult(
            ToolResult::ok("call_r", std::string(60'000, 'z')), rd, ".af-dump-selftest");
        check(!r2.persisted,
              "I-25 Read 豁免落盘（否则形成「读文件→落盘→再读落盘文件」的死循环）");
    }

    // ---- ⑥ ★ H5 主验收：连续读 50 个文件 ----
    {
        CompactionConfig cc = compactCfgForTest(3'000);
        cc.keepRecentTokens   = 1'000;
        cc.foldThresholdChars = 800;        // 打开 L2
        cc.perMessageCapDiv   = 2;          // 打开 L1
        auto comp = std::make_shared<Compactor>(
            cc, std::make_shared<ExtractiveSummarizer>(cc.maxSummaryChars));

        const std::string kGoal = "把 src/ 下所有文件里的 TODO 汇总成一份清单";
        auto hist  = makeReadHistory(kGoal, 50, 1'500);
        const int budget = cc.budget.forLayer(Layer::L5_History);
        const int before = Compactor::estimateHistoryTokens(hist);

        int rounds = 0;
        for (int i = 0; i < 20; ++i) {                 // 防呆上限：最多压 20 轮
            const int t = Compactor::estimateHistoryTokens(hist);
            if (!comp->shouldCompact(t)) break;
            comp->compact(hist, t);
            ++rounds;
        }
        const int after = Compactor::estimateHistoryTokens(hist);

        check(before > budget,
              "I-26 H5：50 个文件的历史一开始就超预算（否则用例本身没意义）");
        check(after <= budget * 2,
              "I-27 ★ H5：压缩后上下文被压回预算量级（不爆）");
        check(hist.size() < 30,
              "I-28 ★ H5：消息条数也压下来了（不是靠缩小每条）");
        check(!hist.empty() && hist[0].content == kGoal,
              "I-29 ★ H5：仍然记得最初的需求（锚点在 history[0]）");
        bool todoKept = false;
        for (const auto& m : hist)
            if (m.content.find("TODO") != std::string::npos) todoKept = true;
        check(todoKept, "I-30 ★ H5：摘要里仍带着原始需求（交接摘要能独立成立）");
        check(countOrphanToolResults(hist) == 0,
              "I-31 ★ H5：压完之后历史依然合法（没有孤儿 tool_result）");
        std::cout << "     [H5] 50 文件：token " << before << " → " << after
                  << "（预算 " << budget << "，压缩 " << rounds << " 轮）\n";
    }

    // ---- ⑦ 组装器的分层预算真的生效（L3/L4 走降级链，不再是无预算直塞）----
    {
        ContextAssembler a(BudgetConfig{}, ui);
        a.setSystem("sys");
        Turn t;
        t.input = "任务";
        Message chunk;
        chunk.role = Role::User;
        // 400K 字符 ≈ 100K token，远超 L3 层分到的预算（默认约 44K token）
        chunk.content = "[检索命中 huge.cpp]\n" + std::string(400'000, 'x');
        t.retrieved.push_back(chunk);

        const ChatRequest r = a.assemble(t);
        std::string hit;
        for (const auto& m : r.messages)
            if (m.content.find("[检索命中 huge.cpp]") != std::string::npos) hit = m.content;
        check(!hit.empty() &&
                  ContextAssembler::estimateTokens(hit) <
                      BudgetConfig{}.forLayer(Layer::L3_Retrieved),
              "I-32 ★ 组装器的分层预算生效：超预算的 L3 命中被降级链压进预算内");
    }

    std::cout << "----- 自检结果：" << pass << " 通过 / " << fail << " 失败 -----\n";
    return fail == 0 ? 0 : 1;
}

// ===========================================================================
//  终端渲染器（观察者的一端）
// ===========================================================================
struct Renderer {
    bool sawFirstToken = false;
    int  textChars = 0;

    void operator()(const ChatChunk& c) {
        std::visit(overloaded{
            [&](const TextDelta& d) {
                if (!d.text.empty()) { sawFirstToken = true; textChars += int(d.text.size()); }
                std::cout << d.text << std::flush;
            },
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

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // ★ 闸门 6 的源头：接管 Ctrl-C，交给循环收尾而不是就地终止进程
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#endif

    std::vector<std::string> args;
#ifdef _WIN32
    args = winUtf8Args();
    (void)argc; (void)argv;
#else
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
#endif
    if (args.size() < 2) {
        std::cerr << "用法: agent.exe \"<任务描述>\"\n"
                     "      agent.exe --selftest\n"
                     "      agent.exe --sandbox-info\n";
        return 1;
    }

    if (args[1] == "--selftest") return runSelfTest();

    const std::string task = sanitizeUtf8(args[1]);

    // ---- 配置（★ L7：两家厂商，各自的默认端点/密钥/默认模型都不同）--------
    ProviderConfig cfg;
    cfg.kind = envUtf8("AF_PROVIDER", "openai");
    if (cfg.kind == "anthropic") {
        cfg.baseUrl = envUtf8("ANTHROPIC_BASE_URL", "https://api.anthropic.com/v1");
        cfg.apiKey  = envUtf8("ANTHROPIC_API_KEY", "");
        cfg.model   = envUtf8("AF_MODEL", "claude-sonnet-4-5");
    } else {
        cfg.baseUrl = envUtf8("OPENAI_BASE_URL", "https://api.openai.com/v1");
        cfg.apiKey  = envUtf8("OPENAI_API_KEY", "");
        cfg.model   = envUtf8("AF_MODEL", "gpt-4o-mini");
    }
    // ★ 上下文窗口由配置决定，而不是硬编码 200k ——
    //   压缩的触发判据用的是它，所以验证脚本靠调小它来演示压缩
    //   （用真模型的 200k 窗口去演示"上下文中爆"要烧掉真金白银）。
    cfg.contextWindow = std::max(2'000, envInt("AF_CONTEXT_WINDOW", cfg.contextWindow));

    const bool isLocal = cfg.baseUrl.find("127.0.0.1") != std::string::npos ||
                         cfg.baseUrl.find("localhost") != std::string::npos;
    if (cfg.apiKey.empty() && !isLocal) {
        std::cerr << "请设置 " << (cfg.kind == "anthropic" ? "ANTHROPIC_API_KEY"
                                                           : "OPENAI_API_KEY")
                  << "（本地 Ollama/vLLM 无需密钥）\n";
        return 1;
    }

    // ---- 沙箱（端口 ③ 的实现）---------------------------------------------
    SandboxConfig sbc;
    sbc.enabled   = true;
    sbc.workspace = pathUtf8(fs::current_path());
    sbc.allowNetwork = (envUtf8("AF_SANDBOX_NET", "0") == "1");
    // 演示用：AF_SANDBOX=0 放开**策略**（禁网/denyRead），内核约束仍在。
    // 注意这不是"关掉沙箱" —— 关掉沙箱（enabled=false）会直接拒绝执行。
    if (envUtf8("AF_SANDBOX", "1") == "0") sbc.allowNetwork = true;
    if (sbc.allowNetwork) sbc.denyRead.clear();
    else sbc.denyRead = {".ssh", ".env", "id_rsa", "credentials", "secret"};
    sbc.protectGit = true;
    auto sandbox = createSandbox(sbc);

    // ---- --sandbox-info：把能力边界打印出来（含"没做到"的部分）------------
    if (args[1] == "--sandbox-info") {
        std::string rep;
        const bool ok = sandbox->selfCheck(rep);
        std::cout << rep << "\nselfCheck=" << (ok ? "OK" : "DEGRADED") << "\n";
        return ok ? 0 : 0;      // 沙箱能力不足不是错误，是**事实**
    }

    // ---- 工具表 ----------------------------------------------
    ToolRegistry registry;
    const int cmdTimeoutMs = envInt("AF_CMD_TIMEOUT_MS", 120'000);
    registerBuiltinTools(registry, sandbox, cmdTimeoutMs);
    registry.setSandbox(sandbox);      // 接口已定；本讲由工具自己调用（见讲义 §3）

    const std::string pluginPath = envUtf8("AF_PLUGIN", "");
    if (!pluginPath.empty()) {
        std::string err;
        if (registry.loadPlugin(pluginPath, err))
            std::cerr << "[插件] 已加载 " << pluginPath << "\n";
        else
            std::cerr << "[插件] 加载失败: " << err << "\n";
    }

    // ---- 权限门：显式白名单 + 按 risk 生成默认（修 L4 遗留 I-9）------------
    {
        std::vector<PermissionRule> rules{
            // deny 段（DefaultPermissionGate 会强制排到最前，不依赖这里的顺序）
            {DecisionKind::Deny,  "Bash(rm -rf *)", "删除类命令一律拒绝"},
            {DecisionKind::Deny,  "Bash(del *)",    "删除类命令一律拒绝"},
            {DecisionKind::Deny,  "Bash(rd *)",     "删除类命令一律拒绝"},
            {DecisionKind::Deny,  "Bash(format *)", "格式化命令一律拒绝"},
            // allow 段（课程演示需要的显式白名单）
            {DecisionKind::Allow, "Read",           "只读操作直接放行"},
            {DecisionKind::Allow, "Write",          "本讲任务就是改文件"},
            {DecisionKind::Allow, "Edit",           "本讲任务就是改文件"},
            {DecisionKind::Allow, "Bash(findstr*)", "只放行查找类命令"},
            {DecisionKind::Allow, "Bash(type *)",   "只放行读取文件"},
            {DecisionKind::Allow, "Bash(ping*)",    "只放行本机回环探测（验证沙箱超时）"},
            // ★ 刻意放行 curl：权限门**放行**，沙箱仍然**拦住**。
            //   这不是配置疏忽，而是想演示"纵深防御"——
            //   权限门回答"给不给这个人这个权限"（接近人机交互），
            //   沙箱回答"这条命令到底能碰到什么"（接近内核）。
            //   两者判据不同、失效方式不同，所以都要有。
            {DecisionKind::Allow, "Bash(curl*)",    "权限门放行 → 交给沙箱判断（纵深防御）"},
        };
        // ★ I-9 的修法：**没被显式规则覆盖**的工具，按它声明的 `risk` 定默认。
        //   为什么必须按 risk 而不是按名字：插件工具的**名字运行时才知道**
        //   （L4 踩过），按名字写规则 = "每装一个插件都要改一次宿主配置"。
        //   按 risk 写：插件作者声明 Read 就自动放行、声明 Exec 就自动落到"询问"，
        //   宿主一行配置都不用改。
        for (const auto& d : registry.defs()) {
            const bool covered = std::any_of(
                rules.begin(), rules.end(), [&](const PermissionRule& r) {
                    return r.toolPattern == d.name ||
                           r.toolPattern.rfind(d.name + "(", 0) == 0;
                });
            if (covered) continue;
            switch (d.risk) {
                case RiskLevel::Read:
                    rules.push_back({DecisionKind::Allow, d.name,
                                     "按 risk=Read 的默认：放行"});
                    break;
                case RiskLevel::Write:
                case RiskLevel::Exec:
                case RiskLevel::Network:
                    rules.push_back({DecisionKind::Ask, d.name,
                                     "按 risk 的默认：需要人工确认"});
                    break;
            }
        }
        registry.setGate(std::make_shared<DefaultPermissionGate>(std::move(rules)));
    }

    auto ui = std::make_shared<ConsoleUI>();

    // ---- 分层预算（★ L7：这一份预算**同时**给组装器和压缩器用）-----------
    //  ★ "上下文能装多少"只能有一个真相来源。两套参数的结果是：
    //    组装器按 200k 装、压缩器按 100k 压，然后你花一整天去查
    //    "为什么压缩老是触发但上下文看起来还很空"。
    BudgetConfig bcfg;
    bcfg.contextWindow = cfg.contextWindow;

    // ---- 组装器 ----
    ContextAssembler assembler(bcfg, ui);
    assembler.setTools(registry.defs());
    assembler.setSystem(
        "你是一个编码 Agent。原则：\n"
        "1. 先用 Read 看懂文件，再动手改 —— 不要凭猜测编辑。\n"
        "2. 改完必须跑命令验证，不能只说\"应该好了\"。\n"
        "3. 工具报错是信息不是失败 —— 读它，然后换一种方法。\n"
        "4. 一次只做一件小事，做完就确认结果。\n"
        "5. 卡住就说明卡在哪，不要原样重试同样的调用。\n"
        "6. 上下文可能被压缩过 —— 以「交接摘要」为准，不要重复已经做过的事。\n");

    // ---- 六道闸门的上限（L5 只读了 maxSteps；本讲全部可配）----------------
    LoopLimits limits;
    limits.stop                 = g_stop.get_token();
    limits.maxSteps             = std::max(1, envInt("AF_MAX_STEPS", limits.maxSteps));
    limits.maxTokensSpent       = envInt("AF_MAX_TOKENS", limits.maxTokensSpent);
    limits.maxConsecutiveErrors = envInt("AF_MAX_ERRORS", limits.maxConsecutiveErrors);
    limits.maxRepeatedCalls     = envInt("AF_MAX_REPEATS", limits.maxRepeatedCalls);
    limits.maxWallClock         = Ms(envInt("AF_MAX_WALL_MS",
                                            int(limits.maxWallClock.count())));

    // ---- 闸门 6 的"自动取消"开关：给自动化验证用 ---------------------------
    //  ★ 它**不是**替代 Ctrl-C —— 走的是同一个 stop_source。
    //    真按 Ctrl-C 走 onConsoleCtrl；这里只是把"人按按钮"换成"到点触发"，
    //    否则离线脚本没法验证取消路径。
    std::thread canceller;
    {
        const int ms = envInt("AF_CANCEL_AFTER_MS", 0);
        if (ms > 0) {
            canceller = std::thread([ms] {
                std::this_thread::sleep_for(Ms(ms));
                g_stop.request_stop();
            });
        }
    }

    auto provider = createProvider(cfg);

    // ---- ★ L7：压缩器 -----------------------------------------------------
    //  摘要器的选择是一个**策略**选择（P-04）：
    //     llm   → LlmSummarizer：调模型写交接摘要（真实路径）
    //     local → ExtractiveSummarizer：本地规则抽取（零成本、确定、离线可验证）
    //     off   → 不压缩（回到 L6 的行为）
    //  ★ 一定要有 "local" 和 "off" 这两个选项：**一个能力如果只能在
    //    "有网 + 有钱"时被验证，那它实际上就是没被验证过。**
    CompactionConfig ccfg;
    ccfg.budget              = bcfg;
    ccfg.keepRecentTokens    = std::max(0, envInt("AF_COMPACT_KEEP_TOKENS",
                                                  ccfg.keepRecentTokens));
    ccfg.foldThresholdChars  = std::max(0, envInt("AF_COMPACT_FOLD_CHARS",
                                                  ccfg.foldThresholdChars));
    ccfg.maxSummaryChars     = std::max(256, envInt("AF_COMPACT_SUMMARY_CHARS",
                                                    ccfg.maxSummaryChars));
    const std::string compMode = envUtf8("AF_COMPACT", "llm");
    ccfg.enabled = (compMode != "off");

    std::shared_ptr<ISummarizer> summarizer;
    if (compMode == "llm") {
        // 摘要的 max_tokens 与 maxSummaryChars 对齐（4 字符 ≈ 1 token）
        summarizer = std::make_shared<LlmSummarizer>(*provider,
                                                     ccfg.maxSummaryChars / 4 + 256);
    } else {
        summarizer = std::make_shared<ExtractiveSummarizer>(ccfg.maxSummaryChars);
    }
    auto compactor = std::make_shared<Compactor>(ccfg, summarizer);

    AgentLoop loop(*provider, registry, assembler, limits);
    loop.setCompactor(compactor);

    // ---- 观察者接线 ----
    Renderer renderer;
    int toolCalls = 0;
    int chunks = 0;
    int notices = 0;

    LoopHooks hooks;
    hooks.onChunk = [&](const ChatChunk& c) { ++chunks; renderer(c); };
    hooks.onToolStart = [](const ToolCall& tc) {
        std::cout << "[事件] onToolStart: " << tc.name << "\n" << std::flush;
    };
    hooks.onToolDone = [&](const ToolResult& r) {
        std::cout << "[事件] onToolDone: " << (r.isError ? "错误" : "成功") << "\n"
                  << (r.isError ? "[工具失败] " : "[工具返回] ")
                  << r.content.substr(0, 400)
                  << (r.content.size() > 400 ? " ..." : "") << "\n\n" << std::flush;
        ++toolCalls;
    };
    hooks.onNotice = [&](const std::string& msg) {
        ++notices;
        std::cout << "[事件] onNotice: " << msg << "\n" << std::flush;
    };
    hooks.onError = [](const std::string& e) {
        std::cerr << "\n[请求失败] " << e << "\n" << std::flush;
    };

    // ---- 跑一个 turn ----
    Turn turn;
    turn.input = task;

    const LoopOutcome outcome = loop.run(turn, hooks);
    if (canceller.joinable()) canceller.join();

    const auto lu = assembler.measure(turn);
    const auto& comps = loop.compactionEvents();
    std::cout << "\n===== " << toString(outcome)
              << " | toolCalls=" << toolCalls
              << " | chunks=" << chunks
              << " | notices=" << notices
              << " | steps<= " << limits.maxSteps
              << " | tokens=" << loop.spentTokens()
              << " (in " << loop.totalUsage().inputTokens
              << " / out " << loop.totalUsage().outputTokens << ")"
              << " | history=" << turn.history.size() << " msgs"
              << " | L0=" << lu.l0 << " L5=" << lu.l5 << " L6=" << lu.l6
              << " =====\n";

    // ---- ★ L7：压缩的可观测性 ------------------------------------------
    //  ★ 只报"压缩了"是不够的。必须能看出**用了哪一级、花了多少、丢了多少** ——
    //    否则"摘要质量差"这类问题根本无从排查：
    //    到底是走了本地折叠（没理解语义），还是走了 LLM 摘要（那次调用失败了）？
    std::cout << "compact=" << comps.size()
              << " level=";
    if (comps.empty()) {
        std::cout << "none";
    } else {
        for (std::size_t i = 0; i < comps.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << comps[i].level;
        }
        std::cout << " tokens=" << comps.front().tokensBefore
                  << "->" << comps.back().tokensAfter;
    }
    std::cout << " =====\n";
    for (const auto& c : comps)
        std::cout << "[压缩] L" << c.level << " " << c.tokensBefore << " -> "
                  << c.tokensAfter << " : " << c.detail << "\n";

    if (outcome == LoopOutcome::Completed) {
        if (!renderer.sawFirstToken) std::cout << "[警告] 最终回答为空\n";
        return 0;
    }
    return 2;
}
