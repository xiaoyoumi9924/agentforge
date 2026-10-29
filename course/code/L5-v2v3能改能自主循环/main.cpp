// ---------------------------------------------------------------------------
//  L5 · main.cpp —— v3 组合根（Composition Root）
//
//  v1（L4）的 main 里有一个**临时的 for 循环**，注释写着"它还不是 AgentLoop"。
//  v3 把它搬进 `agent_loop.cpp`，main 退回它真正该干的事：
//
//      装配依赖 → 给上限 → 接线观察者 → 跑 → 收尾打印
//
//  ★ 本讲 main 里的三件新东西：
//      ① `Write` / `Edit` / `Bash` 三个新工具（原语，不是功能）
//      ② 观察者接线：把 LoopHooks 的 6 个回调接到终端
//      ③ `--selftest`：单元级自检（G8 的三类非法历史 + 组装三规矩）
//         —— 因为"孤儿 tool_result"是**裁剪造出来的**，而裁剪在 L7，
//            本讲没有端到端路径能触发它
//
//  用法：
//      agent.exe "把 work_target.txt 里的 add 改名为 sum，然后跑测试确认"
//      agent.exe --selftest
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

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "agent_loop.hpp"
#include "context_budget.hpp"
#include "model_provider.hpp"
#include "tool_registry.hpp"
#include "types.hpp"

namespace fs = std::filesystem;
using namespace af;

namespace {

std::string envUtf8(const char* key, const std::string& def);

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

// ★ 与 winUtf8Args 同一类问题：getenv 拿到的也是 ANSI 代码页字节。
//   本课程自己的目录就叫 11-逐讲/L5-v2v3能改能自主循环 —— 用 getenv 读
//   含中文的路径会拿到 GBK 字节，再经 sanitizeUtf8 把汉字换成 U+FFFD。
//   Windows 上"内核要用的字符串"一律走宽字符 API。
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

// ---------------------------------------------------------------------------
//  端口 ④ 的朴素实现：终端
//
//  ★ 注意它同时被两条路用：
//      · 工具派发（ToolRegistry::dispatch 的权限门）→ confirm()
//      · 循环把它当 uiPort() 取出来
//    这是"端口"的价值：同一份实现服务两个调用方，而两边都不知道对方存在。
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
//
//  ★ 原则：**原语优于集成**。
//    不给模型造 FixTest / Refactor 这种大工具 —— 4 个原语能组合出无限行为，
//    而且每一步都是可观察、可回滚、可审计的独立调用。
// ===========================================================================

std::string readFileWithLineNumbers(const std::string& path, int maxLines = 2000) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream os;
    std::string line;
    int n = 0;
    while (std::getline(ifs, line) && n < maxLines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // 去 CRLF 的 \r
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
    // ★ 豁免落盘。否则会形成"读文件 → 结果太大被落盘 → 再去读那个落盘文件"
    //   的循环依赖。这不是优化，是正确性要求。
    def.persistThresholdChars = SIZE_MAX;

    reg.registerBuiltin(def, [](const json& args) -> ToolResult {
        const std::string path = args.value("path", std::string());
        if (path.empty()) return ToolResult::error("", "path 不能为空");
        std::string content = readFileWithLineNumbers(path);
        if (content.empty()) return ToolResult::error("", "无法读取文件（不存在或为空）: " + path);
        return ToolResult::ok("", std::move(content));
    });
}

// Write 的"唯一要点"：它是**破坏性覆盖**。
// 描述里必须写明"先 Read 再写"，否则模型是在盲写 —— 它会凭记忆重写文件，
// 悄悄丢掉自己没读到的那几行。
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

// Edit 的"唯一要点"：old_str 必须**在文件中唯一出现**。
// 不唯一就失败 —— 这是一条用"失败"换"准确性"的设计：
// 模型被迫给出更多上下文，于是它必须先把文件读准。
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

// Bash 的"唯一要点"：它是**没有护栏**的。
// ★ 本讲刻意不接沙箱 —— 但必须在描述和注释里写明，
//   让学生知道"我现在手上的东西有什么危险"。
//   L6 会把它换成 ISandbox::exec()，而**这个 lambda 的签名和位置都不用变**。
void registerBashTool(ToolRegistry& reg) {
    ToolDef def;
    def.name = "Bash";
    def.description =
        "执行一条 shell 命令：跑测试、构建、查找、查看文件。"
        "命令的退出码会一并返回 —— 非 0 表示命令自己失败了，不是工具坏了。"
        "★ 本讲未接沙箱：命令直接在本机运行，没有网络隔离与写权限限制。";
    def.inputSchema = json::object({
        {"type", "object"},
        {"properties", {{"command", {{"type", "string"},
                                     {"description", "要执行的命令，例如 findstr /C:sum a.txt"}}}}},
        {"required", json::array({"command"})},
    });
    def.risk = RiskLevel::Exec;
    def.timeoutMs = 120'000;

    reg.registerBuiltin(def, [](const json& args) -> ToolResult {
        const std::string cmd = args.value("command", std::string());
        if (cmd.empty()) return ToolResult::error("", "command 不能为空");

        std::string out;
#ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "r");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe) return ToolResult::error("", "无法启动命令（popen 失败）");
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
#ifdef _WIN32
        const int code = _pclose(pipe);
#else
        const int code = pclose(pipe);
#endif
        // ★ 退出码必须进结果。把"非 0"当成功返回空输出，
        //   会让模型得到"文件里没有"这个**错误结论**。
        if (code != 0)
            return ToolResult::error("", out + "\n[exit code " + std::to_string(code) + "]");
        if (out.empty()) out = "(命令成功，无输出)";
        return ToolResult::ok("", std::move(out));
    });
}

void registerBuiltinTools(ToolRegistry& reg) {
    registerReadTool(reg);
    registerWriteTool(reg);
    registerEditTool(reg);
    registerBashTool(reg);
}

// ===========================================================================
//  --selftest：单元级自检
//
//  ★ 为什么本讲需要它：孤儿 tool_result 是**被历史裁剪造出来的**，
//    而裁剪在 L7 —— 本讲没有端到端路径能触发它。
//    所以用单元级验证补上，并明确标注"这是自检，不是端到端"。
//    （推进计划的 DoD 允许这种降级，前提是**不假装跑通**。）
// ===========================================================================
int runSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) ++pass; else ++fail;
        std::cout << "[自检] " << (ok ? "PASS" : "FAIL") << "  " << name << "\n";
    };

    // 造消息的小工具
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

    std::cout << "===== L5 自检：消息合法性（G8）+ 组装三规矩 =====\n";

    // ---- I-1 孤儿 tool 消息 ----
    {
        Turn t; t.input = "任务";
        t.history = { user("你好"), toolResult("call_x") };
        check(countTool(asm_.assemble(t)) == 0, "I-1 孤儿 tool 消息被丢弃（否则服务端 400）");
    }
    // ---- I-2 ★ 并行工具调用：两条结果都要保留 ----
    {
        Turn t; t.input = "任务";
        t.history = { user("你好"),
                      assistantWithTools({"call_a", "call_b"}),
                      toolResult("call_a"), toolResult("call_b") };
        check(countTool(asm_.assemble(t)) == 2,
              "I-2 并行 tool_call 的两条结果都保留（按 id 集合配对，不是看邻居）");
    }
    // ---- I-3 配对窗口被非 tool 消息打断 ----
    {
        Turn t; t.input = "任务";
        t.history = { user("你好"),
                      assistantWithTools({"call_a"}),
                      user("[系统] 模型请求失败，请继续"),     // 打断配对窗口
                      toolResult("call_a") };
        check(countTool(asm_.assemble(t)) == 0,
              "I-3 配对窗口被打断后，迟到的 tool 结果被丢弃");
    }
    // ---- I-4 L6 用户输入永远最后、永远完整 ----
    {
        Turn t; t.input = "这是用户的原始任务";
        t.history = { user("旧消息") };
        const ChatRequest r = asm_.assemble(t);
        check(!r.messages.empty() && r.messages.back().role == Role::User &&
              r.messages.back().content == t.input,
              "I-4 L6 用户输入永远在 messages 末尾且完整");
    }
    // ---- I-5 工具表原样下发（顺序稳定） ----
    {
        Turn t; t.input = "任务";
        const ChatRequest r = asm_.assemble(t);
        check(r.tools.size() == 4 && r.tools[0].name == "Read" &&
              r.tools[3].name == "Bash",
              "I-5 工具表原样下发，顺序与注册顺序一致（顺序即缓存）");
    }
    // ---- I-6 L0 系统提示非空 ----
    {
        Turn t; t.input = "任务";
        const ChatRequest r = asm_.assemble(t);
        check(r.system.has_value() && !r.system->empty(),
              "I-6 L0 系统提示非空（缓存前缀，字节级稳定）");
    }

    std::cout << "----- 自检结果：" << pass << " 通过 / " << fail << " 失败 -----\n";
    return fail == 0 ? 0 : 1;
}

// ===========================================================================
//  终端渲染器（观察者的一端）
//  ★ 仍然**不写兜底分支** —— 让编译器盯着 8 种事件有没有漏处理。
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
            [&](const ToolCallDelta&) {},              // 半个 JSON，展示只会误导
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
#endif

    // ---- 取任务 ----
#ifdef _WIN32
    std::vector<std::string> args = winUtf8Args();
    (void)argc; (void)argv;
#else
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
#endif
    if (args.size() < 2) {
        std::cerr << "用法: agent.exe \"<任务描述>\"\n"
                     "      agent.exe --selftest\n";
        return 1;
    }

    // ---- 自检入口（不连网络、不需要密钥）----
    if (args[1] == "--selftest") return runSelfTest();

    const std::string task = sanitizeUtf8(args[1]);

    // ---- 配置 ----
    ProviderConfig cfg;
    cfg.kind    = envUtf8("AF_PROVIDER", "openai");
    cfg.baseUrl = envUtf8("OPENAI_BASE_URL", "https://api.openai.com/v1");
    cfg.apiKey  = envUtf8("OPENAI_API_KEY", "");
    cfg.model   = envUtf8("AF_MODEL", "gpt-4o-mini");

    const bool isLocal = cfg.baseUrl.find("127.0.0.1") != std::string::npos ||
                         cfg.baseUrl.find("localhost") != std::string::npos;
    if (cfg.apiKey.empty() && !isLocal) {
        std::cerr << "请设置 OPENAI_API_KEY（本地 Ollama/vLLM 无需密钥）\n";
        return 1;
    }
    std::cerr << "[配置] model=" << cfg.model << " base=" << cfg.baseUrl << "\n";

    // ---- 工具表（4 个原语）----
    ToolRegistry registry;
    registerBuiltinTools(registry);

    // 可选：加载 C ABI 插件（L4 的能力，本讲保留但不重复验证）
    const std::string pluginPath = envUtf8("AF_PLUGIN", "");
    if (!pluginPath.empty()) {
        std::string err;
        if (registry.loadPlugin(pluginPath, err))
            std::cerr << "[插件] 已加载 " << pluginPath << "\n";
        else
            std::cerr << "[插件] 加载失败: " << err << "\n";
    }

    // ---- 权限门：deny 永远赢（DefaultPermissionGate 内部强制排序）----
    {
        std::vector<PermissionRule> rules{
            {DecisionKind::Deny,  "Bash(rm -rf *)", "删除类命令一律拒绝"},
            {DecisionKind::Deny,  "Bash(del *)",    "删除类命令一律拒绝"},
            {DecisionKind::Allow, "Read",           "只读操作直接放行"},
            {DecisionKind::Allow, "Write",          "本讲允许写：任务就是改文件"},
            {DecisionKind::Allow, "Edit",           "本讲允许改：任务就是改文件"},
            {DecisionKind::Allow, "Bash(findstr*)","只放行查找类命令"},
        };
        registry.setGate(std::make_shared<DefaultPermissionGate>(std::move(rules)));
    }

    auto ui = std::make_shared<ConsoleUI>();

    // ---- 组装器 ----
    ContextAssembler assembler(BudgetConfig{}, ui);
    // ★ 工具表必须交给组装器，否则发出去的请求里没有 tools，模型无从调用
    assembler.setTools(registry.defs());
    assembler.setSystem(
        "你是一个编码 Agent。原则：\n"
        "1. 先用 Read 看懂文件，再动手改 —— 不要凭猜测编辑。\n"
        "2. 改完必须跑命令验证，不能只说\"应该好了\"。\n"
        "3. 工具报错是信息不是失败 —— 读它，然后换一种方法。\n"
        "4. 一次只做一件小事，做完就确认结果。\n"
        "5. 卡住就说明卡在哪，不要原样重试同样的调用。\n");

    // ---- 循环上限：AF_MAX_STEPS 可调（本讲的"粗上限"，L6 变成闸门 1）----
    LoopLimits limits;
    {
        const std::string s = envUtf8("AF_MAX_STEPS", "");
        if (!s.empty()) {
            try { limits.maxSteps = std::stoi(s); } catch (...) {}
        }
    }
    if (limits.maxSteps < 1) limits.maxSteps = 1;

    auto provider = createProvider(cfg);
    AgentLoop loop(*provider, registry, assembler, limits);

    // ---- 观察者接线：核心只抛事件，渲染全在这里 ----
    Renderer renderer;
    int toolCalls = 0;
    int chunks = 0;              // onChunk 被调用的次数（观察者确实在工作）

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
    hooks.onNotice = [](const std::string& msg) {
        std::cout << "[事件] onNotice: " << msg << "\n" << std::flush;
    };
    hooks.onError = [](const std::string& e) {
        std::cerr << "\n[请求失败] " << e << "\n" << std::flush;
    };
    // hooks.confirm 本讲未接：环还没有"需要用户二次确认"的决策点（L6 的闸门 6）

    // ---- 跑一个 turn ----
    Turn turn;
    turn.input = task;

    const LoopOutcome outcome = loop.run(turn, hooks);

    const auto lu = assembler.measure(turn);
    std::cout << "\n===== " << toString(outcome)
              << " | toolCalls=" << toolCalls
              << " | chunks=" << chunks
              << " | tokens=" << loop.spentTokens()
              << " (in " << loop.totalUsage().inputTokens
              << " / out " << loop.totalUsage().outputTokens << ")"
              << " | history=" << turn.history.size() << " msgs"
              << " | L0=" << lu.l0 << " L5=" << lu.l5 << " L6=" << lu.l6
              << " =====\n";

    if (outcome == LoopOutcome::Completed) {
        if (!renderer.sawFirstToken) std::cout << "[警告] 最终回答为空\n";
        return 0;
    }
    return 2;
}
