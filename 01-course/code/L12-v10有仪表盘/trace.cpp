// ---------------------------------------------------------------------------
//  trace.cpp —— 旁路埋点的实现
//
//  ★ 本文件里每一个 public 方法的第一行都是 `if (!enabled_) return ...;`。
//    这不是防御性编程，这是"旁路"的定义：
//    **关掉它，代价必须是零。**
//
//  ★ 落盘格式与 L10 的会话日志同构（JSONL、seq 递增、坏行容忍），
//    包括那条容易被忘的规则：**追加之前先补换行**（K-37）。
//    被杀的进程留下的最后一行没有 `\n`，不补的话新事件会粘在坏行上一起作废。
// ---------------------------------------------------------------------------
#include "trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace af {

using json = nlohmann::json;

namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

Tracer::Tracer(bool enabled, std::size_t capacity)
    : enabled_(enabled), capacity_(capacity ? capacity : kDefaultCapacity) {
    t0_ = nowMs();
}

// ===========================================================================
//  span
// ===========================================================================
int Tracer::beginSpan(TraceKind k, std::string name, std::string note) {
    if (!enabled_) return 0;            // ★ 旁路：关闭时零代价
    const int id = ++nextId_;
    TraceEvent e;
    e.seq  = id;
    e.kind = k;
    e.name = std::move(name);
    e.note = std::move(note);
    e.tMs  = nowMs() - t0_;
    e.durUs = -nowUs();                 // ★ 负数 = "尚未结束"的标记
    push(std::move(e));
    return id;
}

void Tracer::endSpan(int id, int tokIn, int tokOut, long long chars, int64_t ttftUs) {
    if (!enabled_ || id <= 0) return;
    // ★★ 必须**从后往前**找：span 通常刚开就关，尾部一次命中，O(1)。
    //    从前往后在事件多时是 O(n) —— 一次 endSpan 要扫几万条，
    //    整个会话退化成 O(n²)。实测：50k 事件下每事件 18µs → 改成反向后 <1µs（K-49）。
    //    ★ 这类 bug 只在"事件变多"时才现身，小会话完全测不出来，
    //      而它长得像"模型变慢 / 内存泄漏"，排查方向会完全跑偏。
    //
    //    事件也可能被环形缓冲挤掉了 → 找不到就算了，不影响正确性。
    for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
        TraceEvent& e = *it;
        if (e.seq != id || e.durUs >= 0) continue;
        e.durUs  = nowUs() + e.durUs;   // 当初存的是 -start
        e.tokIn  = tokIn;
        e.tokOut = tokOut;
        e.chars  = chars;
        e.ttftUs = ttftUs;
        return;
    }
}

// ===========================================================================
//  event
// ===========================================================================
void Tracer::event(TraceKind k, std::string name, std::string note) {
    if (!enabled_) return;
    TraceEvent e;
    e.seq  = ++nextId_;
    e.kind = k;
    e.name = std::move(name);
    e.note = std::move(note);
    e.tMs  = nowMs() - t0_;
    push(std::move(e));
}

void Tracer::push(TraceEvent e) {
    // ★ 环形缓冲：满了就从头部丢，并**记下丢了多少**（K-48）
    //   deque::pop_front 是 O(1)；vector::erase(begin()) 是 O(n)（K-49）。
    if (events_.size() >= capacity_) {
        events_.pop_front();
        ++dropped_;
    }
    events_.push_back(std::move(e));
}

// ===========================================================================
//  汇总
// ===========================================================================
Tracer::Summary Tracer::summary() const {
    Summary s;
    s.wallMs = nowMs() - t0_;
    for (const auto& e : events_) {
        switch (e.kind) {
            case TraceKind::Request:
                ++s.requests;
                s.tokIn  += e.tokIn;
                s.tokOut += e.tokOut;
                s.peakChars = std::max(s.peakChars, e.chars);
                break;
            case TraceKind::ToolExec:
                // ★ 只数 span，不数 ToolDone —— 一次工具执行会产生**两条**
                //   （开始/结束各一），两个都数就翻倍。
                //   ★ 这类"看着都对、结果差一倍"的 bug 不会报错，
                //     只会让 /cost 和 /regress 悄悄失真。
                ++s.toolCalls;
                break;
            case TraceKind::ToolDone:
                break;
            case TraceKind::Subagent:
                // ★ 子代理的 token **不进** 主循环的 tokIn/tokOut，单独累（K-42）
                s.subTok += e.tokIn + e.tokOut;
                break;
            case TraceKind::Notice:
                ++s.steps;
                break;
            default:
                break;
        }
    }
    return s;
}

// ===========================================================================
//  落盘（JSONL，与 L10 同构）
// ===========================================================================
bool Tracer::flush(const fs::path& p) const {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

    // ★ K-37：追加之前先补换行。文件最后一个字节不是 '\n' 时，
    //   不补的话新写的第一行会粘在坏行上一起作废 ——
    //   症状是"恢复后总少一条"，少的永远是**最新**那条。
    {
        std::error_code ec2;
        if (fs::exists(p, ec2) && fs::file_size(p, ec2) > 0) {
            std::ifstream in(p, std::ios::binary | std::ios::ate);
            if (in) {
                // ★ tellg() 返回的是 fpos 不是整数，`sz - 1` 在 MSVC 下有歧义
                //   （C2666：fpos::operator- 与内置 operator- 都匹配）。
                //   必须先转成 streamoff 再做算术。
                const std::streamoff sz = static_cast<std::streamoff>(in.tellg());
                if (sz > 0) {
                    in.seekg(sz - 1, std::ios::beg);
                    char last = 0;
                    in.read(&last, 1);
                    in.close();
                    if (last != '\n') {
                        std::ofstream fix(p, std::ios::binary | std::ios::app);
                        fix << '\n';
                    }
                }
            }
        }
    }

    std::ofstream out(p, std::ios::binary | std::ios::app);
    if (!out) return false;

    const Summary s = summary();
    out << json{{"t", "meta"}, {"v", 1}, {"dropped", dropped_},
                {"wall_ms", s.wallMs}, {"events", events_.size()}}.dump() << "\n";
    for (const auto& e : events_) {
        if (e.seq <= lastFlushedSeq_) continue;   // ★ 不重复写（水位）
        json j;
        j["seq"]  = e.seq;
        j["t"]    = toString(e.kind);
        j["name"] = e.name;
        j["at_ms"] = e.tMs;
        if (e.durUs >= 0) j["dur_us"] = e.durUs;
        if (e.ttftUs >= 0) j["ttft_us"] = e.ttftUs;
        if (e.tokIn || e.tokOut) { j["tok_in"] = e.tokIn; j["tok_out"] = e.tokOut; }
        if (e.chars) j["chars"] = e.chars;
        if (!e.note.empty()) j["note"] = e.note;
        out << j.dump() << "\n";
    }
    if (!events_.empty()) lastFlushedSeq_ = events_.back().seq;
    return true;
}

// ===========================================================================
//  读回（坏行容忍，与 session_store 同构）
// ===========================================================================
bool Tracer::load(const fs::path& p, Summary& out, int& badLines) {
    badLines = 0;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;

    out = Summary{};
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            const json j = json::parse(line);
            ++n;
            const std::string t = j.value("t", std::string{});
            if (t == "meta") {
                out.wallMs = j.value("wall_ms", int64_t{0});
                continue;
            }
            const int ti = j.value("tok_in", 0);
            const int to = j.value("tok_out", 0);
            if (t == "request")  { ++out.requests; out.tokIn += ti; out.tokOut += to; }
            else if (t == "subagent") out.subTok += ti + to;
            else if (t == "tool_exec") ++out.toolCalls;      // ★ 只数 span，避免翻倍
            else if (t == "notice") ++out.steps;
        } catch (...) {
            ++badLines;      // ★ 坏行只丢自己，且必须报出来（INV-26）
        }
    }
    return n > 0 || badLines > 0;
}

// ===========================================================================
//  TracingProvider —— 装饰器：请求级埋点，不碰契约层
// ===========================================================================
long long TracingProvider::requestChars(const ChatRequest& req) noexcept {
    long long n = 0;
    // ★ noexcept 里不能让 dump() 抛出去（那会直接 terminate）。
    //   JSON 序列化理论上会抛，所以这里显式兜住 ——
    //   **度量代码绝不能成为崩溃来源**，这是旁路设计的底线。
    try {
        if (req.system) n += static_cast<long long>(req.system->size());
        n += static_cast<long long>(req.model.size());
        for (const auto& m : req.messages) {
            n += static_cast<long long>(m.content.size());
            for (const auto& tc : m.toolCalls) {
                n += static_cast<long long>(tc.name.size());
                n += static_cast<long long>(tc.args.dump().size());
            }
        }
        for (const auto& t : req.tools) {
            n += static_cast<long long>(t.name.size());
            n += static_cast<long long>(t.description.size());
            n += static_cast<long long>(t.inputSchema.dump().size());
        }
    } catch (...) {
        return n;
    }
    return n;
}

ChatResponse TracingProvider::chat(const ChatRequest& req) {
    // ★ 关闭时**直接透传**，连 requestChars 都不算 ——
    //   否则"关掉埋点"只是关掉了记录，开销还在（C51 要的就是零开销）。
    if (!tracer_.enabled()) return inner_->chat(req);
    const int id = tracer_.beginSpan(
        tracer_.inSubagent() ? TraceKind::Subagent : TraceKind::Request,
        std::string(inner_->id()));
    const long long chars = requestChars(req);
    ChatResponse r = inner_->chat(req);
    tracer_.endSpan(id, r.usage.inputTokens, r.usage.outputTokens, chars);
    return r;
}

ChatResponse TracingProvider::stream(const ChatRequest& req,
                                     const std::function<bool(const ChatChunk&)>& onChunk) {
    if (!tracer_.enabled()) return inner_->stream(req, onChunk);
    const int id = tracer_.beginSpan(
        tracer_.inSubagent() ? TraceKind::Subagent : TraceKind::Request,
        std::string(inner_->id()));
    const long long chars     = requestChars(req);
    const int64_t   startedUs = nowUs();
    int64_t         ttftUs    = -1;

    // ★ 包装回调只做一件事：记下第一个**非空** text 的时刻。
    //   非空很重要 —— L3 踩过：首帧可能是 content:""（空串不是 null），
    //   拿它当首 token 会把 TTFT 打成 0ms，并让"有没有收到内容"一起失效。
    //
    // ★★ onChunk 的返回值必须**原样透传**：false = 调用方要求取消，
    //    吞掉它就等于让取消失效（L6 的闸门 6 / 用户 Ctrl-C 会失灵）。
    auto wrapped = [&](const ChatChunk& c) -> bool {
        if (ttftUs < 0 && std::holds_alternative<TextDelta>(c) &&
            !std::get<TextDelta>(c).text.empty())
            ttftUs = nowUs() - startedUs;
        return onChunk(c);
    };

    ChatResponse r = inner_->stream(req, wrapped);
    tracer_.endSpan(id, r.usage.inputTokens, r.usage.outputTokens, chars, ttftUs);
    return r;
}

}  // namespace af
