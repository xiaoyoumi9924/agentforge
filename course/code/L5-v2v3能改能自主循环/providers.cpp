// ---------------------------------------------------------------------------
//  L4 · providers.cpp —— 模型适配层：加入 function calling
//
//  v0 → v1 的变化只有一条主线：**协议里多了一条轴**。
//
//      L3：messages 进  →  文本出
//      L4：messages + tools 进  →  文本 或 **工具调用意图** 出
//
//  这条新轴带来三个必须处理的东西：
//    ① 请求侧：tools 定义要下发，且**顺序必须字节级稳定**（否则提示缓存永不命中）
//    ② 响应侧：tool_calls 的 arguments 是**被切成 N 段的 JSON 字符串**，
//              只能在结束时 parse 一次 —— 中途 parse 必然失败
//    ③ 历史侧：上一轮的 assistant.tool_calls 和这一轮的 tool 结果
//              都必须能被序列化回去，否则模型不认账
//
//  ★ 本讲不实现 Anthropic（L7 补）。第二个协议才会暴露适配器的全部差异，
//    现在加只会让你"照着抄一遍"而看不到为什么。
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
//  流式上下文：SSE 事件流 → ChatChunk 事件流
// ===========================================================================
struct StreamCtx {
    SSEParser parser;
    std::function<bool(const ChatChunk&)> onChunk;

    // ---- ★ L4 新增：工具参数增量缓冲 ----
    // key 用 provider 给的 index（一个 assistant 消息里可以有 N 个并行 tool_call）。
    // raw 是**尚未解析的 JSON 片段**，可能在任何位置被切断。
    struct ToolBuf {
        std::string id;
        std::string name;
        std::string raw;         // 累积的 arguments 字符串，**中途绝不 parse**
    };
    std::unordered_map<int, ToolBuf> toolBufs;

    std::string  textAcc;
    std::vector<ToolCall> calls;      // 已完成的工具调用
    Usage        usage;
    FinishReason finishReason = FinishReason::Stop;

    bool emit(const ChatChunk& c) { return onChunk ? onChunk(c) : true; }

    bool feed(const char* data, std::size_t len) {
        for (auto& ev : parser.push(std::string_view(data, len))) {
            if (ev.isDone()) return false;
            if (!dispatch(ev)) return false;
        }
        return true;
    }

    bool dispatch(const SSEEvent& ev) {
        if (ev.data.empty()) return true;
        json j = json::parse(ev.data, nullptr, false);
        if (j.is_discarded()) return true;      // 坏帧跳过，不崩
        return dispatchOpenAI(j);
    }

    // 返回 false = 调用方要求取消。
    //
    // ★ 本函数的纪律（L3 定下，L4 更要守住）：
    //   1. 一个 chunk 里可能**同时**带 content、tool_calls 和 finish_reason，
    //      所以任何分支都不许提前 return。
    //   2. finish_reason 永远放在最后处理。
    //   3. 工具参数只在 finish_reason 出现的这一刻 parse 一次。
    bool dispatchOpenAI(const json& j) {
        bool keep = true;

        // ---- ① usage（每帧都可能是 null，必须判 is_null）----
        if (j.contains("usage") && !j["usage"].is_null()) {
            const auto& u = j["usage"];
            usage.inputTokens  = u.value("prompt_tokens", usage.inputTokens);
            usage.outputTokens = u.value("completion_tokens", usage.outputTokens);
            keep = emit(UsageChunk{usage}) && keep;
        }

        if (!j.contains("choices") || j["choices"].empty()) return keep;

        const auto& ch = j["choices"][0];
        const auto  d  = ch.value("delta", json::object());

        // ---- ② 正文 / 思考 ----
        if (d.contains("content") && !d["content"].is_null()) {
            std::string t = d["content"].get<std::string>();
            textAcc += t;
            keep = emit(TextDelta{std::move(t)}) && keep;
        } else if (d.contains("reasoning_content") && !d["reasoning_content"].is_null()) {
            keep = emit(ThinkingDelta{d["reasoning_content"].get<std::string>()}) && keep;
        }

        // ---- ③ ★ L4 新增：工具调用增量 ----
        if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
            for (const auto& tc : d["tool_calls"]) {
                const int idx = tc.value("index", 0);
                ToolBuf& buf = toolBufs[idx];

                if (tc.contains("id") && !tc["id"].is_null())
                    buf.id = tc["id"].get<std::string>();

                if (tc.contains("function") && tc["function"].is_object()) {
                    const auto& f = tc["function"];
                    if (f.contains("name") && !f["name"].is_null()) {
                        buf.name = f["name"].get<std::string>();
                        // 名字一出现就意味着"有新的工具调用开始了"
                        keep = emit(ToolCallStart{buf.id, buf.name}) && keep;
                    }
                    if (f.contains("arguments") && !f["arguments"].is_null()) {
                        const std::string piece = f["arguments"].get<std::string>();
                        buf.raw += piece;              // ★ 只累积，不解析
                        keep = emit(ToolCallDelta{buf.id, piece}) && keep;
                    }
                }
            }
        }

        // ---- ④ finish_reason（永远最后）----
        if (ch.contains("finish_reason") && !ch["finish_reason"].is_null()) {
            finishReason = fromOpenAIFinish(ch["finish_reason"].get<std::string>());

            // ★ 只在这一刻 parse 一次 —— 此时 arguments 才是完整 JSON。
            //   中途任何位置 parse 都会失败（半个 JSON），而失败信息看起来像
            //   "模型给了非法参数"，会让你怀疑模型而不是怀疑时序。
            for (auto& [idx, buf] : toolBufs) {
                if (buf.id.empty()) continue;
                json args = json::parse(buf.raw.empty() ? "{}" : buf.raw, nullptr, false);
                if (args.is_discarded()) {
                    // 模型幻觉出了非法 JSON —— 容错成空对象，让参数校验去报错。
                    // 这样模型下一轮能看到"缺少必需字段"并自己修正。
                    args = json::object();
                }
                calls.push_back(ToolCall{buf.id, buf.name, std::move(args)});
                keep = emit(ToolCallDone{buf.id, calls.back().args}) && keep;
            }
            toolBufs.clear();
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
        spec.headers.push_back("X-Accel-Buffering: no");

        for (int attempt = 0; attempt < cfg_.retry.maxAttempts; ++attempt) {
            HttpResponseInfo info;

            if (sctx) {
                info = transport_->postStream(spec, [sctx](const char* d, std::size_t n) {
                    return sctx->feed(d, n);
                });
                // ★ 已经开始吐 token 就不再重试：重试 = 重新计费 + 用户看到重复内容。
                //   （严格做法是记录"是否已收到首个字节"，见 L6 的复核项）
                if (!sctx->textAcc.empty() || !sctx->calls.empty()) {
                    out.code           = info.statusCode;
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
// ===========================================================================
class OpenAIProvider final : public HttpProviderBase {
public:
    explicit OpenAIProvider(ProviderConfig cfg) : HttpProviderBase(std::move(cfg)) {}

    std::string_view id() const override { return cfg_.kind; }
    int  contextWindow() const override { return cfg_.contextWindow; }
    bool supportsTools() const override { return true; }
    bool supportsStreaming() const override { return true; }
    bool supportsPromptCaching() const override { return false; }

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
        if (sctx) body["stream_options"] = {{"include_usage", true}};

        std::vector<std::string> headers{"Authorization: Bearer " + cfg_.apiKey};
        auto r = execute(cfg_.baseUrl + "/chat/completions", body.dump(), headers, sctx);

        if (r.code != 200)
            throw std::runtime_error("openai http " + std::to_string(r.code) + ": " +
                                     r.body.substr(0, 300));

        if (sctx) {
            for (auto& ev : sctx->parser.flush()) sctx->dispatch(ev);

            ChatResponse resp;
            resp.usage           = sctx->usage;
            resp.message.role    = Role::Assistant;
            resp.message.content = sctx->textAcc;
            // ★ 工具调用必须挂回 message —— 下一轮要把它原样发回去，
            //   否则模型会重复调用同一个工具（它不知道自己已经调过了）。
            resp.message.toolCalls = sctx->calls;
            // 有工具调用时 finish_reason 一律归一为 ToolUse：
            // 有些网关在带 tool_calls 时仍然回 "stop"，不能信它。
            resp.finishReason = sctx->calls.empty() ? sctx->finishReason
                                                    : FinishReason::ToolUse;
            resp.message.finishReason = resp.finishReason;
            return resp;
        }
        return parseResponse(json::parse(r.body));
    }

    json toRequest(const ChatRequest& req, bool stream) const {
        json j;
        j["model"]      = req.model.empty() ? cfg_.model : req.model;
        j["stream"]     = stream;
        j["max_tokens"] = req.maxTokens;
        if (req.temperature)            j["temperature"] = *req.temperature;
        if (!req.stopSequences.empty()) j["stop"] = req.stopSequences;

        json msgs = json::array();
        if (req.system) msgs.push_back({{"role", "system"}, {"content", *req.system}});
        for (const auto& m : req.messages) msgs.push_back(messageToJson(m));
        j["messages"] = msgs;

        // ---- ★ L4 新增：下发工具定义 ----
        // 顺序即缓存：tools 数组顺序一变，整个前缀缓存就失效。
        // 所以 defs() 的顺序必须稳定（ToolRegistry 保证），这里不做任何排序。
        if (!req.tools.empty()) {
            json tools = json::array();
            for (const auto& t : req.tools) {
                tools.push_back({{"type", "function"},
                                 {"function", {{"name", t.name},
                                               {"description", t.description},
                                               {"parameters", t.inputSchema}}}});
            }
            j["tools"] = tools;
            switch (req.toolChoice) {
                case ToolChoice::Auto:     j["tool_choice"] = "auto"; break;
                case ToolChoice::None:     j["tool_choice"] = "none"; break;
                case ToolChoice::Required: j["tool_choice"] = "required"; break;
                case ToolChoice::Named:
                    j["tool_choice"] = {{"type", "function"},
                                        {"function", {{"name", req.forcedToolName.value_or("")}}}};
                    break;
            }
        }
        return j;
    }

    // -----------------------------------------------------------------------
    //  Message → OpenAI JSON
    //
    //  三种形态，缺一个模型就不认账：
    //    · 普通消息          {role, content}
    //    · assistant 带工具  {role, content, tool_calls:[{id,type,function:{name,arguments}}]}
    //                        ★ arguments 必须是 **JSON 字符串**，不是对象
    //    · tool 结果         {role:"tool", tool_call_id, content}
    // -----------------------------------------------------------------------
    static json messageToJson(const Message& m) {
        json j{{"role", roleName(m.role)}, {"content", m.content}};

        if (m.role == Role::Tool) {
            j["tool_call_id"] = m.toolCallId;
            return j;
        }
        if (!m.toolCalls.empty()) {
            json tcs = json::array();
            for (const auto& tc : m.toolCalls) {
                tcs.push_back({{"id", tc.id},
                               {"type", "function"},
                               {"function", {{"name", tc.name},
                                             {"arguments", tc.args.dump()}}}});  // 字符串！
            }
            j["tool_calls"] = tcs;
            // 只调工具、没说话时 content 是空串 —— 有些实现会拒绝空 content
            if (m.content.empty()) j.erase("content");
        }
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

    static ChatResponse parseResponse(const json& j) {
        ChatResponse r;
        const auto& choice = j["choices"][0];
        const auto& msg = choice["message"];
        r.message.role = Role::Assistant;
        if (msg.contains("content") && !msg["content"].is_null())
            r.message.content = msg["content"].get<std::string>();

        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
                ToolCall c;
                c.id   = tc.value("id", "");
                c.name = tc.contains("function") ? tc["function"].value("name", "") : "";
                // arguments 是 JSON 字符串，可能不合法（模型幻觉）→ 容错
                const std::string rawArgs =
                    tc.contains("function") ? tc["function"].value("arguments", "{}") : "{}";
                c.args = json::parse(rawArgs, nullptr, false);
                if (c.args.is_discarded()) c.args = json::object();
                r.message.toolCalls.push_back(std::move(c));
            }
            r.finishReason = FinishReason::ToolUse;
        } else {
            r.finishReason = StreamCtx::fromOpenAIFinish(choice.value("finish_reason", "stop"));
        }
        r.message.finishReason = r.finishReason;

        if (j.contains("usage") && !j["usage"].is_null()) {
            r.usage.inputTokens  = j["usage"].value("prompt_tokens", 0);
            r.usage.outputTokens = j["usage"].value("completion_tokens", 0);
        }
        return r;
    }
};

}  // namespace

// ===========================================================================
//  工厂
// ===========================================================================
std::unique_ptr<IModelProvider> createProvider(const ProviderConfig& cfg) {
    return std::make_unique<OpenAIProvider>(cfg);
}

}  // namespace af
