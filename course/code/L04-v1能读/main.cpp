// ---------------------------------------------------------------------------
//  L4 · main.cpp —— v1：能读
//
//  这一讲第一次出现"**模型决定下一步**"。
//
//  v0 是：  用户问 → 模型答 → 结束
//  v1 是：  用户问 → 模型说"我要读 a.cpp" → 我们读 → 把内容回喂 →
//           模型看到内容 → 终于能答了
//
//  ★ 这里出现了一个**临时的 for 循环**。它还不是 AgentLoop ——
//    没有六道闸门、没有死循环检测、没有取消传播、没有观察者。
//    L5 会把它正式抽成 agent_loop.{hpp,cpp}，L6 再加闸门。
//    本讲先让它能跑，是为了让你**先看见循环的必要性**，
//    而不是先学一堆抽象再回头理解为什么需要它。
//
//  用法：
//      agent.exe "mock_target.txt 里有几个函数？"
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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "model_provider.hpp"
#include "tool_registry.hpp"
#include "types.hpp"

using namespace af;

namespace {

// 一次 CLI 会话里最多来回几轮。L6 会换成正式的多道闸门。
constexpr int kMaxRounds = 6;

// ---------------------------------------------------------------------------
//  读环境变量
//
//  ★ 与 winUtf8Args 同一类问题：`getenv` 拿到的是 **ANSI 代码页（中文机 =
//    GBK）** 的字节。本课程自己的目录就叫 `11-逐讲/L4-v1能读`，
//    于是 AF_PLUGIN 传进来是一个中文路径 —— 用 getenv 读会拿到 GBK 字节，
//    再经 sanitizeUtf8 把每个汉字替换成 U+FFFD，路径彻底废掉。
//
//    所以 Windows 上必须用 `_wgetenv`（宽字符）读，再转 UTF-8。
// ---------------------------------------------------------------------------
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
#else
std::string envUtf8(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    return (v && *v) ? sanitizeUtf8(v) : def;
}
#endif

#ifdef _WIN32
// Windows 的 argv 是 ANSI 代码页（中文机 = GBK），不是 UTF-8。
// 直接塞进 JSON，nlohmann 的 dump() 会抛 type_error.316，而报错完全指不到根因。
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
#endif

// ---------------------------------------------------------------------------
//  端口 ④ 的一个朴素实现：终端询问
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
//  内置工具：L4 只有 Read
//
//  为什么先只加 Read：v1 的里程碑是"能读"。
//  Write / Edit / Bash 会在 L5 一起进来 —— 因为"能改"才有"改错了怎么办"，
//  那才需要检查点、权限门和沙箱。
// ---------------------------------------------------------------------------
std::string readFileWithLineNumbers(const std::string& path, int maxLines = 2000) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};                      // 空 → 由调用方转成错误
    std::ostringstream os;
    std::string line;
    int n = 0;
    while (std::getline(ifs, line) && n < maxLines) {
        // 去掉行尾 \r（CRLF 文件），避免行号后面拖一个不可见字符
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

    // ★ 豁免落盘。否则会形成"读文件 → 结果太大被落盘 → 再去读那个落盘文件"
    //   的循环依赖。这不是优化，是正确性要求。
    def.persistThresholdChars = SIZE_MAX;

    reg.registerBuiltin(def, [](const json& args) -> ToolResult {
        const std::string path = args.value("path", std::string());
        if (path.empty()) return ToolResult::error("", "path 不能为空");
        std::string content = readFileWithLineNumbers(path);
        if (content.empty()) {
            // 回到模型的是"文件不存在"，不是"空文件" —— 两者含义完全不同
            return ToolResult::error("", "无法读取文件（不存在或为空）: " + path);
        }
        return ToolResult::ok("", std::move(content));
    });
}

// ---------------------------------------------------------------------------
//  渲染：ChatChunk → 终端
//
//  仍然**不写兜底分支** —— 让编译器盯着 8 种事件有没有漏处理。
// ---------------------------------------------------------------------------
struct Renderer {
    Ms   ttft{0};
    bool sawFirstToken = false;
    Clock::time_point t0;
    int  usageEvents = 0;

    void beginChunk() {
        if (sawFirstToken) return;
        sawFirstToken = true;
        ttft = std::chrono::duration_cast<Ms>(Clock::now() - t0);
    }

    void operator()(const ChatChunk& c) {
        std::visit(overloaded{
            [&](const TextDelta& d) {
                if (!d.text.empty()) beginChunk();       // 空串不算首 token
                std::cout << d.text << std::flush;
            },
            [&](const ThinkingDelta& d) {
                if (!d.text.empty()) beginChunk();
                std::cout << "\033[2m" << d.text << "\033[0m" << std::flush;
            },
            [&](const ToolCallStart& s) {
                std::cout << "\n[模型请求工具] " << s.name << "\n" << std::flush;
            },
            // 参数是一段段来的，中途不展示 —— 展示半个 JSON 只会误导
            [&](const ToolCallDelta&) {},
            [&](const ToolCallDone& t) {
                std::cout << "[参数] " << t.args.dump() << "\n" << std::flush;
            },
            [&](const UsageChunk&) { ++usageEvents; },
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
    if (args.size() < 2) { std::cerr << "用法: agent.exe \"<任务描述>\"\n"; return 1; }
    const std::string task = sanitizeUtf8(args[1]);
    (void)argc; (void)argv;
#else
    if (argc < 2) { std::cerr << "用法: agent \"<任务描述>\"\n"; return 1; }
    const std::string task = sanitizeUtf8(argv[1]);
#endif

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

    // ---- 工具表 ----
    ToolRegistry registry;
    registerReadTool(registry);

    // 可选：加载 C ABI 插件（环境变量给路径）
    const std::string pluginPath = envUtf8("AF_PLUGIN", "");
    if (!pluginPath.empty()) {
        std::string err;
        if (registry.loadPlugin(pluginPath, err))
            std::cerr << "[插件] 已加载 " << pluginPath << "\n";
        else
            std::cerr << "[插件] 加载失败: " << err << "\n";
    }

    // 权限门：deny 永远赢（DefaultPermissionGate 内部强制排序）
    // L4 只有 Read，这里主要是把接口用起来；L6 会把它接到真正的危险操作上。
    {
        std::vector<PermissionRule> rules{
            {DecisionKind::Deny,  "Bash(rm -rf *)", "删除类命令一律拒绝"},
            {DecisionKind::Allow, "Read",           "只读操作直接放行"},
        };
        registry.setGate(std::make_shared<DefaultPermissionGate>(std::move(rules)));
    }

    auto ui = std::make_shared<ConsoleUI>();

    // ---- 循环（临时版）----
    // ★ 这就是"第一性循环"的第一次出现：
    //     while (模型返回工具调用) { 执行 → 捕获 → 追加到上下文 → 再调模型 }
    //   只是这里用 for + 上限代替了 while（上限在 L6 会变成六道闸门之一）。
    std::vector<Message> history;
    {
        Message u;
        u.role    = Role::User;
        u.content = task;
        history.push_back(std::move(u));
    }

    auto provider = createProvider(cfg);
    Usage totalUsage;
    LoopOutcome outcome = LoopOutcome::Completed;
    std::string finalText;
    size_t toolCallCount = 0;

    for (int round = 0; round < kMaxRounds; ++round) {
        ChatRequest req;
        req.system   = "你是一个编码 Agent。原则：\n"
                       "1. 需要文件内容时必须**调用工具去读**，不要凭猜测回答。\n"
                       "2. 工具报错是信息不是失败 —— 读它，然后换方法。\n"
                       "3. 回答要给出依据（哪个文件、哪几行）。\n";
        req.messages = history;
        req.tools    = registry.defs();      // ★ 不下发工具，模型永远不会调工具

        Renderer r;
        r.t0 = Clock::now();

        ChatResponse resp;
        try {
            resp = provider->stream(req, [&r](const ChatChunk& c) { r(c); return true; });
        } catch (const std::exception& e) {
            std::cerr << "\n[请求失败] " << e.what() << "\n";
            outcome = LoopOutcome::Error;
            break;
        }

        totalUsage.inputTokens  += resp.usage.inputTokens;
        totalUsage.outputTokens += resp.usage.outputTokens;
        // ★ 把完整的 resp.message 存回历史（不能只存 content —— 工具字段会丢）
        history.push_back(resp.message);

        if (resp.finishReason == FinishReason::Length) {
            std::cerr << "\n[警告] 回答被截断（finish_reason=length）\n";
            outcome = LoopOutcome::Error;
            break;
        }

        // ---- 模型没有要工具 → 任务结束 ----
        if (resp.message.toolCalls.empty()) {
            finalText = resp.message.content;
            break;
        }

        // ---- 模型要工具 → 执行并回喂 ----
        // 失败也是 observation：dispatch 永远返回 ToolResult，从不抛异常。
        for (const auto& tc : resp.message.toolCalls) {
            ++toolCallCount;
            ToolResult tr = registry.dispatch(tc, *ui);
            std::cout << (tr.isError ? "[工具失败] " : "[工具返回] ")
                      << tr.content.substr(0, 300)
                      << (tr.content.size() > 300 ? " ..." : "") << "\n\n" << std::flush;
            history.push_back(tr.toMessage());
        }
    }

    if (finalText.empty() && outcome == LoopOutcome::Completed) {
        // 转满上限仍未收敛：本讲还没有闸门，只能如实报告
        outcome = LoopOutcome::StepLimitReached;
    }

    std::cout << "\n===== " << toString(outcome)
              << " | rounds<= " << kMaxRounds
              << " | toolCalls=" << toolCallCount
              << " | in " << totalUsage.inputTokens
              << " / out " << totalUsage.outputTokens << " tokens"
              << " | history=" << history.size() << " msgs =====\n";

    return outcome == LoopOutcome::Completed ? 0 : 2;
}
