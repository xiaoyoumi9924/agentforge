// ---------------------------------------------------------------------------
//  trace.hpp —— 旁路埋点：度量而不扭曲（L12，全课最后一个模块）
//
//  ★★ 本模块唯一的设计目标是这句话：
//     **把 Tracer 整个拔掉，系统的行为必须逐字节不变。**
//
//  这不是口号，它是用例 C51。要做到它，需要两条：
//    ① 关闭时所有方法都是 **no-op**（`if (!enabled_) return;` 在最前面）；
//    ② **没有任何外部代码读它的返回值来决定下一步做什么**。
//
//  ★ 最容易破这条规矩的方式：在 tracer 里发现某个工具很慢，就"顺手"加个缓存。
//    那一刻 tracer 就不再是旁路，而成了控制路径的一部分 ——
//    于是"开不开 trace"会改变系统行为，C51 立刻会红。
//
//  ★ 为什么它是独立模块（与 L7 压缩 / L8 索引 / L11 子代理同一条判据）：
//    它有自己的状态（事件缓冲）、自己的策略（容量/采样/落盘）、
//    自己的测试面（开销、自洽性、旁路性）。塞进 AgentLoop 会让那个类
//    同时承担"推进"和"记录推进"两件事。
//
//  ★ types.hpp 本讲**零改动**（CR-3，连续第五讲）：
//    TraceEvent / Tracer 只有 trace 用 → 放这里。
//    与 L9 的 UiEvent（ui/tui.hpp）、L10 的 CheckpointEvent（session_store.hpp）、
//    L11 的 SubagentSpec（subagent.hpp）是同一个判断。
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "model_provider.hpp"
#include "types.hpp"

namespace af {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//  事件种类
//
//  ★ 分成 event 与 span 两类，不是图省事：
//     event  —— 某一刻发生的事（工具开始、压缩触发、出错）：**只有一个时间点**
//     span   —— 一段有始有终的事（一次请求、一次工具执行）：**有开始/结束/时长**
//
//  ★ span 的时长是**唯一能回答"慢在哪儿"的东西**，而 event 只能回答"发生了什么"。
//    只记 event 的 trace 到最后一定会被问"那到底哪一步慢" —— 答不上来。
// ---------------------------------------------------------------------------
enum class TraceKind {
    Request,     // span：一次模型请求
    ToolExec,    // span：一次工具执行
    Subagent,    // span：一次子代理运行（★ 成本要单列，K-42）
    Compact,     // event：触发了一次压缩
    ToolDone,    // event：工具结束（成功/失败）
    Error,       // event：出错
    Notice,      // event：循环通知
};

inline const char* toString(TraceKind k) noexcept {
    switch (k) {
        case TraceKind::Request:  return "request";
        case TraceKind::ToolExec: return "tool_exec";
        case TraceKind::Subagent: return "subagent";
        case TraceKind::Compact:  return "compact";
        case TraceKind::ToolDone: return "tool_done";
        case TraceKind::Error:    return "error";
        case TraceKind::Notice:   return "notice";
    }
    return "?";
}

struct TraceEvent {
    int         seq     = 0;
    TraceKind   kind    = TraceKind::Notice;
    std::string name;
    int64_t     tMs     = 0;    // 相对起点（毫秒）
    int64_t     durUs   = 0;    // ★ span 时长（微秒）；event 恒为 0
    int64_t     ttftUs  = -1;   // ★ 首 token 延迟；只有 provider 量得到（<0 = 没量到）
    int         tokIn   = 0;
    int         tokOut  = 0;
    long long   chars   = 0;    // 当次请求上下文字符数（Request 时记）
    std::string note;
};

// ===========================================================================
//  旁路埋点器
// ===========================================================================
class Tracer {
public:
    // ★ 固定容量（K-48）：超出就丢最老的，并把丢弃条数报出来。
    //   为什么不能用无限数组：长跑任务可能有几十万次 chunk，
    //   每个都留一条 → 内存和落盘都会失控。
    //   ★ 而"丢弃"必须可见：静默采样会让"我看到的不是全部"变成隐形事实。
    //
    //  ★★ 用 deque 不用 vector：满了要丢**最老的**（头部）。
    //     vector::erase(begin()) 是 O(n) 搬运 —— 长会话里每加一条就搬几千个，
    //     整体退化成 O(n²)。**埋点自身的复杂度必须是 O(1)**，
    //     否则"开个 trace"会让系统随会话长度越来越慢，
    //     而这条曲线长得非常像"内存泄漏"或"模型变慢"，排查方向会完全跑偏（K-49）。
    static constexpr std::size_t kDefaultCapacity = 4096;

    explicit Tracer(bool enabled = true, std::size_t capacity = kDefaultCapacity);

    bool enabled() const noexcept { return enabled_; }

    // ---- 作用域：当前是不是"子代理里发出的请求" ----
    //
    //  ★ 为什么需要它：子代理的成本必须**单列**（K-42），
    //    而装饰器看不出自己是被主循环调用、还是被子代理调用 ——
    //    AgentLoop 是同一份代码，子代理只是"再 new 一个"（L11 的性质）。
    //
    //  ★ 为什么是**一个 bool 而不是一个参数**：
    //    装饰器是端口适配层，它不该知道"谁在用它"。
    //    作用域由**调用方**（Task 工具的闭包）在进入/离开时标记，
    //    这是唯一知道上下文的地方。
    void setInSubagent(bool v) noexcept { inSub_ = v; }
    bool inSubagent() const noexcept { return inSub_; }

    // ---- span ----
    //  ★ 返回 spanId（从 1 开始）；关闭时返回 0，且 endSpan(0) 是 no-op ——
    //    调用点因此可以完全不写 if，这是 P-18 类型擦除的小用法。
    int  beginSpan(TraceKind k, std::string name, std::string note = {});
    void endSpan(int id, int tokIn = 0, int tokOut = 0, long long chars = 0,
                 int64_t ttftUs = -1);

    // ---- event ----
    void event(TraceKind k, std::string name, std::string note = {});

    // ---- 读取 ----
    const std::deque<TraceEvent>& events() const noexcept { return events_; }
    std::size_t dropped() const noexcept { return dropped_; }

    struct Summary {
        int       requests  = 0;
        int       toolCalls = 0;
        int       steps     = 0;      // ★ 步数：回归集三个数之一
        int       tokIn     = 0;
        int       tokOut    = 0;
        int       subTok    = 0;      // ★ 子代理成本，单列（K-42）
        int64_t   wallMs    = 0;
        long long peakChars = 0;
        int       total() const { return tokIn + tokOut + subTok; }
    };
    Summary summary() const;

    // ---- 落盘：JSONL，与 L10 的会话日志**同一种形状** ----
    //  ★ 为什么复用而不是新造一种：半行容忍、追加语义、坏行只丢自己
    //    这些已验证的性质全部继承，且用同一套工具就能看。
    //    **格式一致性本身就是一种设计能力** —— 12 讲下来，日志只有一种形状。
    bool flush(const fs::path& p) const;

    // ---- 从 JSONL 读回（给 --cost 用；坏行容忍，与 session_store 同构）----
    static bool load(const fs::path& p, Summary& out, int& badLines);

private:
    void push(TraceEvent e);

    bool                     enabled_;
    std::size_t              capacity_;
    std::deque<TraceEvent>   events_;     // ★ 环形：满则从头部丢弃
    std::size_t              dropped_ = 0;
    int                      nextId_  = 0;
    int64_t                  t0_      = 0;
    bool                     inSub_   = false;
    // ★ 追加型日志的水位：flush 两次不能把事件写两遍。
    //   seq 单调递增，所以"只写 seq 大于上次的"就够了 ——
    //   这条性质也是 L10 会话日志里那条"恢复后不能重复追加"的同构版本。
    //   mutable：flush() 是 const 方法，但水位是"写到哪了"的内部状态。
    mutable int              lastFlushedSeq_ = 0;
};

// ===========================================================================
//  ★★ TracingProvider —— 用装饰器拿请求级埋点，一个字节都不改契约层
//
//  为什么是装饰器，而不是给 LoopHooks 加两个回调：
//    ② types.hpp 连续五讲零改动是 CR-3 的证据，为埋点破例不划算；
//    ② 更根本的是 —— **请求是从 provider 出去的**，
//       在"请求发生的地方"记它，比在"循环通知我的地方"记它更准：
//       TTFT、重试、取消都只有 provider 知道。
//
//  ★ 它必须**完全透传**：不加重试、不加缓存、不改返回值。
//    一旦它开始影响结果，C51（"关掉 trace 行为不变"）就会红。
//    这就是"旁路"这个词的实际含义 —— 不是"不重要的路"，是"不影响主干的路"。
//
//  ★ 与 L2 预留的 `RequestTrace` 的关系：那个结构体把"一次请求该记什么"
//    定下来了（ttftMs / totalMs / usage / cacheHitRate / retried），
//    本讲只是把它**接上线**。落点挂了 10 讲，现在才需要它。
// ===========================================================================
class TracingProvider : public IModelProvider {
public:
    TracingProvider(std::unique_ptr<IModelProvider> inner, Tracer& tracer)
        : inner_(std::move(inner)), tracer_(tracer) {}

    std::string_view id() const override { return inner_->id(); }
    int  contextWindow() const override { return inner_->contextWindow(); }
    bool supportsTools() const override { return inner_->supportsTools(); }
    bool supportsStreaming() const override { return inner_->supportsStreaming(); }
    bool supportsPromptCaching() const override { return inner_->supportsPromptCaching(); }
    int  countTokens(std::string_view text) override { return inner_->countTokens(text); }

    ChatResponse chat(const ChatRequest& req) override;
    ChatResponse stream(const ChatRequest& req,
                        const std::function<bool(const ChatChunk&)>& onChunk) override;

    // ★ 请求体有多大 —— 这是 /context 分层图的**第二来源**（C48）。
    //   它由 ChatRequest 对象现算，与"按层累加"是两条独立的路径。
    static long long requestChars(const ChatRequest& req) noexcept;

private:
    std::unique_ptr<IModelProvider> inner_;
    Tracer& tracer_;
};

// ===========================================================================
//  ★ 分层占用（给 /context）
//
//  为什么单独定义一份、而不直接用 ContextAssembler::LayerUsage：
//    LayerUsage 的每一层是 **token**（estimateTokens 每次都 +1，七层累积偏差）；
//    而画分层图必须**先记字节、最后换算一次** token，否则分层加不出总数（K-46）。
//
//  ★ 自洽不等于正确：七层加起来等于总数只能说明算术对，
//    不能说明"这七层就是线上真的那些字节"。
//    所以 C48 还要和第二来源（**服务端实际收到的**字节数）对照。
// ===========================================================================
struct LayerChars {
    long long l0 = 0;   // 系统提示
    long long l1 = 0;   // 项目知识（CLAUDE.md）
    long long l2 = 0;   // 仓库地图
    long long l3 = 0;   // 检索片段
    long long l4 = 0;   // 活动文件
    long long l5 = 0;   // 历史（含 tool_call 参数）
    long long l6 = 0;   // 用户输入
    long long l7 = 0;   // ★ 工具表（JSON Schema）—— 它也是要发出去的字节
    long long total() const { return l0+l1+l2+l3+l4+l5+l6+l7; }
};

}  // namespace af
