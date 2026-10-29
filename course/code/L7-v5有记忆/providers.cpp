// ---------------------------------------------------------------------------
//  L4 · providers.cpp —— 模型适配层：加入 function calling
//  L7 · 增补 —— 加入 Anthropic（**第二个协议**）+ 修掉 I-8
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
//                都必须能被序列化回去，否则模型不认账
//
//  ===========================================================================
//  ★ L7：为什么"第二个协议"值得单独占一讲的一大块
//  ===========================================================================
//  端口 ① 从 L3 就存在，但直到本讲之前，它只有**一个**实现。
//  一个只有一个实现的接口，它对不对是**看不出来**的 ——
//  它可能只是把"OpenAI 的形状"换了个名字。
//
//  加上 Anthropic 之后，接口的成色立刻显形。两家协议有三处**结构性差异**，
//  全都必须在这个文件里被吸收掉（上层一行都不用改）：
//
//    差异                       OpenAI                      Anthropic
//    ------------------------- --------------------------- ----------------------------
//    工具结果的表达           独立一条 role:"tool"         user 消息里的 tool_result 块
//    系统提示                 可以放在 messages 里          只能在顶层 system 字段
//    ★ 连续同角色消息         允许                         **要求严格交替**（必须合并）
//
//  第三条最要命，而且它不是"厂商怪癖"，是我们**自己会主动制造**的：
//  L7 的压缩把"交接摘要"插在锚点之后 →
//      [user(锚点需求), user(交接摘要), ...]  ← 连续两条 user
//  在 OpenAI 下没事，在 Anthropic 下直接被拒。
//  结论：**适配器必须归一化，而不是让上层去迁就厂商。**
//  这正是端口存在的意义 —— 上层只写一遍，差异全部落在这里。
//
//  ===========================================================================
//  ★ L7 顺手关掉 L3 的遗留 I-8
//  ===========================================================================
//  L3 的重试判据是"`StreamCtx::textAcc` 非空"—— 也就是"是不是已经吐出内容了"。
//  这是个**近似**：服务端先发了两帧心跳/空串时，textAcc 仍是空的 →
//  判成"还没开始" → 重试 → 重新计费 + 用户看到重复内容。
//  严格判据只有一个：**是否已经收到过字节**。收到过就不能重试，
//  因为你不知道那些字节已经产生了什么副作用。
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
//  流式上下文（OpenAI 形状）：SSE 事件流 → ChatChunk 事件流
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

    // ★ L7（I-8）：真正的"是否已经开始"判据 —— 收到**字节**就算，
    //   与"字节里有没有内容"无关。心跳、空串、注释帧都算。
    bool sawAnyByte = false;

    bool emit(const ChatChunk& c) { return onChunk ? onChunk(c) : true; }

    bool feed(const char* data, std::size_t len) {
        sawAnyByte = true;                       // ★ 进来就先记上，别等到 parse 完
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

    // -----------------------------------------------------------------------
    //  ★ L7：把"流数据怎么处理"抽成一个 sink，而不是写死 `StreamCtx`
    //
    //  为什么必须抽：Anthropic 是**另一套事件名**
    //  （message_start / content_block_delta / input_json_delta / message_delta），
    //  不可能复用 OpenAI 的 StreamCtx。如果 execute() 的签名写死 StreamCtx*，
    //  那么"重试 / 超时 / 错误分类 / HTTP 头"这些**与协议无关**的逻辑
    //  就得整份复制一遍 —— 复制出去的代码不会跟着修。
    //
    //  这是"端口与适配器"在同一文件内部的缩小版：
    //    · 与协议无关的：重试策略、超时、状态码分类  → 基类（这里）
    //    · 与协议有关的：怎么解析字节                → sink
    // -----------------------------------------------------------------------
    struct StreamSink {
        std::function<bool(const char* data, std::size_t len)> onData;
        // 是否已经开始收到字节 —— 决定"还能不能重试"（I-8 的修法）
        std::function<bool()> started;
    };

    HttpResult execute(const std::string& url, const std::string& payload,
                       const std::vector<std::string>& extraHeaders,
                       const StreamSink* sink) {
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

            if (sink) {
                info = transport_->postStream(spec, sink->onData);
                // ★ 已经开始收字节就不再重试：重试 = 重新计费 + 用户看到重复内容。
                //   L3~L6 判的是"textAcc 非空"（≈ 是否已吐出内容），那是个近似：
                //   服务端先发心跳/空串时它仍然为空 → 会错误地重试。
                //   L7 改成问 sink：**收到过字节就停手**（I-8 关闭）。
                if (sink->started && sink->started()) {
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

        StreamSink sink;
        if (sctx) {
            sink.onData  = [sctx](const char* d, std::size_t n) { return sctx->feed(d, n); };
            sink.started = [sctx] { return sctx->sawAnyByte; };
        }
        auto r = execute(cfg_.baseUrl + "/chat/completions", body.dump(), headers,
                         sctx ? &sink : nullptr);

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

// ===========================================================================
//  Anthropic 流式上下文
//
//  ★ 事件名完全不同，但**纪律完全一样**。这一份最能说明"协议无关的部分"
//    到底是什么 —— 两条纪律一个字都不用改：
//
//      ① 一个事件里可能同时带多种信息（比如 message_delta 同时带
//         stop_reason 和 output_tokens），所以**任何分支都不许提前 return**
//      ② 工具参数是**增量 JSON 片段**（input_json_delta），
//         只在 content_block_stop 的那一刻 parse 一次
//
//    "跟着事件走"是协议的差别，"怎么处理增量"是通用的规律。
//    规则没有变，变的只是事件的名字。
// ===========================================================================
class AnthropicStream {
public:
    SSEParser parser;
    std::function<bool(const ChatChunk&)> onChunk;

    struct Block {
        std::string type;        // "text" | "tool_use" | "thinking"
        std::string id, name;
        std::string raw;         // input_json_delta 的累积，**中途绝不 parse**
    };
    std::unordered_map<int, Block> blocks;

    std::string textAcc;
    std::vector<ToolCall> calls;
    Usage usage;
    FinishReason finishReason = FinishReason::Stop;
    bool sawAnyByte = false;

    bool emit(const ChatChunk& c) { return onChunk ? onChunk(c) : true; }

    bool feed(const char* data, std::size_t len) {
        sawAnyByte = true;
        for (auto& ev : parser.push(std::string_view(data, len))) {
            if (ev.isDone()) return false;      // message_stop
            if (!dispatch(ev)) return false;
        }
        return true;
    }

    bool dispatch(const SSEEvent& ev) {
        if (ev.data.empty()) return true;
        json j = json::parse(ev.data, nullptr, false);
        if (j.is_discarded()) return true;      // 坏帧跳过，不崩
        return handle(j);
    }

    static FinishReason fromStop(const std::string& s) {
        if (s == "end_turn")      return FinishReason::Stop;
        if (s == "tool_use")      return FinishReason::ToolUse;
        if (s == "max_tokens")    return FinishReason::Length;      // 截断，不是完成
        if (s == "stop_sequence") return FinishReason::Stop;
        if (s == "refusal")       return FinishReason::ContentFilter;
        return FinishReason::Other;
    }

    bool handle(const json& j) {
        bool keep = true;
        const std::string type = j.value("type", std::string());

        // ---- ① message_start：输入用量在这里 ----
        if (type == "message_start") {
            if (j.contains("message") && j["message"].contains("usage")) {
                const auto& u = j["message"]["usage"];
                usage.inputTokens      = u.value("input_tokens", 0);
                usage.cacheReadTokens  = u.value("cache_read_input_tokens", 0);
                usage.cacheWriteTokens = u.value("cache_creation_input_tokens", 0);
                keep = emit(UsageChunk{usage}) && keep;
            }
        }
        // ---- ② content_block_start：工具调用在这里"报名字"----
        else if (type == "content_block_start") {
            const int idx = j.value("index", 0);
            Block& b = blocks[idx];
            if (j.contains("content_block") && j["content_block"].is_object()) {
                const auto& cb = j["content_block"];
                b.type = cb.value("type", std::string());
                if (b.type == "tool_use") {
                    b.id   = cb.value("id", std::string());
                    b.name = cb.value("name", std::string());
                    keep = emit(ToolCallStart{b.id, b.name}) && keep;
                }
            }
        }
        // ---- ③ content_block_delta：正文 / 思考 / 参数片段 ----
        else if (type == "content_block_delta") {
            const int idx = j.value("index", 0);
            if (j.contains("delta") && j["delta"].is_object()) {
                const auto& d = j["delta"];
                const std::string dt = d.value("type", std::string());
                if (dt == "text_delta") {
                    std::string t = d.value("text", std::string());
                    textAcc += t;
                    keep = emit(TextDelta{std::move(t)}) && keep;
                } else if (dt == "thinking_delta") {
                    keep = emit(ThinkingDelta{d.value("thinking", std::string())}) && keep;
                } else if (dt == "input_json_delta") {
                    const std::string piece = d.value("partial_json", std::string());
                    Block& b = blocks[idx];
                    b.raw += piece;                    // ★ 只累积，不解析
                    keep = emit(ToolCallDelta{b.id, piece}) && keep;
                }
            }
        }
        // ---- ④ content_block_stop：工具参数到这一刻才是完整 JSON ----
        else if (type == "content_block_stop") {
            const int idx = j.value("index", 0);
            auto it = blocks.find(idx);
            if (it != blocks.end() && it->second.type == "tool_use") {
                Block& b = it->second;
                json args = json::parse(b.raw.empty() ? "{}" : b.raw, nullptr, false);
                if (args.is_discarded()) args = json::object();
                ToolCall c{b.id, b.name, std::move(args)};
                keep = emit(ToolCallDone{c.id, c.args}) && keep;
                calls.push_back(std::move(c));
                blocks.erase(it);
            }
        }
        // ---- ⑤ message_delta：stop_reason 与输出用量同帧 ----
        else if (type == "message_delta") {
            if (j.contains("delta") && j["delta"].is_object() &&
                j["delta"].contains("stop_reason") && j["delta"]["stop_reason"].is_string()) {
                finishReason = fromStop(j["delta"]["stop_reason"].get<std::string>());
            }
            if (j.contains("usage") && j["usage"].is_object()) {
                usage.outputTokens = j["usage"].value("output_tokens", usage.outputTokens);
                keep = emit(UsageChunk{usage}) && keep;
            }
        }
        // ---- ⑥ error：流内错误也要报出去，不能静默 ----
        else if (type == "error") {
            std::string msg = "anthropic stream error";
            if (j.contains("error") && j["error"].is_object())
                msg = j["error"].value("message", msg);
            keep = emit(ErrorChunk{msg, false}) && keep;
        }
        // ping / message_stop：前者忽略，后者由 isDone() 处理
        return keep;
    }
};

// ===========================================================================
//  Anthropic 实现
// ===========================================================================
class AnthropicProvider final : public HttpProviderBase {
public:
    explicit AnthropicProvider(ProviderConfig cfg) : HttpProviderBase(std::move(cfg)) {}

    std::string_view id() const override { return cfg_.kind; }
    int  contextWindow() const override { return cfg_.contextWindow; }
    bool supportsTools() const override { return true; }
    bool supportsStreaming() const override { return true; }
    // ★ 这一家**真的**支持提示缓存（cache_control: ephemeral），
    //   而 OpenAI 兼容端点没有等价物 —— 所以这个能力位在两家之间是**不同**的。
    //   一个接口方法只有在"存在返回不同值的实现"时才有信息量，
    //   这也是"第二个实现出现时才抽象"的另一面。
    bool supportsPromptCaching() const override { return true; }

    ChatResponse chat(const ChatRequest& req) override { return run(req, nullptr); }

    ChatResponse stream(const ChatRequest& req,
                        const std::function<bool(const ChatChunk&)>& cb) override {
        AnthropicStream s;
        s.onChunk = cb;
        return run(req, &s);
    }

    int countTokens(std::string_view text) override { return int(text.size() / 4) + 1; }

private:
    std::vector<std::string> authHeaders() const {
        return {"x-api-key: " + cfg_.apiKey,
                "anthropic-version: 2023-06-01"};
    }

    ChatResponse run(const ChatRequest& req, AnthropicStream* s) {
        json body = toRequest(req, s != nullptr);

        StreamSink sink;
        if (s) {
            sink.onData  = [s](const char* d, std::size_t n) { return s->feed(d, n); };
            sink.started = [s] { return s->sawAnyByte; };
        }
        // ★ 端点也不同：/messages（不是 /chat/completions）
        auto r = execute(cfg_.baseUrl + "/messages", body.dump(), authHeaders(),
                         s ? &sink : nullptr);

        if (r.code != 200)
            throw std::runtime_error("anthropic http " + std::to_string(r.code) + ": " +
                                     r.body.substr(0, 300));

        if (s) {
            for (auto& ev : s->parser.flush()) s->dispatch(ev);

            ChatResponse resp;
            resp.usage           = s->usage;
            resp.message.role    = Role::Assistant;
            resp.message.content = s->textAcc;
            resp.message.toolCalls = s->calls;
            resp.finishReason = s->calls.empty() ? s->finishReason : FinishReason::ToolUse;
            resp.message.finishReason = resp.finishReason;
            return resp;
        }
        return parseResponse(json::parse(r.body));
    }

    static json textBlock(const std::string& s) {
        json b = json::object();
        b["type"] = "text";
        b["text"] = s;
        return b;
    }

    // -----------------------------------------------------------------------
    //  请求构造
    //
    //  与 OpenAI 的差异（全部在这里吸收）：
    //    · system 是**顶层字段**，不是 messages 里的一条
    //    · max_tokens **必填**
    //    · tools 用 input_schema，形状不带 "type":"function" 外层
    //    · tool_choice 是对象（{"type":"auto"}），不是字符串
    //    · 流式**不需要** stream_options
    // -----------------------------------------------------------------------
    json toRequest(const ChatRequest& req, bool stream) const {
        json j;
        j["model"]      = req.model.empty() ? cfg_.model : req.model;
        j["stream"]     = stream;
        j["max_tokens"] = req.maxTokens;

        if (req.system) {
            if (req.cacheControl.enabled) {
                json blk = textBlock(*req.system);
                blk["cache_control"] = json::object({{"type", "ephemeral"}});
                j["system"] = json::array({blk});
            } else {
                j["system"] = *req.system;
            }
        }
        if (req.temperature)            j["temperature"] = *req.temperature;
        if (!req.stopSequences.empty()) j["stop_sequences"] = req.stopSequences;

        j["messages"] = toMessages(req.messages);

        if (!req.tools.empty()) {
            json tools = json::array();
            for (std::size_t i = 0; i < req.tools.size(); ++i) {
                const auto& t = req.tools[i];
                json tj = json::object();
                tj["name"]         = t.name;
                tj["description"]  = t.description;
                tj["input_schema"] = t.inputSchema;
                // 缓存断点打在**最后一个**工具上：缓存以"前缀"为单位，
                // 断点放在前缀的末尾才有意义（打在中间等于只缓存了前半段）。
                if (req.cacheControl.enabled && i + 1 == req.tools.size())
                    tj["cache_control"] = json::object({{"type", "ephemeral"}});
                tools.push_back(std::move(tj));
            }
            j["tools"] = tools;
            switch (req.toolChoice) {
                case ToolChoice::Auto:     j["tool_choice"] = json::object({{"type", "auto"}}); break;
                case ToolChoice::None:     j["tool_choice"] = json::object({{"type", "none"}}); break;
                case ToolChoice::Required: j["tool_choice"] = json::object({{"type", "any"}});  break;
                case ToolChoice::Named:
                    j["tool_choice"] = json::object(
                        {{"type", "tool"}, {"name", req.forcedToolName.value_or("")}});
                    break;
            }
        }
        return j;
    }

    // -----------------------------------------------------------------------
    //  内部扁平消息 → Anthropic 的 {role, content:[block...]}
    //
    //  ★ 三处结构性差异全在这里吸收，其中第 ③ 条是**我们自己制造的**：
    //    ③ 连续同角色必须合并 —— L7 的压缩会产出
    //       [user(锚点), user(交接摘要)]，在 OpenAI 下合法，在这里会被拒绝。
    // -----------------------------------------------------------------------
    static json toMessages(const std::vector<Message>& msgs) {
        json out = json::array();

        auto mergeInto = [&](const char* role, json blocks) {
            if (!out.empty() && out.back().value("role", std::string()) == role) {
                for (auto& b : blocks) out.back()["content"].push_back(std::move(b));
                return;
            }
            json m = json::object();
            m["role"]    = role;
            m["content"] = std::move(blocks);
            out.push_back(std::move(m));
        };

        for (const auto& m : msgs) {
            if (m.role == Role::System) {
                // 顶层 system 已经放好了；万一这里还有一条，就当普通文本带上
                json arr = json::array();
                arr.push_back(textBlock(m.content));
                mergeInto("user", std::move(arr));
                continue;
            }
            if (m.role == Role::Tool) {
                json blk = json::object();
                blk["type"]        = "tool_result";
                blk["tool_use_id"] = m.toolCallId;
                blk["content"]     = m.content.empty() ? "(empty)" : m.content;
                json arr = json::array();
                arr.push_back(std::move(blk));
                mergeInto("user", std::move(arr));
                continue;
            }

            json blocks = json::array();
            // 只调工具、没说话时 content 是空串 —— 这一家对空 text 块是容忍的，
            // 但空块没有信息，不发更省 token。
            if (!m.content.empty() || m.toolCalls.empty())
                blocks.push_back(textBlock(m.content));
            for (const auto& tc : m.toolCalls) {
                json b = json::object();
                b["type"]  = "tool_use";
                b["id"]    = tc.id;
                b["name"]  = tc.name;
                b["input"] = tc.args;
                blocks.push_back(std::move(b));
            }
            mergeInto(m.role == Role::Assistant ? "assistant" : "user", std::move(blocks));
        }

        // ★ 第一条必须是 user（硬要求）。压缩之后历史可能从 assistant 开始
        //   （比如 L4 丢弃把最前面的 user 也丢了）—— 补一条最小的开场，
        //   而不是让请求失败。**适配器的职责是让合法输入永远能发出去。**
        if (!out.empty() && out[0].value("role", std::string()) != "user") {
            json arr = json::array();
            arr.push_back(textBlock("(已压缩上下文，从这里继续)"));
            json filler = json::object();
            filler["role"]    = "user";
            filler["content"] = std::move(arr);
            json repl = json::array();
            repl.push_back(std::move(filler));
            for (auto& m : out) repl.push_back(std::move(m));
            out = std::move(repl);
        }
        return out;
    }

    static ChatResponse parseResponse(const json& j) {
        ChatResponse r;
        r.message.role = Role::Assistant;

        if (j.contains("content") && j["content"].is_array()) {
            for (const auto& b : j["content"]) {
                const std::string t = b.value("type", std::string());
                if (t == "text") {
                    r.message.content += b.value("text", std::string());
                } else if (t == "tool_use") {
                    ToolCall c;
                    c.id   = b.value("id", std::string());
                    c.name = b.value("name", std::string());
                    c.args = (b.contains("input") && b["input"].is_object())
                                 ? b["input"] : json::object();
                    r.message.toolCalls.push_back(std::move(c));
                }
            }
        }
        std::string stop = "end_turn";
        if (j.contains("stop_reason") && j["stop_reason"].is_string())
            stop = j["stop_reason"].get<std::string>();
        r.finishReason = r.message.toolCalls.empty()
                             ? AnthropicStream::fromStop(stop)
                             : FinishReason::ToolUse;
        r.message.finishReason = r.finishReason;

        if (j.contains("usage") && j["usage"].is_object()) {
            r.usage.inputTokens      = j["usage"].value("input_tokens", 0);
            r.usage.outputTokens     = j["usage"].value("output_tokens", 0);
            r.usage.cacheReadTokens  = j["usage"].value("cache_read_input_tokens", 0);
            r.usage.cacheWriteTokens = j["usage"].value("cache_creation_input_tokens", 0);
        }
        return r;
    }
};

}  // namespace

// ===========================================================================
//  工厂（P-16）
//
//  ★ 注意工厂是**唯一**知道"kind → 具体类"映射的地方。
//    其它任何代码都只认 `IModelProvider`。
//    这也是为什么"加一家厂商"的改动面是 1 个文件里的 3 行 ——
//    而不是"所有用到 provider 的地方"。
// ===========================================================================
std::unique_ptr<IModelProvider> createProvider(const ProviderConfig& cfg) {
    if (cfg.kind == "anthropic") return std::make_unique<AnthropicProvider>(cfg);
    return std::make_unique<OpenAIProvider>(cfg);
}

}  // namespace af
