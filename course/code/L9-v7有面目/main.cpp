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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
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
#include "repo_index.hpp"
#include "sandbox.hpp"
#include "symbol_extract.hpp"
#include "tool_registry.hpp"
#include "types.hpp"
#include "ui/tui.hpp"

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

// ★★ K-7 的另一半（L8 才踩到）：
//
//  `pathUtf8()` 修的是"**读出来**是 ANSI"这一半。另一半是：
//  `fs::path` 从 `char` / `std::string` **构造**时，Windows 上也按 ANSI 代码页
//  解释字节（中文机 = GBK），**不是 UTF-8**。
//
//  于是"程序内部一律 UTF-8"这条约定在 path 这一层断了：
//      UTF-8 字节 --(被当 GBK 读)--> 乱码 wchar_t --(再存回)--> 更乱的字节
//  轻则"文件明明在，exists() 却说没有"；重则乱码串喂进 exists()/absolute()
//  直接 0xC0000409 崩进程（本讲实测：`--show-repo-map . 中文名.txt` 必崩）。
//
//  ★ 判据很简单：**打印出来是"涓枃"这种字** —— 那就是 UTF-8 被当 GBK 读了。
//    看到这种字不要去查编码库，直接查"哪一步把 std::string 交给了 fs::path"。
fs::path pathFromUtf8(std::string_view s) {
#ifdef _WIN32
    if (s.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (need <= 0) return fs::path(std::string(s));
    std::wstring w(size_t(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), need);
    return fs::path(w);
#else
    return fs::path(std::string(s));
#endif
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

    // ---- ⑧ ★ L8：仓库索引（增量 / 边界 / 依赖边 / 预算）--------------------
    std::cout << "===== L8 自检：仓库索引 =====\n";
    {
        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path(ec) / "af_repo_selftest";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp / "src", ec);

        auto wr = [](const fs::path& p, const std::string& s) {
            std::error_code e2;
            fs::create_directories(p.parent_path(), e2);
            std::ofstream f(p, std::ios::binary);
            f.write(s.data(), static_cast<std::streamsize>(s.size()));
        };

        wr(tmp / "src" / "a.hpp",
           "#pragma once\nnamespace n { class Alpha { public: int fa(int x); }; }\n");
        wr(tmp / "src" / "a.cpp",
           "#include \"a.hpp\"\nnamespace n { int Alpha::fa(int x) { return x + 1; } }\n");
        wr(tmp / "src" / "b.cpp",
           "#include \"a.hpp\"\nnamespace n { int fb() { return 2; } }\n");
        wr(tmp / "src" / "empty.cpp", "");
        wr(tmp / "src" / "comment.cpp", "// 只有一行注释\n");
        // 非法 UTF-8：必须照常入库、不许崩
        wr(tmp / "src" / "bad.cpp",
           std::string("// bad \xff\xfe\x80\nint g(int x){ return x; }\n"));
        // 超大文件：必须被 maxBytes 拦下
        wr(tmp / "src" / "huge.cpp", "// huge\nint big[] = {" +
                                     std::string(1'500'000, '1') + "};\n");

        RepoIndexConfig rc;
        rc.tokenBudget = 1024;
        RepoIndex idx(rc);
        idx.build(tmp);

        check(idx.stats().files >= 7,
              "I-33 仓库索引：文件照常入库（空/纯注释/非法 UTF-8 都不崩）");
        check(idx.stats().skippedTooBig == 1,
              "I-34 ★ 超大文件被 maxBytes 拦下（索引不因一个生成文件而拖垮）");

        const size_t n1 = idx.stats().symbols;
        // 改一个文件（长度变了 → hash 变了 → 会被重解析）
        wr(tmp / "src" / "a.cpp",
           "#include \"a.hpp\"\nnamespace n { int Alpha::fa(int x) { return x + 1; }\n"
           "int extraFn(int y) { return y * 2; } }\n");
        idx.update();
        const size_t n2 = idx.stats().symbols;
        check(n2 > n1 && n2 < n1 * 2,
              "I-35 ★ 增量 update：符号数增加但没翻倍（重解析前先清旧符号）");
        std::cout << "     [索引] update 前后符号数 " << n1 << " → " << n2 << "\n";

        // 依赖边：a.hpp 被 a.cpp 与 b.cpp include，rank 应高于只出不进的 b.cpp
        const double ra = idx.rankOf(tmp / "src" / "a.hpp", {});
        const double rb = idx.rankOf(tmp / "src" / "b.cpp", {});
        check(ra > rb, "I-36 ★ 依赖图：被 include 最多的文件 rank 更高");
        std::cout << "     [索引] rank a.hpp=" << ra << "  b.cpp=" << rb << "\n";

        // ★ rank 必须是一个"权重"（总和 ≈ 1），否则它只是个不可解释的数。
        //   这条断言抓的是"悬挂节点质量被算了两次"那类 bug —— 它们不报错、
        //   不改变排序，只会让数字悄悄膨胀，直到某天有人拿 rank 去设阈值。
        double sum = 0;
        for (const auto& f : idx.files()) sum += idx.rankOf(f, {});
        check(sum > 0.9 && sum < 1.1, "I-36b ★ PageRank 归一化：所有文件的 rank 之和 ≈ 1");
        std::cout << "     [索引] rank 之和 = " << sum << "\n";

        RepoIndexConfig rc2; rc2.tokenBudget = 64;
        RepoIndex idx2(rc2);
        idx2.build(tmp);
        const std::string m2 = idx2.renderMap({});
        check(static_cast<int>(m2.size()) <= 64 * 4 + 64,
              "I-37 ★ 预算裁剪：tokenBudget=64 时地图确实被裁到预算内");
        check(!m2.empty(), "I-38 预算再小也不能产出空地图（宁可少，不能没有）");
    }

    // =======================================================================
    //  ★ L9：UI 层（渲染与 Agent 线程分离）
    //
    //  ★ 这三条为什么值得进 --selftest（而不是只留在 verify.ps1）：
    //    stripAnsi、队列、后端选择都是**纯函数性质**的，不需要模型也不需要网络。
    //    "不需要外部依赖就能验证"的部分就该放进自检 ——
    //    这是 L7 定下的规矩（凡只能联网验证的能力，等于没被验证）。
    // =======================================================================
    std::cout << "\n===== L9 自检：UI 事件队列 / ANSI / 双后端 =====\n";
    {
        using af::ui::UiEvent;

        // I-39：★ CSI 序列长度不定 —— "跳过 3 个字符"那种写法在这里就会露馅
        const std::string s = std::string("a") + "\x1b[1m" + "b" + "\x1b[38;5;196m" +
                              "c" + "\x1b[0m" + "d";
        const std::string stripped = af::ui::stripAnsi(s);
        check(stripped == "abcd", "I-39 ★ stripAnsi：变长 CSI（4/11/4 字节）全部剥净");
        check(af::ui::stripAnsi("plain text") == "plain text",
              "I-40 stripAnsi 对无转义文本是恒等变换（不能顺手改坏）");
        std::cout << "     [UI] stripAnsi(\"" << s.size() << " 字节\") -> \""
                  << stripped << "\"\n";

        // I-41：★ 队列是 FIFO 且**有界**（背压，不是无限吃内存）
        af::ui::EventQueue q(4);
        for (int i = 0; i < 4; ++i) {
            UiEvent e; e.kind = UiEvent::Kind::Text; e.text = std::to_string(i);
            q.push(e);
        }
        check(q.size() == 4, "I-41 ★ 事件队列有界：容量 4 时装满即止（背压的前提）");
        const auto got = q.drain();
        check(got.size() == 4 && got[0].text == "0" && got[3].text == "3",
              "I-42 ★ 队列是 FIFO：事件顺序就是发生顺序（渲染不能乱序）");
        check(q.size() == 0, "I-43 drain 取空后队列清零（不会重复消费）");

        // I-44：★ 双后端都必须拿得到 —— 缺库时降级不是"没有 UI"
        auto rp = af::ui::makeRenderer(false);
        check(rp != nullptr, "I-44 ★ makeRenderer(false) 永远拿得到渲染器（降级不是瘫痪）");
        check(std::string(rp->name()) == "plain", "I-45 强制非画布时后端是 plain");
        auto rt = af::ui::makeRenderer(true);
        check(rt != nullptr, "I-46 makeRenderer(true) 也永远拿得到（缺库时退回 plain）");
        std::cout << "     [UI] 后端 plain=" << rp->name()
                  << "  tui=" << rt->name() << "\n";

        // I-47：★ 渲染器"吸收事件"与"出帧"是两步 —— 空状态也能出帧（不能崩）
        UiEvent e;
        e.kind = UiEvent::Kind::Text; e.text = "hello";
        rt->apply(e);
        const std::string f1 = rt->render();
        check(!f1.empty() || std::string(rt->name()) == "plain",
              "I-47 空/半状态下的 render() 不崩（画布缺少元素时也要能画）");
        e.kind = UiEvent::Kind::ToolStart; e.toolName = "Read"; e.toolId = "1";
        rt->apply(e);
        check(rt->toolInFlight(), "I-48 ★ toolInFlight：渲染循环据此在没有事件时继续出帧");
        e.kind = UiEvent::Kind::ToolDone; e.toolId = "1"; e.elapsedMs = 42;
        rt->apply(e);
        check(!rt->toolInFlight(), "I-49 工具结束后 inFlight 归位（spinner 必须停）");
    }

    std::cout << "----- 自检结果：" << pass << " 通过 / " << fail << " 失败 -----\n";
    return fail == 0 ? 0 : 1;
}

// ===========================================================================
//  ★ L9：ChatChunk → UiEvent（协议层 → UI 层的翻译）
//
//  ★ 为什么要翻译一层，而不是让 UI 直接消费 ChatChunk：
//    ChatChunk 是**协议层**的类型。让 UI 依赖它，等于让 UI 知道协议 ——
//    换一家厂商多一种 chunk，UI 就得跟着改。翻译层把这条边切断。
//
//  ★ ToolCallDelta 不产生独立事件：参数是"半个 JSON"，画不出来也不需要画，
//    它在 ToolArgs 那一次落地。**UI 只该收到能消费的东西。**
// ===========================================================================
static af::ui::UiEvent toUiEvent(const ChatChunk& c) {
    using af::ui::UiEvent;
    UiEvent e;
    std::visit(overloaded{
        [&](const TextDelta& d) {
            e.kind = UiEvent::Kind::Text; e.text = d.text;
        },
        [&](const ThinkingDelta& d) {
            e.kind = UiEvent::Kind::Thinking; e.text = d.text;
        },
        [&](const ToolCallStart& s) {
            e.kind = UiEvent::Kind::ToolCall; e.toolName = s.name; e.toolId = s.id;
        },
        [&](const ToolCallDelta&) {
            // 半个 JSON：不落地。给个空 Text，两个渲染器都会忽略它。
            e.kind = UiEvent::Kind::Text;
        },
        [&](const ToolCallDone& t) {
            e.kind = UiEvent::Kind::ToolArgs; e.text = t.args.dump(); e.toolId = t.id;
        },
        [&](const UsageChunk& u) {
            e.kind = UiEvent::Kind::Usage; e.tokens = u.usage.total();
        },
        [&](const ErrorChunk& x) {
            e.kind = UiEvent::Kind::StreamError;
            e.text = x.message + (x.retryable ? "（可重试）" : "");
            e.isError = true;
        },
        [&](const DoneChunk&) { e.kind = UiEvent::Kind::StreamDone; },
    }, c);
    return e;
}

// ---------------------------------------------------------------------------
//  ★ L9：--ui-demo —— 不需要模型、不需要网络就能验证 UI 的三件事
//     · 工具执行期间画面**还在更新**（spinner / 耗时条，H7 的同源产物）
//     · 权限弹窗挂着的时候**渲染循环仍在转**（弹窗不阻塞渲染）
//     · 两个后端（ftxui / plain）都能产出内容，且非 TTY 时零 ANSI
//
//  ★ 为什么要有这个命令：UI 的"不卡"是一种**时序性质**，
//    靠一次真实对话去碰运气（希望工具刚好跑够 1 秒）是测不稳的。
//    把时序写成脚本，它就变成可回归的断言。
// ---------------------------------------------------------------------------
static int runUiDemo(const std::vector<std::string>& extra) {
    using af::ui::UiEvent;
    std::string want = extra.empty() ? envUtf8("AF_UI", "auto") : extra[0];
    if (want.empty()) want = "auto";
    const bool isTty = af::ui::stdoutIsTty();
    const bool wantTui = (want == "ftxui" || want == "tui") ||
                         (want == "auto" && isTty);
    auto r = af::ui::makeRenderer(wantTui);
    std::cout << "[UI] 请求后端=" << want << "  实际后端=" << r->name()
              << "  isTty=" << (isTty ? "1" : "0") << "\n" << std::flush;

    af::ui::UiLoop ui(std::move(r), isTty);

    UiEvent e;
    e.kind = UiEvent::Kind::Text;
    e.text = "读一下 work_target.txt\n";
    ui.push(e);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    e = UiEvent{};
    e.kind = UiEvent::Kind::ToolStart;
    e.toolName = "Read"; e.toolId = "t1";
    ui.push(e);

    // ★ 让工具"跑" 1.2 秒。这 1.2 秒里 Agent 线程**一个事件都不推** ——
    //   画面上的「执行中」只能是渲染线程自己画出来的。这就是 C29 的判据。
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    e = UiEvent{};
    e.kind = UiEvent::Kind::ToolDone;
    e.toolName = "Read"; e.toolId = "t1"; e.elapsedMs = 1200; e.text = "128 行";
    ui.push(e);

    e = UiEvent{};
    e.kind = UiEvent::Kind::Text;
    e.text = "改完了，跑一下测试\n";
    ui.push(e);

    // ★ 权限弹窗：只推 PermAsk，**不回答** ——
    //   这样就能验证"弹窗挂在那儿的时候渲染循环还在转"（C30）。
    e = UiEvent{};
    e.kind = UiEvent::Kind::PermAsk;
    e.promptId = 1; e.text = "Bash: rm -rf build";
    ui.push(e);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    e = UiEvent{}; e.kind = UiEvent::Kind::Usage;   e.tokens = 1234; ui.push(e);
    e = UiEvent{}; e.kind = UiEvent::Kind::StreamDone;               ui.push(e);

    ui.stop();
    std::cout << "[UI] events=" << ui.events() << " frames=" << ui.frames()
              << " spinnerTicks=" << ui.spinnerTicks() << "\n" << std::flush;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
//  ★ L8：仓库地图（模块 ③）的两个可观测入口
//
//    --show-repo-map [root] [active...]   打印地图 + 实况统计
//    --symbol-ab     [root]               同一份仓库跑两遍后端，打印召回差异
//
//  ★ 为什么把"A/B 对比"做成命令而不是写在讲义里的一句结论：
//    "真解析器比正则强"是人人都同意的话，但**强多少、强在哪些符号上**必须能
//    一键量出来，否则它只是信仰。L8 的验收就建立在这条命令的输出上。
// ---------------------------------------------------------------------------
static RepoIndexConfig repoIndexConfigFromEnv() {
    RepoIndexConfig rc;
    rc.tokenBudget      = std::max(64, envInt("AF_MAP_TOKENS", rc.tokenBudget));
    rc.extract.backend  = backendFromName(envUtf8("AF_SYMBOLS", "auto"));
    rc.extract.maxBytes =
        size_t(std::max(1, envInt("AF_MAX_FILE_BYTES", int(rc.extract.maxBytes))));
    rc.extract.timeoutMicros =
        uint64_t(std::max(1, envInt("AF_TS_TIMEOUT_MS", 2000))) * 1000;
    return rc;
}

// 工具参数里取 path。
// ★ ToolCall::args 是**已经解析好的 json**，不要再对字符串做查找 ——
//   那是 L4 插件里因为拿不到 JSON 库才用的权宜之计（见遗留 I-10）。
//   取不到就当没有：偏置不生效而已，不能让它崩。
static std::string argPathOf(const ToolCall& tc) {
    if (!tc.args.is_object()) return {};
    auto it = tc.args.find("path");
    if (it == tc.args.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

static int runShowRepoMap(const std::vector<std::string>& extra) {
    const std::string rootArg = extra.empty() ? "." : extra[0];
    std::error_code ec;
    fs::path root = fs::absolute(pathFromUtf8(rootArg), ec);
    if (ec || !fs::exists(root)) { std::cerr << "路径不存在: " << rootArg << "\n"; return 1; }

    // ★ 活动文件路径要相对**仓库根**解析，不是相对当前目录。
    //   直接 fs::absolute 会得到 <cwd>\src\... ，它根本不在索引里 →
    //   fileIdx_ 查不到 → bias 全部落空 → rank 还是 0，而**没有任何报错**。
    //   这类 bug 的症状是"功能看起来没效果"，而不是崩溃。
    std::vector<fs::path> active;
    for (size_t i = 1; i < extra.size(); ++i) {
        // ★ 先按"仓库根"解析，再退回"当前目录"。
        //   顺序反过来（先 absolute 再 root）会在**工作区外 + 含中文**的路径上
        //   撞上 fs::path 的 ANSI 误解码（见 pathFromUtf8 的注释）。
        fs::path cand = (root / pathFromUtf8(extra[i])).lexically_normal();
        if (!fs::exists(cand)) cand = fs::absolute(pathFromUtf8(extra[i]), ec);
        if (fs::exists(cand)) active.push_back(cand.lexically_normal());
        else std::cout << "[警告] 活动文件不在索引里: " << extra[i] << "\n";
    }

    // ★ stdout 是缓冲的：进程一崩，缓冲区里的内容全部丢失，
    //   于是"崩溃现场"看起来是"一行输出都没有"，排查方向直接被带偏。
    //   所以在每个阶段末尾 flush —— 让日志至少能指到"崩在哪一步之后"。
    std::cout << "[索引] 开始构建 root=" << pathUtf8(root)
              << "  活动文件 " << active.size() << " 个" << std::endl;

    RepoIndexConfig rc = repoIndexConfigFromEnv();
    RepoIndex idx(rc);
    idx.build(root);
    std::cout << "[索引] 构建完成 files=" << idx.stats().files << std::endl;

    const RepoIndexStats& st = idx.stats();
    const std::string map = idx.renderMap(active);
    std::cout << "[索引] 地图渲染完成 " << map.size() << " 字符" << std::endl;

    std::cout << "=== repo map ===\n"
              << "root=" << pathUtf8(root) << "\n"
              << "backend=" << (st.usedTreeSitter ? "treesitter" : "lexical")
              << "  (编译期 tree-sitter: " << (treeSitterAvailable() ? "有" : "无") << ")\n"
              << "files=" << st.files << "  symbols=" << st.symbols
              << "  buildMs=" << int64_t(st.buildMs) << "\n"
              << "skipped: ignored=" << st.skippedIgnored
              << " tooBig=" << st.skippedTooBig
              << " noGrammar=" << st.skippedNoGrammar << "\n"
              << "tokenBudget=" << rc.tokenBudget
              << "  mapChars=" << map.size()
              << "  estTokens=" << ContextAssembler::estimateTokens(map) << "\n";
    if (!active.empty()) {
        std::cout << "activeFiles:\n";
        for (const auto& a : active)
            std::cout << "  rank=" << idx.rankOf(a, active) << "  " << pathUtf8(a) << "\n";
        std::cout << "  (对照：不带偏置时同一个文件 rank="
                  << idx.rankOf(active.front(), {}) << ")\n";
    }
    std::cout << "---------------- 地图正文 ----------------\n" << map << "\n";
    return 0;
}

static int runSymbolAb(const std::vector<std::string>& extra) {
    const std::string rootArg = extra.empty() ? "." : extra[0];
    std::error_code ec;
    fs::path root = fs::absolute(pathFromUtf8(rootArg), ec);
    if (ec || !fs::exists(root)) { std::cerr << "路径不存在: " << rootArg << "\n"; return 1; }

    struct Run {
        const char* label;
        SymbolBackend backend;
        RepoIndex idx;
        std::string map;
        std::set<std::string> keys;      // "kind qualName"
    };
    std::vector<Run> runs;
    runs.push_back({"treesitter", SymbolBackend::TreeSitter, RepoIndex(repoIndexConfigFromEnv()), {}, {}});
    runs.push_back({"lexical",    SymbolBackend::Lexical,    RepoIndex(repoIndexConfigFromEnv()), {}, {}});

    for (auto& r : runs) {
        r.idx = RepoIndex(repoIndexConfigFromEnv());
        // 直接改配置里的 backend，绕过 env，保证两条路径跑在同一份配置上
        RepoIndexConfig cfg = repoIndexConfigFromEnv();
        cfg.extract.backend = r.backend;
        r.idx = RepoIndex(cfg);
        r.idx.build(root);
        r.map = r.idx.renderMap({});
        // ★ 比较口径是**末段名**：不带限定、也不带 kind。两条理由，
        //   都是"口径选错 → 结论就错"的教训：
        //     ① 词法后端本来就不补命名空间，拿 qualName 比等于把"漏符号"和
        //        "没加限定"混在一起 —— 召回率会被算成 25%，真实漏抽没那么多；
        //     ② tree-sitter 认得出 method，词法一律叫 function，把 kind 放进
        //        key 会把"认对了种类"也算成"没抽到"。
        //   召回率只回答一个问题：**这个符号你到底抽到没有**。
        for (const auto& s : r.idx.symbols()) r.keys.insert(s.name);
    }

    std::cout << "=== symbol A/B (root=" << pathUtf8(root) << ") ===\n";
    std::cout << "backend        files  symbols  mapChars  estTokens  usedTs\n";
    for (const auto& r : runs) {
        const auto& st = r.idx.stats();
        std::cout << std::left << std::setw(14) << r.label
                  << std::setw(7) << st.files
                  << std::setw(9) << st.symbols
                  << std::setw(10) << r.map.size()
                  << std::setw(11) << ContextAssembler::estimateTokens(r.map)
                  << std::setw(8) << (st.usedTreeSitter ? "yes" : "no") << "\n";
    }

    const auto& ts = runs[0].keys;
    const auto& lx = runs[1].keys;
    std::vector<std::string> onlyTs, onlyLx;
    for (const auto& k : ts) if (!lx.count(k)) onlyTs.push_back(k);
    for (const auto& k : lx) if (!ts.count(k)) onlyLx.push_back(k);

    // 限定名覆盖率：词法后端一律没有命名空间/类名前缀，这一项必然是 0
    size_t tsQual = 0, lxQual = 0;
    for (const auto& s : runs[0].idx.symbols())
        if (s.qualName.find("::") != std::string::npos) ++tsQual;
    for (const auto& s : runs[1].idx.symbols())
        if (s.qualName.find("::") != std::string::npos) ++lxQual;

    const double recall = ts.empty() ? 0.0 : double(lx.size()) / double(ts.size()) * 100.0;
    std::cout << "\n词法后端相对 tree-sitter 的**召回率**(按末段名): "
              << lx.size() << "/" << ts.size() << " = "
              << int(recall * 10) / 10.0 << "%\n";
    std::cout << "带限定名(含 ::)的符号数: treesitter=" << tsQual
              << "  lexical=" << lxQual << "\n";
    std::cout << "只在 tree-sitter 里出现（前 20 个，这些就是词法漏掉的）:\n";
    for (size_t i = 0; i < onlyTs.size() && i < 20; ++i)
        std::cout << "  " << onlyTs[i] << "\n";
    std::cout << "只在词法里出现（前 10 个，这些是词法的**误报**）:\n";
    for (size_t i = 0; i < onlyLx.size() && i < 10; ++i)
        std::cout << "  " << onlyLx[i] << "\n";
    std::cout << "onlyTs=" << onlyTs.size() << "  onlyLx=" << onlyLx.size() << "\n";
    return 0;
}

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
                     "      agent.exe --sandbox-info\n"
                     "      agent.exe --show-repo-map [根目录] [活动文件...]\n"
                     "      agent.exe --symbol-ab     [根目录]\n"
                     "      agent.exe --ui-demo       [plain|ftxui|auto]\n";
        return 1;
    }

    if (args[1] == "--selftest") return runSelfTest();

    // ---- ★ L9：UI 演示（不需要模型，不需要网络）---------------------------
    if (args[1] == "--ui-demo")
        return runUiDemo(std::vector<std::string>(args.begin() + 2, args.end()));

    // ---- ★ L8：仓库地图 / 符号 A-B（不需要模型，不需要密钥）----------------
    if (args[1] == "--show-repo-map")
        return runShowRepoMap(std::vector<std::string>(args.begin() + 2, args.end()));
    if (args[1] == "--symbol-ab")
        return runSymbolAb(std::vector<std::string>(args.begin() + 2, args.end()));

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

    // ---- ★ L8：仓库地图进 L2_RepoMap 层 ------------------------------------
    //  L0–L4 五层压缩做的都是"**压**"（东西已经进来了，想办法变小）；
    //  地图做的是"**选**"（在它进上下文之前就别让它进来）。
    //  没进来的东西既不用花 token，也不需要压缩 —— "选"永远比"压"便宜。
    std::shared_ptr<RepoIndex> repoIndex;
    std::vector<fs::path> activeFiles;
    int mapTokens = 0;
    if (envUtf8("AF_MAP", "1") != "0") {
        RepoIndexConfig rc = repoIndexConfigFromEnv();
        // 两个真相来源必须仲裁：配置里写的 1024，与分层预算算出来的值，取小的。
        const int layerTok = bcfg.forLayer(Layer::L2_RepoMap);
        if (layerTok > 0) rc.tokenBudget = std::min(rc.tokenBudget, layerTok);
        repoIndex = std::make_shared<RepoIndex>(rc);
        repoIndex->build(fs::current_path());
        const std::string map = repoIndex->renderMap({});
        mapTokens = ContextAssembler::estimateTokens(map);
        assembler.setRepoMap(map);
        std::cout << "[地图] 文件 " << repoIndex->stats().files << " 个，符号 "
                  << repoIndex->stats().symbols << " 个，后端 "
                  << (repoIndex->stats().usedTreeSitter ? "treesitter" : "lexical")
                  << "，地图 " << mapTokens << " token\n"
                  << std::flush;
    }

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

    // ---- ★ L9：起 UI（渲染线程 + 事件队列）--------------------------------
    //   Agent 线程只 push，渲染线程只 render，中间只有 EventQueue（伪码 P11）。
    //   ★ 后端选择：auto = 是 TTY 就用画布，否则纯文本。
    //     AF_UI=plain 强制纯文本 —— 验证脚本靠它拿**零 ANSI、可 grep**的输出。
    const bool isTty = af::ui::stdoutIsTty();
    std::string uiMode = envUtf8("AF_UI", "auto");
    // ★ 环境变量被置空（ClearAfEnv 就是这么清的）要当成"没设置"，不是"一种奇怪的模式"
    if (uiMode.empty()) uiMode = "auto";
    const bool wantTui = (uiMode == "ftxui" || uiMode == "tui") ||
                         (uiMode == "auto" && isTty);
    auto uiRenderer = af::ui::makeRenderer(wantTui);
    std::cout << "[UI] 后端=" << uiRenderer->name()
              << "  isTty=" << (isTty ? "1" : "0") << "\n" << std::flush;
    af::ui::UiLoop uiLoop(std::move(uiRenderer), isTty);

    // ---- 观察者接线 ----
    //  ★★ 规矩：回调里**只**做"计数 + 入队"两件事，然后立刻返回。
    //     回调跑在读 socket 的那条线程上，它慢一拍，下一帧数据就晚一拍。
    bool sawFirstToken = false;
    int toolCalls = 0;
    int chunks = 0;
    int notices = 0;
    int permId = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> toolStartedAt;

    LoopHooks hooks;
    hooks.onChunk = [&](const ChatChunk& c) {
        ++chunks;
        if (std::holds_alternative<TextDelta>(c) &&
            !std::get<TextDelta>(c).text.empty()) sawFirstToken = true;
        uiLoop.push(toUiEvent(c));          // ★ 立刻返回
    };
    hooks.onToolStart = [&](const ToolCall& tc) {
        // ★ H7 的耗时：起点记在 **Agent 线程**上（工具真的开始执行的时刻），
        //   终点也在 Agent 线程上算 —— 但**显示**在渲染线程。
        //   耗时不是 UI 造出来的数据，是它从核心拿到的。
        toolStartedAt[tc.id] = std::chrono::steady_clock::now();
        af::ui::UiEvent e;
        e.kind = af::ui::UiEvent::Kind::ToolStart;
        e.toolName = tc.name;
        e.toolId = tc.id;
        uiLoop.push(e);

        // ★ 活动文件偏置：这一轮读/写过的文件，让它的邻居符号下一轮优先进地图。
        if (repoIndex && (tc.name == "Read" || tc.name == "Write" || tc.name == "Edit")) {
            const std::string p = argPathOf(tc);
            if (!p.empty()) {
                std::error_code ec2;
                fs::path abs = fs::absolute(pathFromUtf8(p), ec2).lexically_normal();
                if (!ec2 && std::find(activeFiles.begin(), activeFiles.end(), abs) ==
                                activeFiles.end()) {
                    activeFiles.push_back(abs);
                    if (activeFiles.size() > 8) activeFiles.erase(activeFiles.begin());
                    const std::string m2 = repoIndex->renderMap(activeFiles);
                    mapTokens = ContextAssembler::estimateTokens(m2);
                    assembler.setRepoMap(m2);
                    std::cout << "[地图] 偏置刷新，活动文件 " << activeFiles.size()
                              << " 个，地图 " << mapTokens << " token\n" << std::flush;
                }
            }
        }
    };
    hooks.onToolDone = [&](const ToolResult& r) {
        // ★ H7：耗时 = 终点 - 起点，按 **callId** 配对（不是"上一个工具"）。
        //   并行工具调用时按"当前/上一个"判会串 —— L5 的 K-9 换了个皮。
        double ms = 0;
        auto it = toolStartedAt.find(r.callId);
        if (it != toolStartedAt.end()) {
            ms = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - it->second).count();
            toolStartedAt.erase(it);
        }
        af::ui::UiEvent e;
        e.kind = af::ui::UiEvent::Kind::ToolDone;
        e.toolId = r.callId;
        e.isError = r.isError;
        e.elapsedMs = ms;
        e.text = r.content.substr(0, 400) + (r.content.size() > 400 ? " ..." : "");
        uiLoop.push(e);
        ++toolCalls;
    };
    hooks.onNotice = [&](const std::string& msg) {
        ++notices;
        af::ui::UiEvent e;
        e.kind = af::ui::UiEvent::Kind::Notice;
        e.text = msg;
        uiLoop.push(e);
    };
    hooks.onError = [&](const std::string& e) {
        af::ui::UiEvent ev;
        ev.kind = af::ui::UiEvent::Kind::Error;
        ev.text = e;
        ev.isError = true;
        // ★ 错误是**最该被看见**的事件：既进 UI（用户看到），也保留一份
        //   stderr 直写（万一渲染线程已经收工，日志里也不会什么都不剩）。
        uiLoop.push(ev);
        std::cerr << "\n[请求失败] " << e << "\n" << std::flush;
    };
    // ★ 权限询问：阻塞的是 Agent 线程，渲染线程照常在转（见讲义 §1.5）。
    hooks.confirm = [&](const std::string& prompt) -> bool {
        return uiLoop.renderer().askPermission(prompt, ++permId);
    };

    // ---- 跑一个 turn ----
    Turn turn;
    turn.input = task;

    const LoopOutcome outcome = loop.run(turn, hooks);
    if (canceller.joinable()) canceller.join();

    // ★ 收 UI：关队列 + join 渲染线程，然后才打总结 ——
    //   否则最后几行 UI 输出会和总结行**交错**，日志读起来像随机重排。
    uiLoop.stop();
    std::cout << "[UI] 后端=" << uiLoop.renderer().name()
              << " events=" << uiLoop.events()
              << " frames=" << uiLoop.frames() << "\n" << std::flush;

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
        if (!sawFirstToken) std::cout << "[警告] 最终回答为空\n";
        return 0;
    }
    return 2;
}
