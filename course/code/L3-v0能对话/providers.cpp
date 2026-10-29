// ---------------------------------------------------------------------------
//  L3 · providers.cpp —— 模型适配层的第一个实现（模块 ②）
//
//  v0 的目标只有一个：**能把一句话发给模型，并把流式回答一个字一个字打出来。**
//  所以这个文件刻意只做三件事：
//    1. 把 ChatRequest 翻译成 OpenAI 兼容协议的 JSON
//    2. 把 SSE 字节流翻译成 ChatChunk 事件
//    3. 处理重试（只在流开始之前）与超时
//
//  ★ 本讲**不实现**的部分（都是刻意的，后面每讲补一块）：
//      L4  工具定义下发（tools 字段）与工具调用的增量累积
//      L7  Anthropic 协议（system 在顶层、tool_result 是 user 消息……）
//      L6  真正的取消传播（现在 onChunk 返回 false 已经能中止，但没接用户中断键）
//
//  ★ 这段代码里有两处"看起来多余、删了就出事"的判断，见下文标了 ★ 的地方。
//    它们是 09-核心代码 里用真实端到端测试换来的（那次一共修了 8 个 bug）。
// ---------------------------------------------------------------------------
#include "model_provider.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <thread>
#include <unordered_map>

#include "http_transport.hpp"
#include "sse_parser.hpp"

namespace af {

// ===========================================================================
//  流式上下文：把 SSE 事件流翻译成 ChatChunk 事件流
//
//  它是"协议转换"的状态机。跨 chunk 的累积状态（正文、usage）都挂在这里，
//  因为一次 SSE 响应可能被切成几百个 TCP 分片。
// ===========================================================================
struct StreamCtx {
    SSEParser parser;                                  // 跨块缓冲在这里面
    std::function<bool(const ChatChunk&)> onChunk;     // 返回 false = 取消

    std::string  textAcc;                              // 累积正文（最终答案）
    Usage        usage;
    FinishReason finishReason = FinishReason::Stop;

    // ★ L4 会在这里加工具参数增量缓冲：
    //     struct Buf { std::string id, name, raw; };
    //     std::unordered_map<int, Buf> toolBufs;
    //   本讲不加，是因为 v0 还没注册任何工具，加了就是死代码。

    bool emit(const ChatChunk& c) { return onChunk ? onChunk(c) : true; }

    // 传输层把原始字节喂进来。返回 false = 上层要求取消（一路传回传输层）。
    // ★ 绝不在没有遇到完整分隔符时尝试解析 —— 这是"工具参数 JSON 被静默损坏"的根源。
    bool feed(const char* data, std::size_t len) {
        for (auto& ev : parser.push(std::string_view(data, len))) {
            if (ev.isDone()) return false;     // data: [DONE]
            if (!dispatch(ev)) return false;
        }
        return true;
    }

    bool dispatch(const SSEEvent& ev) {
        if (ev.data.empty()) return true;
        // 容错解析：坏帧直接跳过。**不能 throw** —— 一个坏帧不该毁掉整个会话。
        json j = json::parse(ev.data, nullptr, false);
        if (j.is_discarded()) return true;
        return dispatchOpenAI(j);
    }

    // 返回 false = 调用方要求取消。
    bool dispatchOpenAI(const json& j) {
        bool keep = true;

        // ---- ① usage -------------------------------------------------------
        // ★ 必须同时判 contains 和 is_null。
        //   很多兼容网关（以及本课程的 mock）在**每一帧**都带 "usage": null。
        //   只判 contains 就会把 null 当成一次真实的 usage 事件报给上层，
        //   看起来只是"用量数字偶尔变 0"，实际会让 L12 的成本统计彻底失真。
        if (j.contains("usage") && !j["usage"].is_null()) {
            const auto& u = j["usage"];
            usage.inputTokens  = u.value("prompt_tokens", usage.inputTokens);
            usage.outputTokens = u.value("completion_tokens", usage.outputTokens);
            keep = emit(UsageChunk{usage}) && keep;
        }

        if (!j.contains("choices") || j["choices"].empty()) return keep;

        const auto& ch = j["choices"][0];
        const auto  d  = ch.value("delta", json::object());

        // ---- ② 正文 ---------------------------------------------------------
        if (d.contains("content") && !d["content"].is_null()) {
            std::string t = d["content"].get<std::string>();
            textAcc += t;                                  // 累积，最终答案要靠它
            keep = emit(TextDelta{std::move(t)}) && keep;
        } else if (d.contains("reasoning_content") && !d["reasoning_content"].is_null()) {
            // 推理型模型（DeepSeek-R1 等）的思考过程。**它不进正文**。
            keep = emit(ThinkingDelta{d["reasoning_content"].get<std::string>()}) && keep;
        }

        // ---- ③ 结束原因（必须放在最后）----------------------------------------
        // ★ 这段的纪律：**一个 chunk 里可能同时带 content 和 finish_reason。**
        //   真实网关经常把 finish_reason 和最后一段 content 塞在同一帧。
        //   如果上面处理完 content 就 return，finish_reason 会被吞掉 ——
        //   于是 finish_reason='length'（被截断）会被当成正常结束，
        //   用户拿到半截答案而系统认为"任务完成"。这个 bug 不会报任何错。
        if (ch.contains("finish_reason") && !ch["finish_reason"].is_null()) {
            finishReason = fromOpenAIFinish(ch["finish_reason"].get<std::string>());
            keep = emit(DoneChunk{finishReason}) && keep;
        }
        return keep;
    }

    static FinishReason fromOpenAIFinish(const std::string& s) {
        if (s == "stop")           return FinishReason::Stop;
        if (s == "tool_calls")     return FinishReason::ToolUse;
        if (s == "length")         return FinishReason::Length;
        if (s == "content_filter") return FinishReason::ContentFilter;
        return FinishReason::Other;
    }
};

// ===========================================================================
//  基类：共享「重试 + 超时 + 错误分类」
//
//  为什么是这个形状（而不是在每个 provider 里各写一份）：
//    重试逻辑对所有 provider 都是一样的，只是**协议转换**不同。
//    这里先用一个基类装着重试（模板方法），到 L4 真的有第二个 provider 时
//    再回头看这个决定对不对 —— 那正是引入模式的正确时机。
//
//  HTTP 传输走 IHttpTransport（端口②）：Windows = WinHTTP，其它 = libcurl。
//  协议转换是平台无关的核心逻辑，不碰任何具体的 HTTP 库。
// ===========================================================================
namespace {

class HttpProviderBase : public IModelProvider {
protected:
    explicit HttpProviderBase(ProviderConfig cfg)
        : cfg_(std::move(cfg)), transport_(createHttpTransport()) {}

    struct HttpResult {
        long        code = 0;
        std::string body;
        bool        transportError = false;
    };

    HttpResult execute(const std::string& url, const std::string& payload,
                       const std::vector<std::string>& extraHeaders,
                       StreamCtx* sctx) {
        HttpResult out;

        HttpRequestSpec spec;
        spec.url              = url;
        spec.body             = payload;
        spec.connectTimeoutMs = cfg_.retry.firstTokenTimeoutMs;
        spec.totalTimeoutMs   = cfg_.retry.totalTimeoutMs;
        spec.headers.push_back("Content-Type: application/json");
        for (const auto& h : extraHeaders) spec.headers.push_back(h);
        // SSE 的头号杀手是反向代理缓冲。这一行让它别攒着不发。
        spec.headers.push_back("X-Accel-Buffering: no");

        for (int attempt = 0; attempt < cfg_.retry.maxAttempts; ++attempt) {
            HttpResponseInfo info;

            if (sctx) {
                info = transport_->postStream(spec, [sctx](const char* d, std::size_t n) {
                    return sctx->feed(d, n);        // 返回 false 即刻中止传输
                });
                // ★ 已经吐过 token 就不再重试：重试 = 重新计费 + 用户看到重复内容。
                //   （本讲用 textAcc 非空来判断"开始吐了"）
                if (sctx && !sctx->textAcc.empty()) {
                    out.code = info.statusCode;
                    out.transportError = info.transportError;
                    if (info.transportError) out.body = info.errorMessage;
                    return out;
                }
            } else {
                out.body.clear();
                info = transport_->post(spec, out.body);
            }

            out.code           = info.statusCode;
            out.transportError = info.transportError;
            if (info.transportError) out.body = info.errorMessage;

            if (!info.transportError && out.code >= 200 && out.code < 300) return out;

            // 上下文超限不是重试能解决的（L7 会用它触发压缩）
            if (out.code == 400 &&
                out.body.find("context_length_exceeded") != std::string::npos)
                return out;

            const bool retryable =
                std::find(cfg_.retry.retryOn.begin(), cfg_.retry.retryOn.end(),
                          int(out.code)) != cfg_.retry.retryOn.end();
            if (!retryable || attempt + 1 >= cfg_.retry.maxAttempts) return out;
            backoffSleep(attempt);
        }
        return out;
    }

    // 指数退避 + 抖动。抖动不是可选优化：100 个客户端同时被限流后同时重试会打死服务端。
    void backoffSleep(int attempt) const {
        double d = std::min<double>(
            cfg_.retry.initialBackoffMs * std::pow(cfg_.retry.backoffMultiplier, attempt),
            double(cfg_.retry.maxBackoffMs));
        if (cfg_.retry.jitter) {
            static thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_real_distribution<double> u(0.5, 1.0);
            d *= u(rng);
        }
        std::this_thread::sleep_for(Ms(long(d)));
    }

    ProviderConfig cfg_;
    std::unique_ptr<IHttpTransport> transport_;
};

// ===========================================================================
//  OpenAI 兼容实现
//  —— 一份代码覆盖 OpenAI / DeepSeek / 通义 / vLLM / Ollama / 各种网关，
//     因为它们都（大致）遵循同一个 /chat/completions 协议。
// ===========================================================================
class OpenAIProvider final : public HttpProviderBase {
public:
    explicit OpenAIProvider(ProviderConfig cfg) : HttpProviderBase(std::move(cfg)) {}

    std::string_view id() const override { return cfg_.kind; }
    int  contextWindow() const override { return cfg_.contextWindow; }
    bool supportsTools() const override { return true; }
    bool supportsStreaming() const override { return true; }
    bool supportsPromptCaching() const override { return false; }  // OpenAI 是自动前缀缓存

    ChatResponse chat(const ChatRequest& req) override { return run(req, nullptr); }

    ChatResponse stream(const ChatRequest& req,
                        const std::function<bool(const ChatChunk&)>& cb) override {
        StreamCtx ctx;
        ctx.onChunk = cb;
        return run(req, &ctx);
    }

    int countTokens(std::string_view text) override { return int(text.size() / 4) + 1; }

private:
    ChatResponse run(const ChatRequest& req, StreamCtx* sctx) {
        json body = toRequest(req, sctx != nullptr);
        // 流式请求要显式要 usage —— 默认不带，成本就没法统计。
        if (sctx) body["stream_options"] = {{"include_usage", true}};

        std::vector<std::string> headers{"Authorization: Bearer " + cfg_.apiKey};
        auto r = execute(cfg_.baseUrl + "/chat/completions", body.dump(), headers, sctx);

        if (r.code != 200)
            throw std::runtime_error("openai http " + std::to_string(r.code) + ": " +
                                     r.body.substr(0, 300));

        if (sctx) {
            // 处理不以 "\n\n" 结尾的残留帧
            for (auto& ev : sctx->parser.flush()) sctx->dispatch(ev);

            ChatResponse resp;
            resp.usage        = sctx->usage;
            resp.message.role = Role::Assistant;
            resp.message.content = sctx->textAcc;
            // ★ 用服务端真正的 stop reason，**不要猜**。
            //   猜的后果：length（截断）被当成正常结束，用户拿到半截答案。
            resp.finishReason = sctx->finishReason;
            return resp;
        }
        return parseResponse(json::parse(r.body));
    }

    json toRequest(const ChatRequest& req, bool stream) const {
        json j;
        // ★ 不要用 provider.id() 覆盖 req.model —— id() 返回的是厂商名（"openai"），
        //   不是模型名。曾经因为这个 bug，所有请求都发出了 model="openai"。
        j["model"]       = req.model.empty() ? cfg_.model : req.model;
        j["stream"]      = stream;
        j["max_tokens"]  = req.maxTokens;
        if (req.temperature)         j["temperature"] = *req.temperature;
        if (!req.stopSequences.empty()) j["stop"] = req.stopSequences;

        json msgs = json::array();
        // OpenAI 把 system 当成 messages[0]，不是顶层字段（Anthropic 才是顶层）
        if (req.system) msgs.push_back({{"role", "system"}, {"content", *req.system}});
        for (const auto& m : req.messages) msgs.push_back(messageToJson(m));
        j["messages"] = msgs;

        // ★ L4 在这里补 tools / tool_choice。
        //   顺序必须**字节级稳定**，否则提示缓存永不命中（NFR-2）。
        return j;
    }

    static json messageToJson(const Message& m) {
        json j{{"role", roleName(m.role)}, {"content", m.content}};
        return j;
    }

    static const char* roleName(Role r) {
        switch (r) {
            case Role::System:    return "system";
            case Role::User:      return "user";
            case Role::Assistant: return "assistant";
            case Role::Tool:      return "tool";
        }
        return "user";
    }

    // 非流式响应解析（v0 的 chat() 用它；流式走 StreamCtx）
    static ChatResponse parseResponse(const json& j) {
        ChatResponse r;
        const auto& msg = j["choices"][0]["message"];
        r.message.role = Role::Assistant;
        if (msg.contains("content") && !msg["content"].is_null())
            r.message.content = msg["content"].get<std::string>();
        r.finishReason = StreamCtx::fromOpenAIFinish(
            j["choices"][0].value("finish_reason", "stop"));
        if (j.contains("usage") && !j["usage"].is_null()) {
            r.usage.inputTokens  = j["usage"].value("prompt_tokens", 0);
            r.usage.outputTokens = j["usage"].value("completion_tokens", 0);
        }
        return r;
    }
};

}  // namespace

// ===========================================================================
//  工厂（P-16）—— 调用方只要"一个可用的实现"，不关心是哪个、怎么构造
// ===========================================================================
std::unique_ptr<IModelProvider> createProvider(const ProviderConfig& cfg) {
    // L7 会在这里加：if (cfg.kind == "anthropic") ...
    return std::make_unique<OpenAIProvider>(cfg);
}

}  // namespace af
