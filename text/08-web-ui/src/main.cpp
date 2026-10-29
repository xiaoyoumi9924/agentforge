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
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
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
#include "session_store.hpp"
#include "subagent.hpp"
#include "symbol_extract.hpp"
#include "trace.hpp"
#include "tool_registry.hpp"
#include "types.hpp"
#include "ui/tui.hpp"
#include "ui/tui_web.hpp"     // ★ 题目八：--web / AF_UI=web 的入口
#include "ui/web_json.hpp"    // ★ 题目八：I-95 直接断言事件编码
#include "ui/web_server.hpp"  // ★ 题目八：I-100 用假浏览器验证"关掉不崩"

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
//  ★ 可热更新的 provider（设置面板用）
//
//  ★ 为什么需要一层壳：AgentLoop / LlmSummarizer / Task 工具持有的是
//    `IModelProvider&`（引用），引用一旦绑上就换不了对象。
//    要让"浏览器里改了 API Key，下一轮立刻生效"，只能换**壳里的实现**，
//    壳本身不动 —— 这跟 L12 用装饰器包 provider 是同一手法。
//    （代价：多一层虚调用。一次模型请求动辄几百毫秒，这里省不出东西来。）
// ---------------------------------------------------------------------------
class HotProvider final : public IModelProvider {
public:
    explicit HotProvider(const ProviderConfig& cfg) : cfg_(cfg) { rebuild(); }

    // 换配置：造不出新 provider（配置离谱）就保持旧的，绝不让进程崩在半路
    bool reconfigure(const ProviderConfig& cfg) {
        auto old = std::move(cfg_);
        cfg_ = cfg;
        if (!rebuild()) { cfg_ = old; rebuild(); return false; }
        return true;
    }

    const ProviderConfig& config() const { return cfg_; }

    std::string_view id() const override { return inner_->id(); }
    int  contextWindow() const override { return inner_->contextWindow(); }
    bool supportsTools() const override { return inner_->supportsTools(); }
    bool supportsStreaming() const override { return inner_->supportsStreaming(); }
    bool supportsPromptCaching() const override { return inner_->supportsPromptCaching(); }

    ChatResponse chat(const ChatRequest& req) override { return inner_->chat(req); }
    ChatResponse stream(const ChatRequest& req,
                        const std::function<bool(const ChatChunk&)>& onChunk) override {
        return inner_->stream(req, onChunk);
    }
    int countTokens(std::string_view text) override { return inner_->countTokens(text); }

private:
    bool rebuild() {
        try {
            inner_ = createProvider(cfg_);
            return inner_ != nullptr;
        } catch (...) {
            return false;
        }
    }

    ProviderConfig cfg_;
    std::unique_ptr<IModelProvider> inner_;
};


// ---------------------------------------------------------------------------
//  端口 ④ 的朴素实现：终端
// ---------------------------------------------------------------------------
class ConsoleUI : public IUIPort {
public:
    // ★ 题目八：把"问用户"这件事转交给当前渲染器。
    //   设了才转交；不设就还是原来的终端行为 —— 既有用例逐字节不变。
    void setConfirm(std::function<bool(const std::string&)> f) { confirmFn_ = std::move(f); }

    bool confirm(const std::string& prompt) override {
        if (confirmFn_) return confirmFn_(prompt);
        std::cout << "\n[权限] " << prompt << "\n  允许? [y/N] " << std::flush;
        std::string line;
        // ★ 拿不到输入 → 返回 false。没有明确批准就不执行（fail-closed）。
        if (!std::getline(std::cin, line)) return false;
        return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
    }
    void notice(const std::string& msg) override {
        std::cout << "[通知] " << msg << "\n" << std::flush;
    }

private:
    std::function<bool(const std::string&)> confirmFn_;
};

// ===========================================================================
//  内置工具：Read / Write / Edit / Bash
// ===========================================================================

std::string readFileWithLineNumbers(const std::string& path, int maxLines = 2000) {
    // ★ 路径必须走 pathFromUtf8：std::ifstream 的窄路径按 ANSI(GBK) 解释，
    //   中文路径会永远"不存在"。见 pathFromUtf8 的注释（K-7）。
    std::ifstream ifs(pathFromUtf8(path), std::ios::binary);
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

// ★ 失败要说清楚"为什么"：ofstream 只告诉你"打不开"，errno 才告诉你是
//   拒绝访问（EACCES）、路径不存在（ENOENT）还是磁盘满。没有它，用户只能猜。
//   这条是"错误要可诊断"的落地：同一个坑第二次踩到时，5 秒能定位。
static std::string errnoHint() {
    if (errno == 0) return {};
    return std::string("（") + std::strerror(errno) + "，errno=" +
           std::to_string(errno) + "）";
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
        const fs::path p = pathFromUtf8(path);
        if (p.has_parent_path())
            fs::create_directories(p.parent_path(), ec);
        errno = 0;
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs) return ToolResult::error("", "无法写入文件: " + path + errnoHint());
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

        std::ifstream ifs(pathFromUtf8(path), std::ios::binary);
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
        errno = 0;
        std::ofstream ofs(pathFromUtf8(path), std::ios::binary | std::ios::trunc);
        if (!ofs) return ToolResult::error("", "无法写回文件: " + path + errnoHint());
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
// ---------------------------------------------------------------------------
//  ★ L12：八层占用
//
//  **先记字节、最后换算一次 token**（K-46）：
//  每层各自 estimateTokens 会把 +1 的舍入误差累积七次，分层加不出总数 ——
//  而"分层加不出总数"的分层图会让人以为看见了全貌，比没有图更危险。
// ---------------------------------------------------------------------------
static af::LayerChars layerCharsOf(const std::string& system,
                                   const std::string& knowledge,
                                   const std::string& repoMap,
                                   const std::string& retrieved,
                                   const Turn& turn,
                                   const std::vector<ToolDef>& tools) {
    af::LayerChars c;
    c.l0 = static_cast<long long>(system.size());
    c.l1 = static_cast<long long>(knowledge.size());
    c.l2 = static_cast<long long>(repoMap.size());
    c.l3 = static_cast<long long>(retrieved.size());
    for (const auto& m : turn.history) {
        c.l5 += static_cast<long long>(m.content.size());
        for (const auto& tc : m.toolCalls)
            c.l5 += static_cast<long long>(tc.args.dump().size());
    }
    c.l6 = static_cast<long long>(turn.input.size());
    // ★ 工具表也是要发出去的字节 —— 20 个工具带完整 JSON Schema
    //   轻松上万字符。漏掉它，分层图会「看起来很省」，而账单不这么认为。
    for (const auto& t : tools)
        c.l7 += static_cast<long long>(t.name.size()) +
                static_cast<long long>(t.description.size()) +
                static_cast<long long>(t.inputSchema.dump().size());
    return c;
}

int runSelfTest() {
    int pass = 0, fail = 0;
    // ★ 自检必须与环境无关：外部 AF_UI=web 会让 I-44~I-49 的"后端是 plain"失配
#ifdef _WIN32
    _putenv_s("AF_UI", "plain");
#else
    setenv("AF_UI", "plain", 1);
#endif
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

    // =======================================================================
    //  ★ 题目八：Web UI 后端（事件编码 / 端口降级 / fail-closed）
    //
    //  ★ 这几条为什么**不需要浏览器**也能断言：
    //    · JSON 编码是纯函数；
    //    · "端口被占要降级"只需要两个渲染器抢同一个端口；
    //    · "没有浏览器时权限必须立刻拒绝"只需要一个没人连过的渲染器。
    //    真正非浏览器不可的部分（点弹窗、看流式）留给 verify.ps1 和当堂演示。
    // =======================================================================
    std::cout << "\n===== 题目八自检：Web UI 后端（编码 / 降级 / fail-closed）=====\n";
    {
        using af::ui::UiEvent;

        // I-95：★ 转义错了，浏览器整条 JSON 都解析不了。这种错不报错，
        //       只表现成"页面一动不动"，是最难查的一类，所以必须自检。
        UiEvent je;
        je.kind = UiEvent::Kind::Text;
        je.text = "他说：\"改完了\"\n第二行\t带制表符";
        const std::string js = af::ui::eventToJson(je);
        check(js.find("\\\"") != std::string::npos &&
              js.find("\\n") != std::string::npos &&
              js.find("\\t") != std::string::npos &&
              js.find('\n') == std::string::npos,
              "I-95 ★ 事件编码：引号/换行/制表符全转义，正文里不许留裸换行");
        check(js.find("改完了") != std::string::npos,
              "I-95b UTF-8 正文原样透传（浏览器按 charset=utf-8 解）");

        // I-96：web 后端能起来（固定端口，避开演示用的 8080）
#ifdef _WIN32
        _putenv_s("AF_WEB_PORT", "18099");
#endif
        auto web1 = af::ui::makeWebRenderer(18099);
        check(web1 != nullptr && std::string(web1->name()) == "web",
              "I-96 web 后端能在 127.0.0.1:18099 起来（name()==\"web\"）");

        // I-97：★ 端口被占 → makeRenderer 必须退回 plain，而且**永远非空**。
        //       这是 I-44/I-46 那条不变量在第三个后端上的延续。
#ifdef _WIN32
        _putenv_s("AF_UI", "web");
#endif
        auto web2 = af::ui::makeRenderer(false);
        check(web2 != nullptr && std::string(web2->name()) == "plain",
              "I-97 ★ 端口被占时 makeRenderer 退回 plain（降级不是瘫痪）");

        // I-97b：★ 题目要求"无浏览器时退回 PlainRenderer" ——
        //        AF_WEB_WAIT_MS 就是那条判据：给浏览器 N 毫秒，等不到就退回 plain。
#ifdef _WIN32
        _putenv_s("AF_WEB_PORT", "18098");
        _putenv_s("AF_WEB_WAIT_MS", "1");
#endif
        auto web3 = af::ui::makeRenderer(false);
        check(web3 != nullptr && std::string(web3->name()) == "plain",
              "I-97b ★ 等不到浏览器（AF_WEB_WAIT_MS=1）→ 退回 plain");
        web3.reset();
#ifdef _WIN32
        _putenv_s("AF_WEB_WAIT_MS", "");
#endif

        // I-102：★ 验收里最常见的那条姿势：只设 AF_UI=web，**不额外设**任何变量，
        //        也没有浏览器。默认等 3 秒 → makeRenderer 必须真的返回 PlainRenderer。
        //        （修之前：waitMs 默认 0 = 不等，于是永远返回 WebRenderer，
        //          "无浏览器时退回 plain"只在手动设了 AF_WEB_WAIT_MS 时才成立。）
#ifdef _WIN32
        _putenv_s("AF_WEB_PORT", "18097");
        _putenv_s("AF_WEB_WAIT_MS", "");
        _putenv_s("AF_WEB_REQUIRE_CLIENT", "");
#endif
        auto web4 = af::ui::makeRenderer(false);
        check(web4 != nullptr && std::string(web4->name()) == "plain",
              "I-102 ★ 纯 AF_UI=web + 无浏览器 → 默认 3s 后返回 plain（不用手动设变量）");
        web4.reset();

        // I-102b：★★ 但 --web 这条路径**不能**被降级判据误伤：
        //         用户双击 .bat（或敲 --web）后总要花几秒才打开浏览器，
        //         这里要是也等 3 秒退回 plain，演示就直接变成"终端 UI"了。
#ifdef _WIN32
        _putenv_s("AF_WEB_PORT", "18097");
        _putenv_s("AF_WEB_REQUIRE_CLIENT", "1");
#endif
        auto web5 = af::ui::makeRenderer(false);
        check(web5 != nullptr && std::string(web5->name()) == "web",
              "I-102b ★★ --web（REQUIRE_CLIENT=1）无浏览器时仍给 web，不误降级");
        web5.reset();
#ifdef _WIN32
        _putenv_s("AF_WEB_REQUIRE_CLIENT", "");
        _putenv_s("AF_WEB_PORT", "");
#endif

        // I-98：★ 没有浏览器时权限询问必须立刻 fail-closed。
        //       在这里阻塞 = 整轮任务卡死，而屏幕上什么都看不见。
        const auto t0 = std::chrono::steady_clock::now();
        const bool allowed = (web1 != nullptr) && web1->askPermission("Bash(dir)", 7);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        check(web1 && !allowed && ms < 1000.0,
              "I-98 ★ 无浏览器时权限询问 1 秒内 fail-closed（不阻塞 Agent 线程）");

        // I-99：★ web 的 render() 必须返回空串 —— 否则 UiLoop 会把它当"帧"
        //       写给 stdout，既有那些"stdout 可 grep"的用例全乱。
        if (web1) {
            UiEvent e2;
            e2.kind = UiEvent::Kind::ToolStart;
            e2.toolName = "Read"; e2.toolId = "selftest-1";
            web1->apply(e2);
            check(web1->render().empty() && web1->toolInFlight(),
                  "I-99 ★ web 的 render() 返回空串（不污染 stdout），toolInFlight 正确");
            UiEvent e3;
            e3.kind = UiEvent::Kind::ToolDone;
            e3.toolId = "selftest-1";
            web1->apply(e3);
            check(!web1->toolInFlight(), "I-99b 工具结束后 inFlight 归位");
        }

        // I-100：★★ 题目验收第 3 条"关掉浏览器，Agent 不崩（降级到 plain）"。
        //   I-98 只证明"从来没连过"；这里证明**连上又关掉**这条更难的路径：
        //   读线程必须自己察觉对端没了（recv=0 → 收尸 → onDisconnect），
        //   然后渲染器退回"终端镜像 + 权限 fail-closed"，而不是卡死或崩溃。
        if (web1) {
            const bool handshook = af::web::probeClientConnect(18099, 200);
            int left = -1;
            for (int i = 0; i < 40; ++i) {          // 最多等 2s 让读线程收尸
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                left = af::ui::webClientCount(*web1);
                if (left == 0) break;
            }
            check(handshook && left == 0,
                  "I-100 ★★ 浏览器关掉后服务端自己察觉（握手成功 → clientCount 归 0，不崩）");

            // 断开之后：权限询问必须仍然 fail-closed，而且**立刻**返回。
            // 这里要是挂住，真实场景就是"用户关掉浏览器，Agent 永远停在允许弹窗"。
            const auto t1 = std::chrono::steady_clock::now();
            const bool allowed2 = web1->askPermission("Bash(dir)", 9);
            const double ms2 = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t1).count();
            check(!allowed2 && ms2 < 1000.0,
                  "I-101 ★ 断开后权限询问仍 fail-closed（1 秒内返回，不卡 Agent 线程）");

            // 断开之后：事件还要能"落到终端"（apply 的 !live 镜像分支）且不崩。
            UiEvent e4;
            e4.kind = UiEvent::Kind::Text;
            e4.text = "（浏览器断开后仍然继续输出）";
            web1->apply(e4);
            check(web1->render().empty(),
                  "I-101b ★ 断开后 apply/render 照常（镜像到终端，render 仍返回空串）");
        }
        // I-103：★ 只绑 127.0.0.1 挡不住"本机浏览器里的另一个网站"：
        //        恶意页面可以直接 new WebSocket("ws://127.0.0.1:8080/ws") 连上来，
        //        然后收弹窗、替你点「允许」。所以握手必须验 Origin。
        //        ★ 放在最后跑：前面的 I-98/I-100/I-101 都依赖 clientCount==0，
        //          这里故意开连接，不能插在它们前面。
        check(!af::web::probeClientConnect(18099, 0, "http://evil.example"),
              "I-103 ★ 跨站来源（Origin: http://evil.example）握手被拒，拿不到 101");
        check(!af::web::probeClientConnect(18099, 0, "http://127.0.0.1:9999"),
              "I-103b ★ 同机不同端口（页面不是从这个端口加载的）也被拒");
        check(af::web::probeClientConnect(18099, 60, "http://127.0.0.1:18099"),
              "I-103c ★ 本机同端口来源（真浏览器）照常握手 101");

        // I-104：★★ 回答真的被采纳。前面几条只证明"没浏览器时立刻拒绝"，
        //        这条把完整链路走一遍：连接还在 → Agent 进入等待 → 浏览器发
        //        permAnswer{allow:true} → askPermission 返回 true。
        //        ★ 顺序要卡准：askPermission 在没有连接时会立刻 fail-closed（I-98），
        //          所以先起发送线程、等 clientCount>0，再让 Agent 进入等待。
#ifdef _WIN32
        _putenv_s("AF_WEB_PERM_TIMEOUT_MS", "4000");
#endif
        {
            std::atomic<bool> sent{false};
            std::thread sender([&] {
                sent.store(af::web::probeClientSend(
                    18099, "http://127.0.0.1:18099",
                    "{\"t\":\"permAnswer\",\"id\":42,\"allow\":true}", 50, 400));
            });
            for (int i = 0; i < 200 && af::ui::webClientCount(*web1) == 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const bool allowed = web1->askPermission("Bash(dir)", 42);
            sender.join();
            check(sent.load() && allowed,
                  "I-104 ★★ 浏览器回答「允许」→ askPermission 返回 true（回答被采纳）");
        }
        // I-104b：同一条路径回答「拒绝」→ 必须返回 false（不是把 true 写死）
        {
            std::atomic<bool> sent{false};
            std::thread sender([&] {
                sent.store(af::web::probeClientSend(
                    18099, "http://127.0.0.1:18099",
                    "{\"t\":\"permAnswer\",\"id\":43,\"allow\":false}", 50, 400));
            });
            for (int i = 0; i < 200 && af::ui::webClientCount(*web1) == 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const bool allowed = web1->askPermission("Bash(dir)", 43);
            sender.join();
            check(sent.load() && !allowed,
                  "I-104b ★ 回答「拒绝」→ askPermission 返回 false（拒绝路径也是真的）");
        }
#ifdef _WIN32
        _putenv_s("AF_WEB_PERM_TIMEOUT_MS", "");
#endif

        // I-105：★★ 多会话边界（一）：运行中再提交，**不能覆盖**上一条。
        //   以前服务端只有一个"待处理任务槽"：A 在跑、B 提交、C 提交 → B 直接消失，
        //   而且界面上一点提示都没有。现在是 FIFO：两条都在，顺序还是提交顺序。
        {
            using af::ui::webWaitForTask;
            const bool s1 = af::web::probeClientSend(
                18099, "http://127.0.0.1:18099",
                "{\"t\":\"task\",\"s\":\"第一条\",\"cid\":\"cA\"}", 40);
            const bool s2 = af::web::probeClientSend(
                18099, "http://127.0.0.1:18099",
                "{\"t\":\"task\",\"s\":\"第二条\",\"cid\":\"cB\"}", 40);
            std::string t1, t2, k1, k2;
            const bool g1 = webWaitForTask(*web1, t1, k1, 2000);
            const bool g2 = webWaitForTask(*web1, t2, k2, 2000);
            check(s1 && s2 && g1 && g2 &&
                      t1 == "第一条" && k1 == "cA" && t2 == "第二条" && k2 == "cB",
                  "I-105 ★★ 连提两条任务：都还在，且按提交顺序出队（不再互相覆盖）");
        }

        // I-105b：★★ 多会话边界（二）：事件必须带 cid 才能"按所属对话入库"。
        //   前端就是靠这个字段分拣的 —— 没有它，只能按"当前选中对话"存，
        //   运行中切一下对话就串味。
        {
            af::ui::webNotice(*web1, "多会话打标探针");
            std::string frames;
            const bool got = af::web::probeClientRead(
                18099, "http://127.0.0.1:18099", frames, 400);
            check(got && frames.find("\"cid\":\"cB\"") != std::string::npos &&
                      frames.find("多会话打标探针") != std::string::npos,
                  "I-105b ★★ 事件带 cid（上一轮是 cB），前端据此把消息存进它所属的对话");
        }

        web1.reset();
        web2.reset();

#ifdef _WIN32
        _putenv_s("AF_UI", "plain");        // 复原：后面的用例还要按 plain 断言
        _putenv_s("AF_WEB_PORT", "");
#endif
    }

    // =======================================================================
    //  ★ L10：会话持久化（模块 ⑫）
    //
    //  ★ 这几条为什么进 --selftest（而不是只留在 verify.ps1）：
    //    "半行损坏"、"压缩到一半"这类边界**不需要模型也不需要网络**，
    //    只需要一份能构造出来的 JSONL。凡不需要外部依赖就能验证的，就该进自检
    //    （L7 定下的规矩：只能联网验证的能力，等于没被验证过）。
    // =======================================================================
    std::cout << "\n===== L10 自检：会话持久化（JSONL / 恢复 / T3）=====\n";
    {
        std::error_code ec2;
        const fs::path sdir = fs::temp_directory_path(ec2) / "af_session_selftest";
        fs::remove_all(sdir, ec2);
        fs::create_directories(sdir, ec2);

        auto writeLines = [](const fs::path& p, const std::vector<std::string>& lines) {
            std::ofstream f(p, std::ios::binary);
            for (const auto& l : lines) f << l << '\n';
        };
        auto loadFile = [&](const std::string& name) {
            auto st = af::SessionStore::open(sdir, name);
            return st->load();
        };

        // ---- I-50：往返 ----
        {
            auto st = af::SessionStore::open(sdir, "round");
            Message a; a.role = Role::User; a.content = "第一条";
            Message b; b.role = Role::Assistant; b.content = "第二条";
            st->append(a); st->append(b);
            const auto r = st->load();
            check(r.messages.size() == 2 && r.messages[0].content == "第一条" &&
                  r.messages[1].content == "第二条",
                  "I-50 ★ JSONL 往返：写进去的两条原样读出来");
        }

        // ---- I-51：★ 半行损坏只丢一行（INV-26）----
        {
            const fs::path p = sdir / "half.jsonl";
            writeLines(p, {
                "{\"t\":\"meta\",\"v\":1,\"id\":\"half\"}",
                "{\"seq\":1,\"t\":\"user\",\"content\":\"完好的第一条\"}",
                "{\"seq\":2,\"t\":\"user\",\"content\":\"写到一半就被杀",
            });
            const auto r = loadFile("half");
            check(r.messages.size() == 1 && r.badLines == 1,
                  "I-51 ★★ 半行只丢自己：其余消息照常加载（坏行数要报出来）");
            std::cout << "     [会话] 半行 → 消息 " << r.messages.size()
                      << " 条 / 坏行 " << r.badLines << " 条\n";
        }

        // ---- I-52：★ T3 正解 —— 摘要取代了 seq<=replaced_upto 的消息 ----
        {
            const fs::path p = sdir / "comp.jsonl";
            writeLines(p, {
                "{\"seq\":1,\"t\":\"user\",\"content\":\"旧消息1\"}",
                "{\"seq\":2,\"t\":\"assistant\",\"content\":\"旧消息2\"}",
                "{\"seq\":3,\"t\":\"user\",\"content\":\"交接摘要\"}",
                "{\"seq\":4,\"t\":\"assistant\",\"content\":\"新消息\"}",
                "{\"seq\":5,\"t\":\"compaction\",\"replaced_upto\":2,"
                "\"summary\":\"交接摘要\",\"level\":3}",
            });
            const auto r = loadFile("comp");
            check(r.messages.size() == 2 && r.messages[0].content == "交接摘要",
                  "I-52 ★★ T3：被摘要取代的旧消息不进视图（replaced_upto 生效）");
            check(r.hadSummary, "I-53 用上了交接摘要（可观测）");
        }

        // ---- I-54：★ T3 的反面 —— 半截压缩不能造出空上下文 ----
        {
            const fs::path p = sdir / "part.jsonl";
            writeLines(p, {
                "{\"seq\":1,\"t\":\"user\",\"content\":\"旧消息1\"}",
                "{\"seq\":2,\"t\":\"assistant\",\"content\":\"旧消息2\"}",
                "{\"seq\":3,\"t\":\"compaction\",\"replaced_upto\":2,\"summary\":\"\"}",
            });
            const auto r = loadFile("part");
            check(r.messages.size() == 2 && r.ignoredPartialCompaction,
                  "I-54 ★★ 摘要为空的 compaction 被忽略 → 回退原文（宁可长，不能空）");
        }

        // ---- I-55：★ INV-28 —— 截断不得把上下文截成空 ----
        {
            const fs::path p = sdir / "empt.jsonl";
            writeLines(p, {
                "{\"seq\":1,\"t\":\"user\",\"content\":\"唯一的一条\"}",
                "{\"seq\":2,\"t\":\"compaction\",\"replaced_upto\":1,\"summary\":\"摘要\"}",
            });
            const auto r = loadFile("empt");
            check(r.messages.size() == 1,
                  "I-55 ★★ INV-28：截断后为空则回退不截断（T3 灾难情形的兜底）");
        }

        // ---- I-56：审计型 compaction（replaced_upto < 0）不参与重建 ----
        {
            const fs::path p = sdir / "audit.jsonl";
            writeLines(p, {
                "{\"seq\":1,\"t\":\"user\",\"content\":\"消息1\"}",
                "{\"seq\":2,\"t\":\"assistant\",\"content\":\"消息2\"}",
                "{\"seq\":3,\"t\":\"compaction\",\"replaced_upto\":-1,\"summary\":\"\",\"level\":2}",
            });
            const auto r = loadFile("audit");
            check(r.messages.size() == 2 && !r.hadSummary,
                  "I-56 审计型 compaction 不影响重建（日志层与视图层分开）");
        }

        // ---- I-57：fork —— 原会话不变 ----
        {
            auto st = af::SessionStore::open(sdir, "forkbase");
            Message m; m.role = Role::User; m.content = "原始";
            st->append(m);
            const auto before = fs::file_size(st->meta().jsonlPath, ec2);
            auto fk = st->fork();
            Message n; n.role = Role::User; n.content = "分叉后新增";
            fk->append(n);
            const auto after = fs::file_size(st->meta().jsonlPath, ec2);
            check(before == after, "I-57 ★ fork 后原会话文件字节数不变（复制即分叉）");
            check(fk->load().messages.size() == 2,
                  "I-58 分叉会话继承历史且能继续追加");
        }

        // ---- I-59：★ 检查点 —— 先快照，再改 ----
        {
            const fs::path tgt = sdir / "cp_target.txt";
            {
                std::ofstream f(tgt, std::ios::binary);
                f << "改之前的内容";
            }
            auto st = af::SessionStore::open(sdir, "cp");
            const af::CheckpointEvent cp = st->snapshot({tgt}, "Edit");
            check(cp.snapshots.size() == 1 &&
                  cp.snapshots[0].second == "改之前的内容",
                  "I-59 ★ 检查点快照的是**改之前**的内容（顺序：先快照再改）");
            st->appendCheckpoint(cp);
            {
                std::ofstream f(tgt, std::ios::binary | std::ios::trunc);
                f << "改之后的内容";
            }
            check(st->restore(cp.id), "I-60 restore 找得到那个检查点");
            std::ifstream in(tgt, std::ios::binary);
            std::ostringstream os; os << in.rdbuf();
            check(os.str() == "改之前的内容",
                  "I-61 ★ restore 后文件内容回到改动前（备忘录模式的兑现）");
        }

        // ---- I-62：★ K-36 —— 中文路径进/出 JSON 全程不抛 ----
        //
        //  ★ 注意这里**不能**写 `sdir / "中文目录"`：那是 K-7 的另一半
        //    （`fs::path` 从 char 构造按 GBK 解释），字节解不开就直接抛。
        //    正确姿势是 pathFromUtf8 —— 本用例测的是"**照正确姿势做**会不会抛"，
        //    不是测"照错误姿势做会不会抛"（后者 K-29 已经测过了）。
        {
            std::error_code ec3;
            const fs::path cnDir = sdir / pathFromUtf8("中文目录");
            fs::create_directories(cnDir, ec3);
            const fs::path cn = cnDir / pathFromUtf8("文件名.cpp");
            bool threw = false; std::string err, stage; int snapCount = -1;
            try {
                stage = "建文件";
                { std::ofstream f(cn, std::ios::binary); f << "中文内容"; }
                stage = "open 会话";
                auto st = af::SessionStore::open(sdir, "cn");
                stage = "snapshot（路径进内存）";
                const af::CheckpointEvent cp = st->snapshot({cn}, "Edit");
                snapCount = int(cp.snapshots.size());
                stage = "appendCheckpoint（路径进 JSON）";
                st->appendCheckpoint(cp);
                stage = "restore（路径出 JSON 再变回 fs::path）";
                st->restore(cp.id);
            } catch (const std::exception& ex) {
                threw = true; err = ex.what();
            }
            check(!threw, "I-62 ★★ K-36：中文路径进/出 JSON 全程不抛");
            if (threw) std::cout << "     [会话] 在「" << stage << "」抛了: " << err << "\n";
            else       std::cout << "     [会话] 全程走 pathFromUtf8 / pathUtf8，未抛\n";
            std::cout << "     [会话] exists=" << fs::exists(cn)
                      << "  snapshots=" << snapCount << "\n";

            // ★ 光"不抛"还不够：还要证明确实写进去的是 **UTF-8 字节**。
            //   若哪天有人把 pathUtf8 换成 p.string()，日志里就是 GBK 乱码，
            //   读回来再构造 fs::path 就又炸一次 —— 这条断言守的是"写对"。
            std::ifstream cnr(sdir / "cn.jsonl", std::ios::binary);
            std::ostringstream cs; cs << cnr.rdbuf();
            const std::string all = cs.str();
            const bool ok63 = all.find("文件名.cpp") != std::string::npos;
            check(ok63, "I-63 ★ JSONL 里的路径是 UTF-8 原文（换成 p.string() 就会变乱码）");
            if (!ok63) {
                const auto pos = all.find("\"path\"");
                std::cout << "     [会话] jsonl 里的 path 段 = "
                          << (pos == std::string::npos ? std::string("(没有 path 字段！)")
                                                       : all.substr(pos, 160)) << "\n";
            }

            // ---- I-66：★★ 半行之后追加，新事件不能粘在坏行上 ----
        //
        //  进程被杀时最后一行**没有换行符**。若直接往后追加，新事件会和坏行
        //  拼成一行 → 一起作废，"恢复后总少一条"，而且少的永远是**最新**那条。
        //  修法见 session_store.cpp 的 open()：补一个 '\n' 宣告上一行作废。
        {
            const fs::path p = sdir / "tail.jsonl";
            {
                std::ofstream f(p, std::ios::binary);
                f << "{\"t\":\"meta\",\"v\":1,\"id\":\"tail\"}\n";
                f << "{\"seq\":1,\"t\":\"user\",\"content\":\"完好的\"}\n";
                f << "{\"seq\":2,\"t\":\"user\",\"content\":\"写一半就被杀";  // ★ 故意不加换行
            }
            auto st = af::SessionStore::open(sdir, "tail");
            Message m; m.role = Role::User; m.content = "恢复后新增";
            st->append(m);
            const auto r = st->load();
            check(r.messages.size() == 2 && r.messages[1].content == "恢复后新增",
                  "I-66 ★★ 半行后追加不粘行（补一个换行 = 宣告上一行作废）");
            check(r.badLines == 1, "I-67 那条半行仍然被记成 1 条坏行（没有假装它没发生）");
        }

        // ---- I-64 / I-65：★ 给了 id 就别偷偷改名 ----
            //   （I-63 一开始失败，就是因为这个：open(sdir,"cn") 把 id 改成了 s-<时间戳>，
            //     于是我去读 cn.jsonl 读到空文件。**测试写对了才发现实现在骗人。**）
            {
                std::error_code ec4; fs::remove_all(sdir / "kx.jsonl", ec4);
                auto st = af::SessionStore::open(sdir, "kx");
                check(st->meta().id == "kx" && !st->meta().resumed,
                      "I-64 ★ 指定的 id 不被偷偷改名（改名 = 用户以为在恢复，其实重开）");
                Message m; m.role = Role::User; m.content = "x"; st->append(m);
                auto st2 = af::SessionStore::open(sdir, "kx");
                check(st2->meta().resumed && st2->meta().id == "kx",
                      "I-65 ★ 同一个 id 再打开 → resumed=1（\"真的接着上次\"可断言）");
            }
        }
    }

    // =======================================================================
    std::cout << "\n===== L11 自检：子代理（工具视图 / 摘要契约 / 隔离）=====\n";
    {
        // ---- I-68…I-70：★ readOnlyView 只留只读工具，且**排除 Task**（K-40）----
        ToolRegistry reg;
        NoUI ui;
        reg.registerBuiltin(af::makeTaskToolDef(),
                            [](const json&) { return ToolResult::ok("", "task"); });
        {
            ToolDef d; d.name = "FakeRead"; d.risk = RiskLevel::Read;
            reg.registerBuiltin(d, [](const json&) { return ToolResult::ok("", "read"); });
        }
        {
            ToolDef d; d.name = "FakeWrite"; d.risk = RiskLevel::Write;
            reg.registerBuiltin(d, [](const json&) { return ToolResult::ok("", "write"); });
        }
        {
            ToolDef d; d.name = "FakeExec"; d.risk = RiskLevel::Exec;
            reg.registerBuiltin(d, [](const json&) { return ToolResult::ok("", "exec"); });
        }

        // ★ 不能加 const：dispatch() 非 const（它内部要记状态），
        //   "只读视图"说的是**工具集**只读，不是**对象**只读。
        ToolRegistry ro = reg.readOnlyView();
        const auto rdefs = ro.defs();
        auto has = [&](const std::string& n) {
            return std::any_of(rdefs.begin(), rdefs.end(),
                               [&](const ToolDef& d) { return d.name == n; });
        };
        check(has("FakeRead"), "I-68 只读视图保留了 risk==Read 的工具");
        check(!has("FakeWrite") && !has("FakeExec"), "I-69 ★ 只读视图排除了 Write / Exec");
        check(!has(std::string(kTaskToolName)),
              "I-70 ★★ K-40：只读视图排除了 Task（不给递归能力）");

        // ---- I-71：★ 只读视图里的工具**真的能执行**（闭包跟着复制过来了）----
        //   参考实现的 bug 就是"只复制 def 不复制闭包" → 有说明书没人干活。
        //   ★ 这条断言存在的理由：`defs()` 里**有**这个工具，不等于**能用**它。
        //     两个都对时才算对，只测前者会漏掉那半个 bug。
        {
            ToolCall c; c.id = "t1"; c.name = "FakeRead"; c.args = json::object();
            const ToolResult r = ro.dispatch(c, ui);
            check(!r.isError && r.content == "read",
                  "I-71 ★★ 只读视图里的工具真能执行（闭包一起复制了）");
        }

        // ---- I-72 / I-73：★ 摘要契约里必须出现"绝对路径"和字数上限 ----
        //   丢路径是摘要类操作最常见的失败模式（L7 压缩 / L10 交接 / L11 子代理，
        //   同一条规矩管三处）。契约写在那儿没人看，写成断言才有人守。
        {
            const std::string p(af::kSubagentSummaryPrompt);
            check(p.find("绝对路径") != std::string::npos,
                  "I-72 ★ 子代理摘要契约要求给出绝对路径（丢路径 = 省下的 token 全还回去）");
            check(p.find("800") != std::string::npos,
                  "I-73 摘要有字数上限（无上限的摘要等于没隔离）");
        }

        // ---- I-74：SubagentSpec 的默认必须是"只读 + 有步数上限" ----
        //   ★ 默认值是最容易被忽略的接口：**用默认值的人不会去看文档**。
        {
            af::SubagentSpec s;
            check(s.readOnly && s.maxSteps > 0 && s.maxTokens > 0,
                  "I-74 ★ 子代理默认只读且步数/预算都有上限（不能无限跑）");
        }

        // ---- I-75：Task 工具的说明书里要写清"它是隔离的" ----
        //   模型决定派不派子代理，靠的就是这段描述。
        {
            const ToolDef td = af::makeTaskToolDef();
            const std::string d = td.description;
            check(d.find("独立") != std::string::npos,
                  "I-75 ★ Task 的描述里说清了它是独立上下文的（模型靠这段决定派不派）");
        }
    }

    // =======================================================================
    std::cout << "\n===== L12 自检：旁路埋点（关闭=零代价 / 丢弃可见 / 坏行容忍）=====\n";
    {
        // ---- I-76 / I-77：★ 关闭时必须是**真的**零代价 ----
        //   不只是"不记录"，而是连 requestChars 都不算（见 trace.cpp 的早退）。
        {
            af::Tracer off(false), on(true);
            check(!off.enabled() && on.enabled(), "I-76 Tracer 开关生效");

            const int idOff = off.beginSpan(af::TraceKind::Request, "r");
            const int idOn  = on.beginSpan(af::TraceKind::Request, "r");
            check(idOff == 0, "I-77 ★ 关闭时 beginSpan 返回 0（调用点无需写 if）");
            check(idOn > 0, "I-78 开启时 beginSpan 返回有效 id");

            off.endSpan(idOff, 10, 20, 100);
            off.event(af::TraceKind::Notice, "n");
            on.endSpan(idOn, 10, 20, 100);
            on.event(af::TraceKind::Notice, "n");

            check(off.events().empty(),
                  "I-79 ★★ 关闭时 events 为空（零代价 ≠ 记了但不用）");
            check(on.events().size() == 2, "I-80 开启时事件都进来了");
        }

        // ---- I-81 / I-82：★ 容量有限，**丢弃必须可见** ----
        //   静默采样会让"我看到的不是全部"变成隐形事实 —— 这比没有 trace 更糟。
        {
            af::Tracer t(true, 8);
            for (int i = 0; i < 20; ++i) t.event(af::TraceKind::Notice, "n");
            check(t.events().size() == 8, "I-81 ★ 环形缓冲限住容量（不会无限涨）");
            check(t.dropped() == 12, "I-82 ★★ 丢弃条数报出来了（静默采样是最坏的设计）");
        }

        // ---- I-83：工具数不翻倍（ToolExec span 与 ToolDone event 只能数一个）----
        {
            af::Tracer t(true);
            const int id = t.beginSpan(af::TraceKind::ToolExec, "Read");
            t.endSpan(id);
            t.event(af::TraceKind::ToolDone, "c1", "ok");
            check(t.summary().toolCalls == 1,
                  "I-83 ★ 一次工具执行只数一次（span 与 event 都数会翻倍，且不报错）");
            const int s2 = t.beginSpan(af::TraceKind::Subagent, "sub");
            t.endSpan(s2, 100, 50, 0);
            check(t.summary().subTok == 150 && t.summary().tokIn == 0,
                  "I-84 ★★ 子代理 token 单列，不混进主 token（K-42）");
        }

        // ---- I-85 / I-86：JSONL 坏行容忍（与 L10 同构）----
        {
            std::error_code ec;
            const fs::path dir = fs::temp_directory_path() / "af_trace_selftest";
            fs::create_directories(dir, ec);
            const fs::path p = dir / "t.jsonl";
            {
                std::ofstream o(p, std::ios::binary | std::ios::trunc);
                o << "{\"seq\":1,\"t\":\"request\",\"tok_in\":10,\"tok_out\":5}\n";
                o << "{\"seq\":2,\"t\":\"request\",\"tok_in\":  \"半行坏掉\n";
                o << "{\"seq\":3,\"t\":\"notice\"}\n";
            }
            af::Tracer::Summary s;
            int bad = 0;
            const bool ok = af::Tracer::load(p, s, bad);
            check(ok && bad == 1, "I-85 ★ 坏行只丢自己，且**报出来**（不静默）");
            check(s.tokIn == 10 && s.steps == 1,
                  "I-86 ★ 坏行前后的好行都还在（一行坏不等于整份废）");
        }

        // ---- I-87：★ 追加前补换行（K-37 在 trace 上重演）----
        //   被杀的进程留下的最后一行是**半行且没有换行符**。
        //   不补 '\n' 就直接追加 → 新写的第一行会粘在坏行上一起作废，
        //   症状是"恢复后总少一条"，少的永远是**最新**那条。
        {
            std::error_code ec;
            const fs::path dir = fs::temp_directory_path() / "af_trace_selftest";
            fs::create_directories(dir, ec);
            const fs::path p = dir / "k37.jsonl";
            { std::ofstream o(p, std::ios::binary | std::ios::trunc);
              // ★ 半行 + 末尾无换行 —— 这正是"进程被杀"留下的形状
              o << "{\"seq\":99,\"t\":\"notice\",\"tok_"; }

            af::Tracer t1(true); t1.event(af::TraceKind::Notice, "a"); t1.flush(p);
            af::Tracer t2(true); t2.event(af::TraceKind::Notice, "b"); t2.flush(p);

            af::Tracer::Summary s; int bad = 0;
            af::Tracer::load(p, s, bad);
            // 半行坏掉：bad==1（只丢它自己）；两次 flush 各一条 notice：steps==2。
            // ★ 如果没补换行，t2 的第一行会粘到坏行上 → 变成 bad==1 且 steps==1。
            check(bad == 1 && s.steps == 2,
                  "I-87 ★★ K-37：末尾无换行的半行不会吃掉后面写的行");
        }

        // ---- I-91：★ 同一个 Tracer flush 两次不能重复写 ----
        //   追加型日志最容易犯的错：以为"追加"就是把整个缓冲再写一遍。
        {
            std::error_code ec;
            const fs::path dir = fs::temp_directory_path() / "af_trace_selftest";
            fs::create_directories(dir, ec);
            const fs::path p = dir / "dup.jsonl";
            { std::ofstream o(p, std::ios::binary | std::ios::trunc); }

            af::Tracer t(true);
            t.event(af::TraceKind::Notice, "a");
            t.flush(p);
            t.event(af::TraceKind::Notice, "b");
            t.flush(p);

            af::Tracer::Summary s; int bad = 0;
            af::Tracer::load(p, s, bad);
            check(bad == 0 && s.steps == 2,
                  "I-91 ★ 同一 Tracer 连续 flush 两次不重复（水位生效）");
        }

        // ---- I-88：★ /context 的八层必须加得出总数 ----
        //   算不出总数的分层图是骗人的 —— 它会让人以为看见了全貌。
        {
            ToolRegistry reg;
            reg.setSandbox(nullptr);
            registerBuiltinTools(reg, nullptr, 120'000);
            Turn t; t.input = "hi";
            Message m; m.role = Role::User; m.content = "old";
            t.history.push_back(m);
            const af::LayerChars c = layerCharsOf("sys", "know", "map", "ret", t,
                                                  reg.defs());
            const long long sum = c.l0+c.l1+c.l2+c.l3+c.l4+c.l5+c.l6+c.l7;
            check(sum == c.total(), "I-88 ★ 八层之和 == 报告总数（自洽）");
            check(c.l7 > 0, "I-89 ★ 工具表算进去了（漏了它就「看起来很省」）");
            check(c.l0 == 3 && c.l5 == 3 && c.l6 == 2, "I-90 各层字节数对得上");
        }

        // ---- I-92：★★ 埋点开销必须**量出来**，不能靠"应该很小" ----
        //
        //  为什么不能拿"跑同一个任务两次比耗时"：
        //    一次 HTTP 请求 ~100ms，埋点一次 ~1µs —— 差五个数量级，
        //    网络抖动一毫秒就把信号淹了。**测不出的对比等于没测。**
        //    所以这里直接量单位操作的开销，并且要求**关/开都量**。
        //
        //  ★ 判据有两条，缺一不可：
        //    ① 开启时每事件开销 < 2µs（相对 100ms 的请求可忽略）；
        //    ② 关闭时必须**明显更快** —— 否则"零代价"只是句口号。
        {
            const int N = 50'000;
            auto bench = [&](bool on) {
                af::Tracer t(on, 200'000);
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < N; ++i) {
                    const int id = t.beginSpan(af::TraceKind::ToolExec, "Read");
                    t.endSpan(id, 1, 1, 100);
                    t.event(af::TraceKind::Notice, "n");
                }
                const auto t1 = std::chrono::steady_clock::now();
                return std::chrono::duration<double, std::micro>(t1 - t0).count() /
                       double(N * 3);       // ★ 每次 begin/end/event 算一次
            };
            const double offUs = bench(false);
            const double onUs  = bench(true);
            std::cout << "     [开销] 每事件 关=" << offUs
                      << "us  开=" << onUs << "us  倍数="
                      << (offUs > 0.0001 ? int(onUs / offUs * 10) / 10.0 : 0.0) << "x\n";
            check(onUs < 2.0, "I-92 ★ 开启时每事件开销 < 2us（相对 100ms 请求可忽略）");
            check(onUs > offUs * 2.0,
                  "I-93 ★★ 关闭时明显更快 —— 「零代价」不是口号，是量出来的");
            // ★ K-49 的守卫：事件变多时**单事件开销不能变贵**。
            //   如果这里退化成 O(n)，1k 与 50k 两个数会差出几十倍。
            //   ★ 这条断言的价值在于它测的是**斜率**，不是绝对值 ——
            //     绝对值会被机器快慢带偏，斜率不会。
            auto benchN = [&](int n) {
                af::Tracer t(true, size_t(n) + 10);
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < n; ++i) {
                    const int id = t.beginSpan(af::TraceKind::ToolExec, "Read");
                    t.endSpan(id, 1, 1, 100);
                }
                const auto t1 = std::chrono::steady_clock::now();
                return std::chrono::duration<double, std::micro>(t1 - t0).count() / double(n);
            };
            const double smallUs = benchN(1'000);
            const double bigUs   = benchN(50'000);
            std::cout << "     [开销] 单 span 1k=" << smallUs
                      << "us  50k=" << bigUs << "us\n";
            check(bigUs < smallUs * 5.0 + 1.0,
                  "I-94 ★★ K-49：事件涨 50 倍，单事件开销不能涨（不退化成 O(n)）");
        }
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
    // ★ 题目八：--ui-demo web 也要能走 web —— 让工厂看到同一个环境变量，
    //   而不是再加一条"从参数传后端"的旁路（后端选择只有一处）。
    if (want == "web") {
#ifdef _WIN32
        _putenv_s("AF_UI", "web");
#else
        setenv("AF_UI", "web", 1);
#endif
    }
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

// ---------------------------------------------------------------------------
//  ★ L10：会话持久化的三个可观测入口
//
//    --session-debug <文件.jsonl>   直接读一份日志，把恢复结果打印出来
//    --sessions                     列出 AF_SESSION_DIR 下的所有会话
//
//  ★ 为什么 --session-debug 必须存在（而不是只在 --resume 里间接验证）：
//    T3 那类"半截的压缩"是**极端情形**，靠一次真实对话去碰运气根本碰不到。
//    能直接喂一份构造好的日志进去，它才变成可回归的断言 ——
//    这与 L9 用 --ui-demo 把"工具执行的 1.2 秒空窗"写成脚本是同一个道理：
//    **把时序/边界写成输入，而不是交给运气。**
// ---------------------------------------------------------------------------
static int runSessionDebug(const std::vector<std::string>& extra) {
    if (extra.empty()) { std::cerr << "用法: --session-debug <文件.jsonl>\n"; return 1; }
    std::error_code ec;
    const fs::path p = fs::absolute(pathFromUtf8(extra[0]), ec);
    if (!fs::exists(p, ec)) { std::cerr << "文件不存在: " << extra[0] << "\n"; return 1; }

    // 借 SessionStore 的解析逻辑：open 一个临时会话目录，再把目标文件挪过来读
    // —— 更简单的办法是把 load() 做成能对任意路径工作。这里用 fork 的技巧：
    // 直接 open 该目录 + 该 id。
    const fs::path dir  = p.parent_path();
    const std::string id = p.stem().string();
    auto store = af::SessionStore::open(dir, id);
    const af::LoadResult r = store->load();

    std::cout << "[会话] 文件=" << pathUtf8(p) << "\n"
              << "[会话] id=" << store->meta().id << "\n"
              << "[恢复] 消息 " << r.messages.size() << " 条"
              << "  坏行 " << r.badLines << " 条"
              << "  用到摘要=" << (r.hadSummary ? "1" : "0")
              << "  忽略半截压缩=" << (r.ignoredPartialCompaction ? "1" : "0") << "\n";
    for (std::size_t i = 0; i < r.messages.size(); ++i) {
        std::string c = r.messages[i].content;
        if (c.size() > 60) c = c.substr(0, 60) + "...";
        std::replace(c.begin(), c.end(), '\n', ' ');
        std::cout << "  [" << i << "] " << c << "\n";
    }
    return 0;
}

static int runSessions() {
    const std::string dirArg = envUtf8("AF_SESSION_DIR", "");
    if (dirArg.empty()) { std::cerr << "请设置 AF_SESSION_DIR\n"; return 1; }
    const auto list = af::listSessions(pathFromUtf8(dirArg));
    std::cout << "[会话] 目录=" << dirArg << "  共 " << list.size() << " 个\n";
    for (const auto& m : list)
        std::cout << "  " << m.id << "  created=" << m.createdAt << "\n";
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

// ---------------------------------------------------------------------------
//  ★ L12：仪表盘的三个入口
//
//    --cost    <trace.jsonl>...   花了多少（★ 子代理单列，K-42 的延续）
//    --context [根目录]           上下文被谁占了（八层，含**工具表**）
//    --regress <regress.jsonl>    回归集：通过率 / 成本 / 步数 **三个一起报**
//
//  ★ 为什么三个都要有，而且**必须一起用**：
//    只报通过率 → 会奖励暴力重试（多试几次就过了，成本翻倍没人看见）；
//    只报成本   → 会奖励什么都不做（不调工具当然便宜）。
//    这是古德哈特定律的标准模样：**度量一旦成为目标，它就不再是好度量。**
//    唯一的解法是让"作弊的方向"互相牵制 —— 三个数同时看。
// ---------------------------------------------------------------------------

// 系统提示抽成函数：真实运行与 /context 离线估算**必须用同一份**，
// 否则两处会悄悄漂移，而 /context 报出来的数也就不再是线上的数。
static std::string buildSystemPrompt() {
    return "你是一个编码 Agent。原则：\n"
           "1. 先用 Read 看懂文件，再动手改 —— 不要凭猜测编辑。\n"
           "2. 改完必须跑命令验证，不能只说\"应该好了\"。\n"
           "3. 工具报错是信息不是失败 —— 读它，然后换一种方法。\n"
           "4. 一次只做一件小事，做完就确认结果。\n"
           "5. 卡住就说明卡在哪，不要原样重试同样的调用。\n"
           "6. 上下文可能被压缩过 —— 以「交接摘要」为准，不要重复已经做过的事。\n"
           "7. 本机没有 gcc；C/C++ 用 MSVC，但不要用 cl 一步链接（沙箱里会报 D8018）。\n"
           "   分两步编译并运行：\n"
           "     cl /nologo /utf-8 /c kmp.c\n"
           "     link /NOLOGO /OUT:kmp.exe kmp.obj\n"
           "     kmp.exe\n"
           "   C++ 把 /c 那一步换成 cl /nologo /EHsc /std:c++20 /utf-8 /c a.cpp，再 link a.obj。\n"
           "   如果 link 报 LNK1104（临时目录不可写），先把临时目录指到当前目录：\n"
           "     mkdir _lnktmp & set TMP=%CD%\\_lnktmp& set TEMP=%CD%\\_lnktmp& link ...\n"
           "   不要因为“没有 gcc”就让用户自己装 MinGW，先用 cl + link 验证。\n";
}

// 画一条 30 格的横条。为什么要画：人读"\n  L5 18234"没有直觉，
// 读"[████████        ]"立刻知道历史占了一半。
static std::string bar(long long v, long long total) {
    const int w = 30;
    int n = (total <= 0) ? 0 : int(double(v) / double(total) * w + 0.5);
    if (n > w) n = w;
    return std::string(n, '#') + std::string(w - n, '.');
}

static void printContext(const af::LayerChars& c, const std::string& title) {
    const long long tot = c.total();
    const auto row = [&](const char* name, long long v) {
        std::cout << "  " << std::left << std::setw(22) << name
                  << std::right << std::setw(9) << v
                  << "  [" << bar(v, tot) << "] "
                  << (tot ? int(double(v) / double(tot) * 1000) / 10.0 : 0.0) << "%\n";
    };
    std::cout << title << "\n";
    row("L0 系统提示",      c.l0);
    row("L1 项目知识",      c.l1);
    row("L2 仓库地图",      c.l2);
    row("L3 检索片段",      c.l3);
    row("L4 活动文件",      c.l4);
    row("L5 历史",          c.l5);
    row("L6 当前输入",      c.l6);
    row("L7 工具表",        c.l7);
    // ★ 自洽性断言写在**输出里**：七层加起来必须等于总数。
    //   算不出总数的分层图是骗人的 —— 它会让你以为看见了全貌。
    const long long sum = c.l0 + c.l1 + c.l2 + c.l3 + c.l4 + c.l5 + c.l6 + c.l7;
    std::cout << "  " << std::left << std::setw(22) << "合计"
              << std::right << std::setw(9) << tot
              << "   sum=" << sum
              << (sum == tot ? "  (自洽)" : "  ★不自洽！")
              << "   estTok=" << int(tot / 4) << "\n";
    std::cout << "[上下文] total=" << tot << " chars"
              << "  sum=" << sum
              << "  estTok=" << int(tot / 4)
              << "  L0=" << c.l0 << " L2=" << c.l2 << " L5=" << c.l5
              << " L7=" << c.l7 << "\n";
}

static int runCost(const std::vector<std::string>& extra) {
    if (extra.empty()) {
        std::cerr << "用法: agent.exe --cost <trace.jsonl> [更多...]\n";
        return 1;
    }
    long long tok = 0, sub = 0;
    int req = 0, tools = 0, steps = 0, bad = 0, files = 0;
    for (const auto& f : extra) {
        af::Tracer::Summary s;
        int bl = 0;
        if (!af::Tracer::load(pathFromUtf8(f), s, bl)) {
            std::cout << "[cost] " << f << "  (读不到)\n";
            continue;
        }
        ++files;
        std::cout << "[cost] " << f
                  << "  请求 " << s.requests
                  << "  工具 " << s.toolCalls
                  << "  步数 " << s.steps
                  << "  主token " << (s.tokIn + s.tokOut)
                  << "  ★子代理 " << s.subTok
                  << "  墙钟 " << s.wallMs << "ms"
                  << "  坏行 " << bl << "\n";
        req += s.requests; tools += s.toolCalls; steps += s.steps;
        tok += s.tokIn + s.tokOut; sub += s.subTok; bad += bl;
    }
    // ★ 合计行里子代理**必须单列**：合进主 token 就又变成那个
    //   "不报错、只是账单少一截"的假数字（K-42）。
    std::cout << "[cost] 合计 文件=" << files << " 请求=" << req << " 工具=" << tools
              << " 步数=" << steps << " 主token=" << tok << " 子代理token=" << sub
              << " 坏行=" << bad << "\n";
    return 0;
}

static int runContext(const std::vector<std::string>& extra) {
    // ★ 离线估算也要**尽力贴近真实运行**：同一份系统提示、同一份工具表、
    //   同一份 CLAUDE.md、同一份仓库地图。任何一处偷懒，报出来的数就是假的。
    ToolRegistry registry;
    registerBuiltinTools(registry, nullptr, 120'000);

    af::MemoryConfig mc;
    mc.projectDir = fs::current_path();
    const std::string know = af::loadProjectKnowledge(mc);

    std::string map;
    if (envUtf8("AF_MAP", "1") != "0") {
        RepoIndexConfig rc = repoIndexConfigFromEnv();
        RepoIndex idx(rc);
        idx.build(extra.empty() ? fs::current_path() : pathFromUtf8(extra[0]));
        map = idx.renderMap({});
    }

    Turn t;
    t.input = "(离线估算：无历史)";
    const af::LayerChars c = layerCharsOf(buildSystemPrompt(), know, map, {}, t,
                                          registry.defs());
    printContext(c, "[上下文] 离线估算（空历史）");
    return 0;
}

static int runRegress(const std::vector<std::string>& extra) {
    if (extra.empty()) {
        std::cerr << "用法: agent.exe --regress <regress.jsonl>\n";
        return 1;
    }
    std::ifstream in(pathFromUtf8(extra[0]));
    if (!in) { std::cerr << "读不到 " << extra[0] << "\n"; return 1; }

    struct Case { std::string name; bool hasPass=false, hasCost=false, hasSteps=false;
                  int pass=0; long long cost=0; int steps=0; };
    std::vector<Case> cs;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            const json j = json::parse(line);
            Case c;
            c.name = j.value("name", std::string("?"));
            if (j.contains("pass")  && !j["pass"].is_null())  { c.hasPass = true;  c.pass  = j["pass"].get<int>() ? 1 : 0; }
            if (j.contains("cost")  && !j["cost"].is_null())  { c.hasCost = true;  c.cost  = j["cost"].get<long long>(); }
            if (j.contains("steps") && !j["steps"].is_null()) { c.hasSteps = true; c.steps = j["steps"].get<int>(); }
            cs.push_back(c);
        } catch (...) { /* 坏行跳过，但下面会报总数 */ }
    }
    if (cs.empty()) { std::cerr << "没有用例\n"; return 1; }

    // ★★ 缺一个数就**拒绝出结果**，而不是填 0 凑一个通过率。
    //    填 0 的后果是："成本 0"看起来像"很便宜"，于是报告一片大好。
    //    **宁可拒绝，不要骗人。**
    std::vector<std::string> bad;
    for (const auto& c : cs) {
        if (!c.hasPass || !c.hasCost || !c.hasSteps) {
            std::string miss;
            if (!c.hasPass)  miss += "pass ";
            if (!c.hasCost)  miss += "cost ";
            if (!c.hasSteps) miss += "steps ";
            bad.push_back(c.name + "(缺 " + miss + ")");
        }
    }
    if (!bad.empty()) {
        std::cout << "[回归] ★ 拒绝出结果：" << bad.size() << " 个用例缺字段\n";
        for (const auto& b : bad) std::cout << "    " << b << "\n";
        std::cout << "[回归] 通过率/成本/步数必须**三个都有** —— "
                     "填 0 凑出来的通过率比没有通过率更危险。\n";
        return 2;
    }

    int passed = 0; long long cost = 0, steps = 0;
    for (const auto& c : cs) {
        passed += c.pass; cost += c.cost; steps += c.steps;
        std::cout << "  " << std::left << std::setw(28) << c.name
                  << (c.pass ? "通过" : "失败")
                  << "  cost=" << c.cost << "  steps=" << c.steps << "\n";
    }
    const double rate = double(passed) / double(cs.size()) * 100.0;
    std::cout << "[回归] 用例=" << cs.size()
              << "  通过率=" << int(rate * 10) / 10.0 << "%"
              << " (" << passed << "/" << cs.size() << ")"
              << "  平均成本=" << (cost / long long(cs.size())) << " token"
              << "  平均步数=" << (steps / long long(cs.size())) << "\n";
    std::cout << "[回归] ★ 三个数必须一起看：只看通过率会奖励暴力重试，"
                 "只看成本会奖励什么都不做。\n";
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
                     "      agent.exe --ui-demo       [plain|ftxui|auto]\n"
                     "      agent.exe --sessions\n"
                     "      agent.exe --session-debug <文件.jsonl>\n"
                     "      agent.exe --resume        [任务]（会话 id 用 AF_SESSION_ID 指定）\n"
                     "      agent.exe --web           [任务]（AF_UI=web；任务留空则等浏览器输入）\n"
                     "      agent.exe --cost          <trace.jsonl>...\n"
                     "      agent.exe --context       [根目录]\n"
                     "      agent.exe --regress       <regress.jsonl>\n";
        return 1;
    }

    if (args[1] == "--selftest") return runSelfTest();

    // ---- ★ L12：仪表盘（不需要模型，不需要网络）---------------------------
    if (args[1] == "--cost")
        return runCost(std::vector<std::string>(args.begin() + 2, args.end()));
    if (args[1] == "--context")
        return runContext(std::vector<std::string>(args.begin() + 2, args.end()));
    if (args[1] == "--regress")
        return runRegress(std::vector<std::string>(args.begin() + 2, args.end()));

    // ---- ★ L10：会话（不需要模型，不需要网络）-----------------------------
    if (args[1] == "--sessions") return runSessions();
    if (args[1] == "--session-debug")
        return runSessionDebug(std::vector<std::string>(args.begin() + 2, args.end()));

    // ---- ★ L9：UI 演示（不需要模型，不需要网络）---------------------------
    if (args[1] == "--ui-demo")
        return runUiDemo(std::vector<std::string>(args.begin() + 2, args.end()));

    // ---- ★ L10：--resume —— 恢复上次会话，接着干 --------------------------
    //  ★ 不给任务就只打印恢复结果（这让"恢复到底恢复了什么"变成可断言的）。
    const bool wantResume = (args[1] == "--resume");
    if (wantResume && args.size() < 3) {
        const std::string dirArg = envUtf8("AF_SESSION_DIR", "");
        if (dirArg.empty()) { std::cerr << "请设置 AF_SESSION_DIR\n"; return 1; }
        const std::string id = envUtf8("AF_SESSION_ID", "");
        auto st = af::SessionStore::open(pathFromUtf8(dirArg), id);
        const af::LoadResult r = st->load();
        std::cout << "[恢复] id=" << st->meta().id
                  << "  resumed=" << (st->meta().resumed ? "1" : "0")
                  << "  消息 " << r.messages.size() << " 条"
                  << "  坏行 " << r.badLines << " 条"
                  << "  用到摘要=" << (r.hadSummary ? "1" : "0") << "\n";
        // ★ 静默降级必须说出来：给了 id 却没接着上，用户有权知道（见 open() 注释）
        if (!id.empty() && !st->meta().resumed)
            std::cout << "[恢复] ★ 没找到会话 " << id
                      << " —— 这是一个**新**会话（不是接着上次干）\n";
        for (std::size_t i = 0; i < r.messages.size(); ++i) {
            std::string c = r.messages[i].content;
            if (c.size() > 80) c = c.substr(0, 80) + "...";
            std::replace(c.begin(), c.end(), '\n', ' ');
            std::cout << "  [" << i << "] " << c << "\n";
        }
        return 0;
    }

    // ---- ★ L8：仓库地图 / 符号 A-B（不需要模型，不需要密钥）----------------
    if (args[1] == "--show-repo-map")
        return runShowRepoMap(std::vector<std::string>(args.begin() + 2, args.end()));
    if (args[1] == "--symbol-ab")
        return runSymbolAb(std::vector<std::string>(args.begin() + 2, args.end()));

    // ---- ★ 题目八：--web —— 任务也可以从浏览器里输入 ----------------------
    //  ★ 为什么需要这个入口：L12 的 CLI 是"一个 argv 跑一个任务"，
    //    而题目八的验收是"浏览器打开 → 输入任务 → 看到流式输出"。
    //    任务只能来自浏览器，所以必须有一个"先起 UI、再收任务"的入口。
    //  ★ --web 只是 AF_UI=web 的语法糖：后端选择**仍然只有一处**（makeRenderer），
    //    否则"环境变量"和"命令行开关"会变成两套真相。
    const bool webArg = (args[1] == "--web");
    if (webArg) {
#ifdef _WIN32
        _putenv_s("AF_UI", "web");
        // ★ --web：用户是"先起程序、再开浏览器"，所以启动阶段不按"没连上就降级"处理。
        //   纯 AF_UI=web（脚本/验收）才走"等 3 秒 → 退回 plain"那条路。
        _putenv_s("AF_WEB_REQUIRE_CLIENT", "1");
#else
        setenv("AF_UI", "web", 1);
        setenv("AF_WEB_REQUIRE_CLIENT", "1", 1);
#endif
    }

    std::string task;
    if (wantResume && args.size() >= 3)  task = sanitizeUtf8(args[2]);
    else if (webArg && args.size() >= 3) task = sanitizeUtf8(args[2]);
    else if (webArg)                     task = "";        // 等浏览器提交
    else                                 task = sanitizeUtf8(args[1]);

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
    //  ★ provider 必须**在工具表之前**建好：Task 工具的执行体要拿它发请求。
    //    这是"依赖顺序"，不是排版偏好。
    auto hotProvider = std::make_unique<HotProvider>(cfg);
    HotProvider* hot = hotProvider.get();      // 设置面板改配置时用（裸指针活得比闭包长）
    std::unique_ptr<IModelProvider> provider = std::move(hotProvider);

    // ---- ★ L12：旁路埋点（装饰 provider，不动契约层）-----------------------
    //
    //  ★ 为什么默认**关**：埋点会改变被测对象（观测者效应）。
    //    开着的代价在这里很小，但默认关能保证"没开 trace 的那一跑"是干净的，
    //    也让 C46 能真的量出"开与不开差多少"，而不是默认就是开、无从对比。
    //
    //  ★ 装饰器包在 provider 外面，所以**子代理的请求也被记到**（它共用 provider），
    //    而标记"这次请求来自子代理"靠的是 setInSubagent ——
    //    由 Task 工具的闭包在进入/离开时设置（只有它知道）。
    const bool traceOn = (envUtf8("AF_TRACE", "0") == "1");
    af::Tracer tracer(traceOn);
    provider = std::make_unique<af::TracingProvider>(std::move(provider), tracer);

    ToolRegistry registry;
    // ★ 命令默认只给 30 秒：递归/死循环程序（例如模型生成的 recursion.exe）
    //   以前会卡满 120 秒，页面看起来像“思考很久”。超时会连同子进程一起杀掉。
    const int cmdTimeoutMs = envInt("AF_CMD_TIMEOUT_MS", 30'000);
    registerBuiltinTools(registry, sandbox, cmdTimeoutMs);
    registry.setSandbox(sandbox);      // 接口已定；本讲由工具自己调用（见讲义 §3）

    // ---- ★ L11：Task 工具 —— 主模型眼里的"派个分身去查" -------------------
    //
    //  ★★ 注册位置不能后移（K-43）。它必须**早于**下面两件事：
    //      ① 权限门的规则表（按 registry.defs() 逐条生成默认规则）
    //      ② assembler.setTools(registry.defs())（请求里的 tools 数组）
    //    晚注册一步的后果是**两个**故障同时出现，而且互不相干：
    //      · 请求里没有 Task → 模型永远不知道有这个能力；
    //      · 规则表里没有 Task → 走到默认分支 → 被判成"询问" → 非交互环境直接拒绝。
    //    而这两个故障的报错都指向"模型没调用 / 用户拒绝了"，
    //    **没有一处指向"注册晚了"**。
    //
    //    ★ 与 L4「顺序即缓存」是同一条规矩的另一面：
    //      那一条说"工具列表顺序要稳定"，这一条说"**注册时机**也属于顺序"。
    //
    //  ★ 成本记账用一个**共享累加器**（边界④）：
    //    子代理的 token 花在它自己的 AgentLoop 上，主循环的 spentTokens()
    //    一个数都看不到 → 不单独记账的话 `/cost` 报出来的数字是假的（K-42）。
    auto subagentUsage = std::make_shared<Usage>();
    auto subagentRuns  = std::make_shared<int>(0);
    {
        ToolDef td = af::makeTaskToolDef();
        // ★ 捕获**裸指针**而不是 shared_ptr 本体：
        //   provider 是 unique_ptr，而 std::function 要求可拷贝 ——
        //   按值捕获它会直接编译失败（C2280）。
        //   裸指针在这里是安全的：provider 在 main 栈上活到最后，
        //   而所有 lambda 的生命周期都不超过它。
        IModelProvider* pp = provider.get();
        registry.registerBuiltin(td, [pp, &registry, subagentUsage, subagentRuns, &tracer]
                                     (const json& args) -> ToolResult {
            const std::string goal = args.value("goal", std::string());
            if (goal.empty()) return ToolResult::error("", "goal 不能为空");

            af::SubagentSpec spec;
            spec.goal     = goal;
            spec.readOnly = args.value("read_only", true);
            spec.maxSteps = std::max(1, envInt("AF_SUBAGENT_STEPS", 30));

            // ★ 每次执行都新建 runner：它不持有状态（除 lastSteps_），
            //   新建比复用更容易推理 —— 尤其"一个 Task 里会不会跑出两个子代理"
            //   这种问题，有状态就会变成隐患。
            af::SubagentRunner runner(*pp, registry);
            // ★ 标记作用域：接下来这些请求来自子代理，成本要单列（K-42）。
            //   退出时必须还原 —— 忘了还原会让**主循环**的钱被记成子代理的，
            //   症状是 /cost 里主 token 很小、子代理很大，而且不报错。
            tracer.setInSubagent(true);
            af::SubagentResult r;
            try {
                r = runner.run(spec);
            } catch (...) {
                tracer.setInSubagent(false);
                throw;
            }
            tracer.setInSubagent(false);

            // ★ 记账：token 不进主上下文，但钱要记到主会话
            subagentUsage->inputTokens  += r.usage.inputTokens;
            subagentUsage->outputTokens += r.usage.outputTokens;
            ++(*subagentRuns);

            std::cout << "[子代理] 目标=" << goal.substr(0, 40)
                      << "  步数=" << runner.lastSteps()
                      << "  读了 " << r.filesRead.size() << " 个文件"
                      << "  摘要 " << r.summary.size() << " 字符"
                      << "  token " << (r.usage.inputTokens + r.usage.outputTokens)
                      << (r.failed ? "  ★未完成" : "")
                      << (r.truncated ? "  ★摘要被截断" : "")
                      << "\n" << std::flush;

            // ★ 失败也是 observation：把失败原因回喂给主模型，让它自己决定怎么办
            return ToolResult::ok("", r.summary);
        });
    }

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
    // ★ L12：抽成函数，让 /context 的离线估算与真实运行用**同一份**。
    //   两处各写一份字面量，迟早漂移 —— 而漂移不会报错，只会让
    //   "我看见的分层图"和"线上真的字节"慢慢分家。
    const std::string systemPrompt = buildSystemPrompt();
    assembler.setSystem(systemPrompt);

    // ---- ★ L10：三层记忆之「项目知识」进 L1 层 ----------------------------
    //  ★ 落点是 L2 就预留的 `setProjectKnowledge()`（头文件注释写着"L10 加载"），
    //    与 L8 的 `setRepoMap()` 是同一类：**好设计会把将来要接的东西留成一个空函数，
    //    而不是留成一段要人去猜的注释。**
    //  ★ 记住：这里是"项目知识"，不是"上一个会话的对话"。
    //    **新会话不带旧对话** —— 跨会话传承只走 CLAUDE.md / MEMORY.md（讲义 §1.2）。
    std::string projectKnowledge;          // ★ L12：留给 /context 用
    {
        af::MemoryConfig mc;
        mc.projectDir = fs::current_path();
        projectKnowledge = af::loadProjectKnowledge(mc);
        if (!projectKnowledge.empty()) {
            assembler.setProjectKnowledge(projectKnowledge);
            std::cout << "[记忆] 项目知识 " << projectKnowledge.size() << " 字符进 L1 层\n"
                      << std::flush;
        }
    }

    // ---- ★ L8：仓库地图进 L2_RepoMap 层 ------------------------------------
    //  L0–L4 五层压缩做的都是"**压**"（东西已经进来了，想办法变小）；
    //  地图做的是"**选**"（在它进上下文之前就别让它进来）。
    //  没进来的东西既不用花 token，也不需要压缩 —— "选"永远比"压"便宜。
    std::shared_ptr<RepoIndex> repoIndex;
    std::vector<fs::path> activeFiles;
    int mapTokens = 0;
    std::string repoMapText;               // ★ L12：留给 /context 用
    if (envUtf8("AF_MAP", "1") != "0") {
        RepoIndexConfig rc = repoIndexConfigFromEnv();
        // 两个真相来源必须仲裁：配置里写的 1024，与分层预算算出来的值，取小的。
        const int layerTok = bcfg.forLayer(Layer::L2_RepoMap);
        if (layerTok > 0) rc.tokenBudget = std::min(rc.tokenBudget, layerTok);
        repoIndex = std::make_shared<RepoIndex>(rc);
        repoIndex->build(fs::current_path());
        repoMapText = repoIndex->renderMap({});
        mapTokens = ContextAssembler::estimateTokens(repoMapText);
        assembler.setRepoMap(repoMapText);
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

    // ---- ★ L7：压缩器 ------------------------------------------------------
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

    // ---- ★ 题目八：--web 的等待任务挪到后面（多轮循环里）--------------------
    //  ★ 早期版本在这里"等一条任务就往下走"，因此一个进程只能跑一轮。
    //    改成多轮后，等待点在轮次之间，见文件末尾的 web 循环。
    if (webArg) {
        std::cout << "[web] 打开 http://127.0.0.1:" << af::ui::webPortFromEnv()
                  << " 在输入框里写任务（Ctrl-C 退出）\n" << std::flush;
    }

    // ---- ★ L10：会话持久化（模块 ⑫）---------------------------------------
    //  ★ 默认**关闭**（AF_SESSION_DIR 为空 = 不落盘）。理由（讲义 §1.5）：
    //    ① 既有 162 条用例是在"无副作用"前提下写的，一开落盘就会互相污染；
    //    ② "默认不做某事"比"默认做、需要时关掉"安全 —— 后者会让"关掉"被忘记。
    std::unique_ptr<af::SessionStore> session;
    int  sessionBase = 0;                 // ★ 恢复出来的历史**不能**再 append 一遍
    int  sessionSeqBefore = 0;            // 本轮落盘前日志里已有的最大 seq
    const std::string sessDir = envUtf8("AF_SESSION_DIR", "");
    if (!sessDir.empty()) {
        // ★ 会话名与"要不要恢复"是**两件事**，必须分开：
        //     AF_SESSION_ID = 会话叫啥（不指定就自动生成）
        //     --resume      = 要不要把历史灌回上下文（继续上次那个任务）
        //   我最初写成 `wantResume ? AF_SESSION_ID : ""`，于是普通跑永远拿到随机 id，
        //   用户就算指定了名字也**没法在下一次 --resume 时指到它**（只能去抄打印出来的
        //   时间戳）。C33 就是这么失败的：落到了 s-1791195575545.jsonl，
        //   而 --resume 去找 c33.jsonl。
        const std::string sessId = envUtf8("AF_SESSION_ID", "");
        session = af::SessionStore::open(pathFromUtf8(sessDir), sessId);
        std::cout << "[会话] id=" << session->meta().id
                  << "  resumed=" << (session->meta().resumed ? "1" : "0")
                  << "  文件=" << pathUtf8(session->meta().jsonlPath) << "\n" << std::flush;
    }

    // ---- 观察者接线 ----
    //  ★★ 规矩：回调里**只**做"计数 + 入队"两件事，然后立刻返回。
    //     回调跑在读 socket 的那条线程上，它慢一拍，下一帧数据就晚一拍。
    bool sawFirstToken = false;
    int toolCalls = 0;
    int chunks = 0;
    int notices = 0;
    int permId = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> toolStartedAt;
    // ★ L12：工具 span 的 id 按 callId 存 —— 与 H7 的耗时配对是同一条规矩
    //   （INV-25：判据永远是 id 集合，不是"上一个/当前"）。
    std::map<std::string, int> toolSpan;

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

        // ★ L10：检查点 —— **先快照，再改**（工具还没执行，文件还是旧内容）。
        //   顺序反了（先改再快照）等于没快照：restore 回来还是改后的内容。
        if (session && (tc.name == "Write" || tc.name == "Edit" || tc.name == "MultiEdit")) {
            const std::string p = argPathOf(tc);
            if (!p.empty()) {
                std::error_code ecCp;
                fs::path abs = fs::absolute(pathFromUtf8(p), ecCp).lexically_normal();
                if (!ecCp) {
                    const af::CheckpointEvent cp = session->snapshot({abs}, tc.name);
                    if (!cp.snapshots.empty()) {
                        session->appendCheckpoint(cp);
                        std::cout << "[检查点] " << cp.id << "  " << tc.name
                                  << "  " << cp.snapshots.size() << " 个文件\n" << std::flush;
                    }
                }
            }
        }

        af::ui::UiEvent e;
        e.kind = af::ui::UiEvent::Kind::ToolStart;
        e.toolName = tc.name;
        e.toolId = tc.id;
        uiLoop.push(e);

        // ★ L12：旁路埋点。**只记，不决策** —— 返回值没人读、没人据此改行为。
        //   这是"旁路"的定义，也是 C51 要验的东西。
        toolSpan[tc.id] = tracer.beginSpan(af::TraceKind::ToolExec, tc.name);

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
                    repoMapText = m2;      // ★ L12：地图变了，/context 也要跟着变
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

        // ★ L12：收 span + 记结果。span 有**时长**（回答"哪一步慢"），
        //   event 只有时刻 —— 两类都要有，缺 span 的 trace 到最后一定答不出"慢在哪"。
        auto sit = toolSpan.find(r.callId);
        if (sit != toolSpan.end()) {
            tracer.endSpan(sit->second);
            toolSpan.erase(sit);
        }
        tracer.event(af::TraceKind::ToolDone, r.callId, r.isError ? "error" : "ok");
    };
    hooks.onNotice = [&](const std::string& msg) {
        ++notices;
        // ★ 步数：回归集三个数之一。它只能由**循环**数出来 ——
        //   provider 侧看不到"这是第几步"。
        tracer.event(af::TraceKind::Notice, msg);
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
        tracer.event(af::TraceKind::Error, e);
        std::cerr << "\n[请求失败] " << e << "\n" << std::flush;
    };
    // ★ 权限询问：阻塞的是 Agent 线程，渲染线程照常在转（见讲义 §1.5）。
    hooks.confirm = [&](const std::string& prompt) -> bool {
        return uiLoop.renderer().askPermission(prompt, ++permId);
    };

    // ★★ 题目八最关键的一根线：权限门真正问的是 IUIPort，不是 LoopHooks。
    //    工具 dispatch 走的是 ContextAssembler 里那个 shared_ptr<IUIPort>，
    //    而 hooks.confirm 在工具路径上根本没人调。
    //    不接这一根线，浏览器里的弹窗永远不出现 —— 日志上还看不出异常
    //    （工具会被当成"非交互环境"直接拒掉），这是这次最容易看漏的一处。
    //    ★ 只在 web 后端接管：plain/ftxui 保持原来的 ConsoleUI，不制造回归。
    if (af::ui::webIsBackend(uiLoop.renderer())) {
        ui->setConfirm([&](const std::string& prompt) -> bool {
            return uiLoop.renderer().askPermission(prompt, ++permId);
        });
    }

    // ---- 跑一个 turn（web 模式下会被反复调用）-----------------------------
    //  ★ 为什么抽成一个 lambda：多轮对话 = "同一个进程里跑很多次 run()"。
    //    把"一轮"变成一个可复用的动作，单轮路径和多轮路径就只差外面那层循环，
    //    不会出现"两条几乎一样的代码，改了一条忘了另一条"。
    Turn turn;
    LoopOutcome outcome = LoopOutcome::Completed;   // 最后一轮的结果（总结用）
    std::map<std::string, std::vector<Message>> chatHist;   // ★ 每个对话独立的上下文

    auto runTurn = [&](const std::string& taskText, const std::string& cid) {
        // ★ 设置面板：浏览器里改过的 API Key / 端点 / 模型，**从下一轮开始生效**。
        //   为什么不是"立刻"：本轮可能正跑着（半路换 key = 请求体已经发出去了）。
        if (af::ui::webIsBackend(uiLoop.renderer())) {
            af::ui::WebSettings ws;
            if (af::ui::webTakeSettings(uiLoop.renderer(), ws)) {
                ProviderConfig nc = cfg;
                if (!ws.apiKey.empty())  nc.apiKey  = ws.apiKey;
                if (!ws.baseUrl.empty()) nc.baseUrl = ws.baseUrl;
                if (!ws.model.empty())   nc.model   = ws.model;
                if (hot->reconfigure(nc)) {
                    cfg = nc;
                    std::cout << "[设置] 已应用：模型=" << nc.model << "\n" << std::flush;
                } else {
                    std::cout << "[设置] 配置不可用，沿用旧配置\n" << std::flush;
                }
            }
        }

        turn = Turn{};
        turn.input = taskText;

        // ★ 多对话：把这条对话自己的历史装回去（第一次出现就是空上下文）
        //
        //  ★★ 这里有个很容易漏的点：**用户那句话不在 turn.history 里**。
        //     循环把 turn.input 当成"最后一条 user 消息"临场拼进请求，
        //     所以跑完之后 turn.history 只有助手回复 + 工具消息。
        //     如果直接把 turn.history 存回去，下一轮模型看到的是
        //     "一串没有前因的助手发言"，答案会变成"你没让我记过任何东西"——
        //     而日志里 history=N 条看着一切正常（这个坑就是这么静默的）。
        //     所以：**先留一份 prior，跑完把用户消息补回中间**。
        std::vector<Message> prior;
        if (!cid.empty()) {
            const auto it = chatHist.find(cid);
            if (it != chatHist.end()) prior = it->second;
            turn.history = prior;
        }

        // ★ L10：恢复。★ 顺序：先 load，再记下 base ——
        //   base 之前的是"从日志里来的"，turn 结束后**不能**再写一遍（否则消息翻倍，
        //   见讲义 §3.4：这个 bug 不报错，只是上下文越跑越大）。
        if (session && wantResume && chatHist.empty()) {
            const af::LoadResult r = session->load();
            turn.history = r.messages;
            std::cout << "[恢复] 消息 " << r.messages.size() << " 条"
                      << "  坏行 " << r.badLines << " 条"
                      << "  用到摘要=" << (r.hadSummary ? "1" : "0") << "\n" << std::flush;
        }
        const std::size_t sessionBaseIdx = turn.history.size();
        if (session) sessionSeqBefore = session->seq();
        if (!cid.empty())
            std::cout << "[多轮] cid=" << cid << " 载入历史 " << sessionBaseIdx << " 条\n"
                      << std::flush;

        outcome = loop.run(turn, hooks);

        // ★ 把这一轮跑完的历史存回该对话 —— 下一轮（同 cid）就接得上
        if (!cid.empty()) {
            std::vector<Message> merged = prior;
            Message um;
            um.role    = Role::User;
            um.content = taskText;
            merged.push_back(std::move(um));
            for (std::size_t i = sessionBaseIdx; i < turn.history.size(); ++i)
                merged.push_back(turn.history[i]);
            chatHist[cid] = std::move(merged);
            std::cout << "[多轮] cid=" << cid << " 历史 " << turn.history.size()
                      << " → " << chatHist[cid].size() << " 条（对话数 "
                      << chatHist.size() << "）\n" << std::flush;
        }

        // ---- ★ L10：唯一的落盘点（turn 结束）-----------------------------
        //  ★★ 顺序不变式（T3 的答案，代码里最要紧的一条）：
        //     **先落盘新的（本轮消息），再落盘"老的可以丢了"（compaction）。**
        //     反过来一旦中间崩溃，就是"老的被标记删除、新的还没写" → 空上下文。
        if (session) {
            Message um; um.role = Role::User; um.content = taskText;
            session->append(um);
            for (std::size_t i = sessionBaseIdx; i < turn.history.size(); ++i)
                session->append(turn.history[i]);
            for (const auto& c : loop.compactionEvents())
                session->appendCompaction(c, sessionSeqBefore);
            std::cout << "[落盘] 新增 " << (turn.history.size() - sessionBaseIdx + 1)
                      << " 条  → " << pathUtf8(session->meta().jsonlPath) << "\n" << std::flush;
        }
    };

    // ---- ★ 手动压缩（浏览器里那个"压缩上下文"按钮）------------------------
    //  ★ 为什么手动压缩要另配一份阈值，而不是直接调 compact()：
    //    自动压缩的判据是"历史超过 L5 预算"（默认窗口下约 47k tok）。
    //    真实聊天常常一两万 token 就聊完了，按那个阈值这个按钮**永远不会生效** ——
    //    用户点了没反应，比没有按钮更糟。手动点 = "现在就压"，所以把预算缩到
    //    当前历史的一半、只逐字保留最近 1/4，其余仍走同一个五层级联
    //    （L2 折叠 → L3 摘要），不另写一套压缩逻辑。
    auto doCompact = [&](const std::string& cid) {
        auto it = chatHist.find(cid);
        if (it == chatHist.end() || it->second.size() < 3) {
            af::ui::webNotice(uiLoop.renderer(),
                              "这个对话还没有可压缩的历史（先跑完一轮再试）");
            return;
        }
        std::vector<Message>& hist = it->second;
        const int before = Compactor::estimateHistoryTokens(hist);

        // ★ 太短的上下文不压：压缩本身要花一次摘要调用（L3 的输入就是你想压掉
        //   的那段），两三百 token 去压，省下的还没有花掉的多。这条不是偷懒，
        //   是 compaction.hpp 开头那句"先用免费的手段，不够再花钱"。
        if (before < 800) {
            af::ui::webNotice(uiLoop.renderer(),
                              "上下文才约 " + std::to_string(before) +
                                  " tok，压了反而更贵（摘要也要 token）—— 多聊几轮再来");
            return;
        }

        af::ui::webNotice(uiLoop.renderer(),
                          "正在压缩上下文…（当前约 " + std::to_string(before) + " tok）");
        std::cout << "[压缩] cid=" << cid << " 开始，约 " << before << " tok\n" << std::flush;

        CompactionConfig mcfg = ccfg;
        const double histFrac =
            1.0 - (mcfg.budget.sysFrac + mcfg.budget.projectFrac +
                   mcfg.budget.repoMapFrac + mcfg.budget.retrievedFrac +
                   mcfg.budget.activeFrac);
        const int target = std::max(400, before / 2);
        const int avail  = int(double(target) / std::max(0.05, histFrac));
        mcfg.budget.contextWindow =
            avail + mcfg.budget.reserveForReply + mcfg.budget.maxOutputTokens;
        mcfg.keepRecentTokens = std::max(200, before / 3);

        Compactor mc(mcfg, summarizer);
        const CompactionOutcome out = mc.compact(hist, before);
        const int after = Compactor::estimateHistoryTokens(hist);

        std::string levels;
        for (const auto& e : out.events) {
            if (!levels.empty()) levels += "+";
            const char* nm = (e.level == 1) ? "单条降级"
                           : (e.level == 2) ? "折叠工具结果"
                           : (e.level == 3) ? "交接摘要"
                           : (e.level == 4) ? "裁剪到预算"
                           : "压缩";
            levels += nm;
        }
        std::string msg;
        if (!out.triggered || out.events.empty()) {
            msg = "没有可压缩的内容（历史里还没有能折叠的工具结果）";
        } else {
            msg = "上下文已压缩：约 " + std::to_string(before) + " → " +
                  std::to_string(after) + " tok（" + levels + "），保留 " +
                  std::to_string(out.keptMessages) + " 条、压掉 " +
                  std::to_string(out.droppedMessages) +
                  " 条。下一轮起生效；页面上的记录不变。";
        }
        af::ui::webNotice(uiLoop.renderer(), msg);
        std::cout << "[压缩] " << msg << "\n" << std::flush;
    };

    if (webArg) {
        // ---- ★ 题目八：多轮循环 ------------------------------------------
        //  ★ 旧版是"等一条任务 → 跑一轮 → 停在最后一屏"。页面一刷新就什么都接不上，
        //    "新建对话""继续追问"全都无从谈起。这里改成：跑完一轮回到"等下一条"，
        //    进程直到 Ctrl-C 才结束（页面也就能一直保持连接）。
        std::string cid = "default";
        std::string pendingTask = task;          // 命令行上带的第一个任务（可选）
        for (;;) {
            if (g_stop.get_token().stop_requested()) break;
            std::string text = pendingTask;
            pendingTask.clear();
            if (text.empty()) {
                std::string c;
                bool got = false;
                while (!g_stop.get_token().stop_requested()) {
                    // ★ 压缩请求优先于"等任务"：它不产生新轮次，只改上下文。
                    std::string ccid;
                    if (af::ui::webTakeCompact(uiLoop.renderer(), ccid)) {
                        doCompact(ccid.empty() ? cid : ccid);
                        continue;
                    }
                    if (af::ui::webWaitForTask(uiLoop.renderer(), text, c, 300)) {
                        got = true;
                        break;
                    }
                }
                if (!got) break;
                if (!c.empty()) cid = c;
            }
            std::cout << "[web] 收到任务[" << cid << "]: " << text << "\n" << std::flush;
            runTurn(text, cid);

            af::ui::UiEvent endEv;
            endEv.kind = af::ui::UiEvent::Kind::Notice;
            endEv.text = "本轮结束 —— 可以继续输入";
            uiLoop.push(endEv);
            std::cout << "[web] 本轮结束，等待下一条任务（Ctrl-C 退出）\n" << std::flush;
        }
    } else {
        runTurn(task, "");
    }
    if (canceller.joinable()) canceller.join();

    // ★ 收 UI：关队列 + join 渲染线程，然后才打总结 ——
    //   否则最后几行 UI 输出会和总结行**交错**，日志读起来像随机重排。
    uiLoop.stop();
    std::cout << "[UI] 后端=" << uiLoop.renderer().name()
              << " events=" << uiLoop.events()
              << " frames=" << uiLoop.frames() << "\n" << std::flush;

    // ---- ★ L12 /context：上下文被谁占了（八层，含工具表）-------------------
    //  ★ 默认关：它自己也要打印一堆字符，默认开着会污染既有 216 条用例的日志断言
    //    —— **度量本身也是一种输出**，这一点经常被忘。
    if (envUtf8("AF_CONTEXT", "0") == "1") {
        const af::LayerChars lc = layerCharsOf(systemPrompt, projectKnowledge,
                                               repoMapText, {}, turn, registry.defs());
        printContext(lc, "[上下文] 本轮实际分层");
        // ★ 与 trace 记下的"请求体字符数"对照 —— 两条独立路径互相验证。
        //   只对得上不算证明，对**不上**一定是哪里漏了。
        const auto ts = tracer.summary();
        if (ts.peakChars > 0)
            std::cout << "[上下文] 对照 trace 请求体峰值=" << ts.peakChars
                      << " chars（分层 " << lc.total()
                      << "，差值是 JSON 信封/键名/转义）\n";
    }

    // ---- ★ L12：trace 落盘（唯一的落盘点，与 L10 会话日志同构）-------------
    const std::string traceFile = envUtf8("AF_TRACE_FILE", "");
    if (traceOn && !traceFile.empty()) {
        const fs::path tp = pathFromUtf8(traceFile);
        if (tracer.flush(tp))
            std::cout << "[trace] 落盘 " << tracer.events().size() << " 条"
                      << "（丢弃 " << tracer.dropped() << " 条） → "
                      << pathUtf8(tp) << "\n" << std::flush;
        else
            std::cerr << "[trace] 落盘失败: " << pathUtf8(tp) << "\n";
    }
    if (traceOn) {
        const auto ts = tracer.summary();
        std::cout << "[trace] 请求=" << ts.requests << " 工具=" << ts.toolCalls
                  << " 步数=" << ts.steps
                  << " 主token=" << (ts.tokIn + ts.tokOut)
                  << " ★子代理=" << ts.subTok
                  << " 墙钟=" << ts.wallMs << "ms"
                  << " 丢弃=" << tracer.dropped() << "\n" << std::flush;
    }

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
              // ★ L11：子代理的成本**不进主上下文，但要单列**（K-42）
              //   不列出来的话，"这个任务花了多少"就是个假数字 ——
              //   而它偏偏不报错，只是让你的账单悄悄少一截。
              << " | subagent=" << *subagentRuns << " run/"
              << (subagentUsage->inputTokens + subagentUsage->outputTokens) << " tok"
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
