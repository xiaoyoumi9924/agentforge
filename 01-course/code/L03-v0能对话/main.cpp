// ---------------------------------------------------------------------------
//  L3 · main.cpp —— v0：能对话
//
//  这一讲的 main 只做一件事：**把一句话发给模型，把流式回答打出来。**
//  它故意没有 Agent 循环、没有工具、没有上下文组装 ——
//  那些分别是 L5 / L4 / L7 的事。
//
//  但它已经包含了两个"第一天就得处理"的 Windows 细节：
//    1. 控制台代码页（不设 → 中文全是方块）
//    2. 命令行参数编码（不处理 → 中文任务直接让 JSON 序列化抛异常）
//  这两条在后面每一讲都会用到，所以放在最早的一讲讲掉。
//
//  用法：
//      set OPENAI_API_KEY=sk-...
//      agent.exe "用一句话解释什么是 SSE"
//      set OPENAI_BASE_URL=http://127.0.0.1:8788/v1  （本地/假服务端，无需密钥）
// ---------------------------------------------------------------------------

// MSVC 会把 getenv 标记为不安全（C4996），CLI 工具不需要 _s 变体
#ifdef _MSC_VER
#  ifndef _CRT_SECURE_NO_WARNINGS
#    define _CRT_SECURE_NO_WARNINGS
#  endif
#endif

#ifdef _WIN32
// ★ NOMINMAX 必须在 windows.h 之前：否则 min/max 宏会污染整条包含链，
//   下游任何 std::min / std::max 都会报 C2589，而报错位置完全指不到这里。
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
#include <iostream>
#include <string>
#include <vector>

#include "model_provider.hpp"
#include "types.hpp"

using namespace af;

namespace {

std::string envOr(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    // ★ 环境变量同样是"外部字节"，也要过消毒
    return (v && *v) ? sanitizeUtf8(v) : def;
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
//  ★ 本讲第一个真坑：Windows 上 main() 拿到的 argv 是 ANSI 代码页编码
//    （中文机器 = GBK），不是 UTF-8。
//
//    症状：英文任务一切正常；一旦任务是中文，nlohmann 的 dump() 抛
//          type_error.316 invalid UTF-8 byte ...
//          而报错信息完全指不到"命令行编码"这个根因。
//
//    修法：绕开 argv，用 GetCommandLineW() 拿宽字符命令行，自己转 UTF-8。
// ---------------------------------------------------------------------------
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
//  事件渲染：把 ChatChunk 的 8 种事件翻译成终端输出
//
//  ★ 这里的 lambda 集**故意不写兜底分支 `[](const auto&)`**。
//    少了兜底，编译器就会替我们检查"是不是每一种事件都处理了"——
//    这正是 L2 选 std::variant 的全部理由。
//    什么时候需要兜底：只在"确实只关心少数几种事件"的场合（比如 L12 的埋点）。
// ---------------------------------------------------------------------------
struct Renderer {
    Ms ttft{0};
    bool sawFirstToken = false;
    Clock::time_point t0;
    Usage usage;
    FinishReason finish = FinishReason::Stop;
    bool sawError = false;
    // 收到过几次 usage 事件。
    // ★ 这个计数器不是装饰：正常服务端只在**最后一帧**报一次 usage。
    //   如果 providers.cpp 少了 `is_null` 判断，那些带 "usage": null 的帧
    //   会被当成真实的用量事件，这个数字会变成几十 ——
    //   而金额统计会因此彻底失真（L12 的 /cost 就靠它）。
    int usageEvents = 0;

    void operator()(const ChatChunk& c) {
        std::visit(overloaded{
            [&](const TextDelta& d) {
                // ★ 只有**非空**正文才算"第一个 token"。
                //   真实服务端的首帧往往是 `delta: {"role":"assistant","content":""}`
                //   —— 一个空串。如果拿它当首 token，TTFT 会被打成接近 0 的假数字，
                //   同时"到底有没有收到内容"的判断也会一起失效。
                if (!d.text.empty()) markFirstToken();
                std::cout << d.text << std::flush;
            },
            [&](const ThinkingDelta& d) {
                if (!d.text.empty()) markFirstToken();
                // 思考过程与正文要能区分开，否则用户以为模型在胡说
                std::cout << "\033[2m" << d.text << "\033[0m" << std::flush;
            },
            [&](const ToolCallStart& s) {
                // L3 还没注册任何工具，模型不可能发出 tool_call。
                // 一旦出现，说明服务端或者我们的协议理解有问题 —— 明说，别静默。
                std::cerr << "\n[未预期] 收到工具调用 " << s.name
                          << "（工具在 L4 才引入）\n";
            },
            [&](const ToolCallDelta&) {},
            [&](const ToolCallDone&)  {},
            [&](const UsageChunk& u) { usage = u.usage; ++usageEvents; },
            [&](const ErrorChunk& e) {
                sawError = true;
                std::cerr << "\n[流内错误] " << e.message
                          << (e.retryable ? "（可重试）" : "") << "\n";
            },
            [&](const DoneChunk& d) { finish = d.reason; },
        }, c);
    }

    void markFirstToken() {
        if (sawFirstToken) return;
        sawFirstToken = true;
        ttft = std::chrono::duration_cast<Ms>(Clock::now() - t0);
    }
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // ★ 本讲第二个真坑：Windows 控制台默认是 GBK 代码页。
    //   程序老老实实输出了 UTF-8 字节，但控制台按 GBK 解释 → 乱码。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // ---- 取任务描述 --------------------------------------------------------
#ifdef _WIN32
    std::vector<std::string> args = winUtf8Args();       // 见上方说明
    if (args.size() < 2) {
        std::cerr << "用法: agent.exe \"<任务描述>\"\n";
        return 1;
    }
    const std::string task = sanitizeUtf8(args[1]);
#else
    if (argc < 2) {
        std::cerr << "用法: agent \"<任务描述>\"\n";
        return 1;
    }
    const std::string task = sanitizeUtf8(argv[1]);
#endif

    // ---- 配置 --------------------------------------------------------------
    // v0 只支持 OpenAI 兼容协议（含 Ollama / vLLM / 各种网关）。
    ProviderConfig cfg;
    cfg.kind    = envOr("AF_PROVIDER", "openai");
    cfg.baseUrl = envOr("OPENAI_BASE_URL", "https://api.openai.com/v1");
    cfg.apiKey  = envOr("OPENAI_API_KEY", "");
    cfg.model   = envOr("AF_MODEL", "gpt-4o-mini");

    // 本地推理（Ollama / vLLM / 本讲的假服务端）不需要密钥 ——
    // 强行要求只会挡住正常用法
    const bool isLocal = cfg.baseUrl.find("127.0.0.1") != std::string::npos ||
                         cfg.baseUrl.find("localhost") != std::string::npos;
    if (cfg.apiKey.empty() && !isLocal) {
        std::cerr << "请设置 OPENAI_API_KEY"
                     "（本地 Ollama/vLLM 无需密钥，把 OPENAI_BASE_URL 指向 127.0.0.1 即可）\n";
        return 1;
    }
    std::cerr << "[配置] provider=" << cfg.kind << " model=" << cfg.model
              << " base=" << cfg.baseUrl << "\n";

    // ---- 组请求 ------------------------------------------------------------
    // v0 只有两条消息：system + user。
    // 分层组装（L0–L6）是 L7 的事，现在硬编码就够。
    ChatRequest req;
    req.system = "你是一个简洁的编码助手。回答用中文，不要寒暄。";
    Message user;
    user.role    = Role::User;
    user.content = task;
    req.messages.push_back(user);

    // ---- 发请求 + 流式渲染 --------------------------------------------------
    auto provider = createProvider(cfg);

    Renderer renderer;
    renderer.t0 = Clock::now();

    ChatResponse resp;
    bool ok = true;
    try {
        // stream() 的第二个参数就是"取消通道"：返回 false 会一路传回传输层并中止。
        // 本讲还没有中断键，所以恒返回 true；L6 会接上 Ctrl-C。
        resp = provider->stream(req, [&renderer](const ChatChunk& c) {
            renderer(c);
            return true;
        });
    } catch (const std::exception& e) {
        std::cerr << "\n[请求失败] " << e.what() << "\n";
        ok = false;
    }

    // ---- 收尾统计 ----------------------------------------------------------
    // 没有收到任何内容、也没有出错 —— 这是最容易被忽略的一种失败：
    // 服务端返回 200、finish_reason=stop，但一个字都没有。
    //
    // ★ 判据必须是"**有没有真的收到内容**"，不能用 usage 字段。
    //   原因：usage 是服务端自报的元数据，很多网关会漏报或报错（我们这次的
    //   假服务端就报了个 completion_tokens=24 的空回答）。
    //   拿元数据判断实际结果，等于让服务端替我们撒谎。
    if (ok && !renderer.sawFirstToken && renderer.finish != FinishReason::ToolUse) {
        std::cerr << "\n[警告] 服务端返回成功，但没有收到任何内容\n";
        ok = false;
    }

    std::cout << "\n\n--- TTFT " << renderer.ttft.count() << "ms"
              << " | in " << renderer.usage.inputTokens
              << " / out " << renderer.usage.outputTokens << " tokens"
              << " | usageEvents=" << renderer.usageEvents
              << " | finish=" << static_cast<int>(renderer.finish);

    // finish_reason 必须显式呈现：length 表示被截断，不是"完成"
    if (renderer.finish == FinishReason::Length)
        std::cout << "（被截断！）";
    std::cout << " ---\n";
    std::cout << "[响应] total=" << resp.usage.total()
              << " finish=" << static_cast<int>(resp.finishReason) << "\n";

    return ok ? 0 : 2;
}
