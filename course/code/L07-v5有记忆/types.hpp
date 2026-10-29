// ---------------------------------------------------------------------------
//  L2 · types.hpp —— 全模块共享的类型契约
//
//  这是整个项目里**最稳定**的文件：谁都能依赖它，它不依赖任何项目内文件。
//  （L1 的分层规则 R3：types.hpp 在最底层）
//
//  三个关键设计决策（讲义 §3.2 有完整论证）：
//    1. ChatChunk 用 std::variant 而不是 struct —— 强迫调用方穷尽处理所有事件
//    2. Turn 放在这里而不是 agent_loop.hpp —— 避免循环依赖
//    3. Message 用值语义而不是继承 —— 它要被大量拷贝，虚函数 × 拷贝 = 性能灾难
//
//  ★ 这个文件会在后续讲次里"长大"（L2 就规划好了长大点）：
//      L7 加 CompactionEvent（✅ 本讲已加）    L10 加 CheckpointEvent
//
//  ★ L7 对本文件做了**两类**改动，两者的性质完全不同，必须分清：
//      ① 【加】新增 `CompactionEvent` —— 契约长大（CR-3 允许，且 L2 已预告）
//      ② 【修】`BudgetConfig::forLayer(L5_History)` 的返回值语义改对
//         —— 它从 L2 起就是**死代码**（没有任何调用点）。L7 第一次调用它，
//            一调用就发现它返回的是"全部可用预算"，而不是"扣掉前五层之后的剩余"。
//            死代码不会报错，只会**在你开始依赖它的那一刻**把错误暴露出来。
//            这与 L6 的 K-15（注释里的"未来计划"未必成立）是同一族问题：
//            **没有调用点的地方，就是没有被验证过的地方。**
// ---------------------------------------------------------------------------
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace af {

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

// ---------------------------------------------------------------------------
//  std::visit 的惯用写法：把多个 lambda 合成一个重载集。
//  用它处理 ChatChunk，编译器会强制你穷尽所有事件类型。
// ---------------------------------------------------------------------------
template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// ============================================================================
//  文本卫生（UTF-8）
//
//  为什么需要：nlohmann::json 的 dump() 遇到非法 UTF-8 会抛 type_error.316。
//  于是"仓库里有个 GBK 编码的源文件"这种再普通不过的现实，
//  会让整个 turn 崩掉，而且报错信息完全指不到根因。
//
//  规则：所有外部字节（命令行参数、文件内容、命令输出）
//       在进入上下文之前必须过一遍 sanitizeUtf8。**这是边界，不是可选项。**
//
//  ★ 这条不是"预防性设计"，是本课程真实踩过的事故。
//    对应需求规格里的约束 C-4。
// ============================================================================
inline bool isValidUtf8(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = 0;
        if      (c < 0x80)                n = 1;
        else if ((c & 0xE0) == 0xC0)      n = 2;
        else if ((c & 0xF0) == 0xE0)      n = 3;
        else if ((c & 0xF8) == 0xF0)      n = 4;
        else return false;

        if (i + std::size_t(n) > s.size()) return false;
        for (int k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(s[i + std::size_t(k)]) & 0xC0) != 0x80) return false;
        }
        i += std::size_t(n);
    }
    return true;
}

// 非法字节替换成 U+FFFD（�），保证下游 JSON 序列化永不抛异常
inline std::string sanitizeUtf8(std::string_view in) {
    if (isValidUtf8(in)) return std::string(in);     // 快路径：绝大多数内容合法
    std::string out;
    out.reserve(in.size() + 16);
    std::size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        int n = 0;
        if      (c < 0x80)                n = 1;
        else if ((c & 0xE0) == 0xC0)      n = 2;
        else if ((c & 0xF0) == 0xE0)      n = 3;
        else if ((c & 0xF8) == 0xF0)      n = 4;

        bool ok = (n != 0) && (i + std::size_t(n) <= in.size());
        for (int k = 1; ok && k < n; ++k) {
            if ((static_cast<unsigned char>(in[i + std::size_t(k)]) & 0xC0) != 0x80) ok = false;
        }
        if (ok) { out.append(in.substr(i, std::size_t(n))); i += std::size_t(n); }
        else    { out += "\xEF\xBF\xBD";                    i += 1; }   // U+FFFD
    }
    return out;
}

// ============================================================================
//  ★ L7 新增：按字节截断时的 UTF-8 安全（K-21）
//
//  `sanitizeUtf8` 管的是**外部进来的**字节。本讲发现还有第二类地方会产出非法
//  UTF-8：**我们自己按字节偏移截断**。
//
//      内容 = "...中文..."（每字 3 字节）  截到第 300 字节
//      → 第 300 字节落在某个汉字中间 → 产生半截字节序列
//      → 下一轮 json::dump() 抛 type_error.316
//      → 报错说"invalid UTF-8 byte at index 378: 0x0A"，**指向截断点后面那个换行**，
//        完全指不到"是你截断截坏的"
//
//  症状特别迷惑：只在"内容里有中文/emoji **且** 触发截断"时出现。
//  纯英文内容永远复现不了 —— 于是它在中文项目里炸，在英文项目里"没问题"。
//
//  规则：**只要按字节切，就必须先吸附到字符边界。**
//  这不是"注意一下"，是可以写成一行函数的事 —— 所以更要写成函数，
//  否则它会在第 6 个地方被重新忘记一次（本讲找到 5 处）。
// ============================================================================
inline std::size_t utf8Snap(std::string_view s, std::size_t pos) noexcept {
    if (pos >= s.size()) return s.size();
    // 若 pos 处是一个 continuation byte（10xxxxxx），向左退到一个 lead byte
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

// 取前 maxBytes 个字节，但保证不切在字符中间
inline std::string utf8Prefix(std::string_view s, std::size_t maxBytes) {
    return std::string(s.substr(0, utf8Snap(s, maxBytes)));
}

// 就地截断（给 std::string 用）
inline void utf8Resize(std::string& s, std::size_t maxBytes) {
    s.resize(utf8Snap(s, maxBytes));
}

// ============================================================================
//  角色与消息
// ============================================================================

enum class Role { System, User, Assistant, Tool };

// 服务端真实返回的停止原因。**不要猜**——
// 猜的后果：finish_reason='length'（被截断）会被当成正常结束，用户拿到半截答案。
enum class FinishReason { Stop, ToolUse, Length, ContentFilter, Other };

struct ToolCall {
    std::string id;      // provider 给的调用 id，回填 tool_result 时必须原样带回
    std::string name;
    json        args;    // 已经完整解析的参数
};

struct Usage {
    int inputTokens      = 0;   // 未命中缓存的输入
    int outputTokens     = 0;
    int cacheReadTokens  = 0;   // 命中缓存（便宜）
    int cacheWriteTokens = 0;   // 写入缓存（稍贵）
    std::optional<double> costUsd;

    // 真实上下文占用。input 只是"未缓存"那部分，必须加上 cache 才是总量 ——
    // 压缩触发判断用的就是这个值，少算会导致压缩永不触发。
    int total() const noexcept {
        return inputTokens + outputTokens + cacheReadTokens + cacheWriteTokens;
    }
    double cacheHitRate() const noexcept {
        const int denom = inputTokens + cacheReadTokens + cacheWriteTokens;
        return denom == 0 ? 0.0 : double(cacheReadTokens) / double(denom);
    }
};

// 扁平消息列表里的一条。见 ADR-1。
struct Message {
    Role        role = Role::User;
    std::string content;

    // assistant 产出
    std::vector<ToolCall> toolCalls;
    FinishReason          finishReason = FinishReason::Stop;

    // tool 角色专用：必须与产生它的 assistant.toolCalls[].id 配对
    std::string toolCallId;

    // 估算值（真实值来自 provider 返回的 usage）。mutable 是为了在 const 上下文里缓存
    mutable int approxTokens = 0;

    bool isToolResult() const noexcept { return role == Role::Tool; }
    bool hasToolCalls() const noexcept { return !toolCalls.empty(); }
};

// ============================================================================
//  工具定义
// ============================================================================

enum class RiskLevel { Read = 0, Write = 1, Exec = 2, Network = 3 };

struct ToolDef {
    std::string name;
    std::string description;   // 给模型看的"文档"，质量直接决定调用准确率
    json        inputSchema;   // JSON Schema
    RiskLevel   risk = RiskLevel::Read;
    int         timeoutMs = 30'000;

    // 超过此字符数则落盘，上下文里只留预览（压缩的 L0 层）。
    // Read 工具必须设为 SIZE_MAX 豁免，否则会形成
    // "读文件 → 落盘 → 再去读那个落盘文件" 的循环依赖。
    std::size_t persistThresholdChars = 50'000;
};

struct ToolResult {
    std::string callId;
    std::string content;
    bool        isError = false;
    bool        persisted = false;        // 内容已落盘，content 只是预览
    std::string persistedPath;

    static ToolResult ok(std::string id, std::string c) {
        return ToolResult{std::move(id), std::move(c), false, false, {}};
    }
    static ToolResult error(std::string id, std::string c) {
        return ToolResult{std::move(id), std::move(c), true, false, {}};
    }

    // 转成回填给模型的 Message。
    // 空串要替换掉 —— 部分 provider 会拒绝 content="" 的 tool 消息。
    Message toMessage() const {
        Message m;
        m.role       = Role::Tool;
        m.toolCallId = callId;
        m.content    = content.empty() ? "(empty)" : content;
        return m;
    }
};

// ============================================================================
//  流式事件
//
//  ★ 用 variant 而不是"一个大 struct 里放所有字段"的理由：
//    变体会强制 std::visit 穷尽所有分支 —— 漏掉一个事件类型编译不过。
//    而大 struct 方案里，"忘了处理 DoneChunk"是完全静默的。
// ============================================================================

struct TextDelta     { std::string text; };
struct ThinkingDelta { std::string text; };
struct ToolCallStart { std::string id, name; };
struct ToolCallDelta { std::string id, partialJson; };   // 增量 JSON 片段，不可单独 parse
struct ToolCallDone  { std::string id; json args; };
struct UsageChunk    { Usage usage; };
struct ErrorChunk    { std::string message; bool retryable = false; };
struct DoneChunk     { FinishReason reason; };

using ChatChunk = std::variant<TextDelta, ThinkingDelta, ToolCallStart, ToolCallDelta,
                               ToolCallDone, UsageChunk, ErrorChunk, DoneChunk>;

// ============================================================================
//  请求 / 响应
// ============================================================================

enum class ToolChoice { Auto, None, Required, Named };

struct CacheControl {
    bool enabled = true;
    // 在系统提示末尾打断点所需的最小前缀长度，太短不值得缓存
    int minPrefixTokens = 1024;
};

struct TraceMetadata {
    std::string traceId, sessionId, turnId;
};

struct ChatRequest {
    std::string model;                          // 留空则由 provider 用配置里的默认值
    std::optional<std::string> system;          // Anthropic 把它独立在顶层
    std::vector<Message> messages;
    std::vector<ToolDef> tools;                 // ★ 顺序必须稳定，否则缓存永不命中
    ToolChoice  toolChoice = ToolChoice::Auto;
    std::optional<std::string> forcedToolName;
    int         maxTokens = 8'192;
    std::optional<double> temperature;
    std::vector<std::string> stopSequences;
    CacheControl cacheControl;
    std::optional<std::stop_token> stop;        // C++20 取消传播
    TraceMetadata trace;
};

struct ChatResponse {
    Message      message;                       // role == Assistant
    Usage        usage;
    FinishReason finishReason = FinishReason::Stop;
};

// ============================================================================
//  循环控制
// ============================================================================

// 六道闸门。一道都不能少 —— 见 L6。
struct LoopLimits {
    int maxSteps             = 200;              // 闸门 1：步数上限
    int maxTokensSpent       = 2'000'000;        // 闸门 2：成本上限（必须有）
    Ms  maxWallClock{1'800'000};                 // 闸门 3：墙钟
    int maxConsecutiveErrors = 5;                // 闸门 4：连续错误
    int maxRepeatedCalls     = 3;                // 闸门 5：死循环检测
    std::optional<std::stop_token> stop;         // 闸门 6：用户中断
};

enum class LoopOutcome {
    Completed, UserAborted, StepLimitReached,
    BudgetExceeded, WallClockExceeded, TooManyErrors, Error
};

inline std::string_view toString(LoopOutcome o) noexcept {
    switch (o) {
        case LoopOutcome::Completed:         return "completed";
        case LoopOutcome::UserAborted:       return "user_aborted";
        case LoopOutcome::StepLimitReached:  return "step_limit";
        case LoopOutcome::BudgetExceeded:    return "budget_exceeded";
        case LoopOutcome::WallClockExceeded: return "wall_clock_exceeded";
        case LoopOutcome::TooManyErrors:     return "too_many_errors";
        case LoopOutcome::Error:             return "error";
    }
    return "unknown";
}

// ============================================================================
//  上下文分层（ADR-5）
// ============================================================================

enum class Layer {
    L0_System,       // 恒定        → 缓存前缀，永不裁剪
    L1_Project,      // CLAUDE.md
    L2_RepoMap,      // 符号骨架
    L3_Retrieved,    // 检索命中
    L4_ActiveFiles,  // 正在编辑的文件全文
    L5_History,      // 对话历史（可能已被压缩）
    L6_UserInput,    // 永远最后、永远完整
};

struct BudgetConfig {
    int contextWindow   = 200'000;
    int reserveForReply = 16'384;    // 给回复留的位置
    int maxOutputTokens = 8'192;

    double sysFrac = 0.05, projectFrac = 0.08, repoMapFrac = 0.05,
           retrievedFrac = 0.25, activeFrac = 0.30;
    // 剩余部分全部归历史层（L5）

    int available() const noexcept {
        return contextWindow - reserveForReply - maxOutputTokens;
    }

    // ★ L7 修正：L5 必须显式写成"扣掉前五层之后的剩余"。
    //
    //   原来的写法是 `default: return a;`，注释写着"L5 拿剩下的" ——
    //   但 `a` 是**全部可用预算**，不是"剩下的"。两个语义差了一个数量级。
    //   它之所以没造成事故，纯粹是因为从 L2 到 L6 **没有任何调用点**。
    //
    //   这个坑的形状值得记住：**注释说的是对的，代码是错的，而没人发现** ——
    //   因为死代码不会运行。第一次调用它的人是 L7 的压缩器。
    //   （教训：让"未使用的代码"保持正确的最省事办法，是**根本不要写它**。）
    int forLayer(Layer l) const noexcept {
        const int a = available();
        const int l0 = int(double(a) * sysFrac);
        const int l1 = int(double(a) * projectFrac);
        const int l2 = int(double(a) * repoMapFrac);
        const int l3 = int(double(a) * retrievedFrac);
        const int l4 = int(double(a) * activeFrac);
        switch (l) {
            case Layer::L0_System:      return l0;
            case Layer::L1_Project:     return l1;
            case Layer::L2_RepoMap:     return l2;
            case Layer::L3_Retrieved:   return l3;
            case Layer::L4_ActiveFiles: return l4;
            case Layer::L5_History:     return a - l0 - l1 - l2 - l3 - l4;  // ★ 剩余部分
            case Layer::L6_UserInput:   return a;      // 用户输入永不裁剪
        }
        return a;
    }
};

// ============================================================================
//  压缩事件（L7 新增）
//
//  ★ 为什么这个类型要放在 types.hpp，而不是 compaction.hpp：
//    它会出现在**三个**地方的契约里：
//      · AgentLoop 的对外接口（`compactionEvents()`）——循环要把它报给 UI
//      · Turn（下面那个字段）——它描述的是"这次 turn 的上下文发生了什么"
//      · 会话持久化（L10）——压缩边界必须落盘，否则 --resume 装不回原样
//    三个模块都要用它，而它们之间互相不认识 → 只能下沉到契约层。
//    （这与 L2 把 `Turn` 放在这里的理由**完全一样**，不是新规矩。）
//
//  ★ 它必须定义在 `Turn` **之前** —— 因为 `Turn` 里直接用它做成员。
//    这是 C++ 的硬要求，也是"把类型下沉到契约层"的一个具体代价：
//    下沉之后，定义顺序就得跟着排好。
//
//  ★ 逐级记录（不是只记一次）的原因：
//    压缩是**级联**——先试便宜的、不行再试贵的。只报"压缩了"会让你在
//    排查"为什么摘要质量这么差"时无从下手：到底是走了本地折叠还是走了
//    LLM 摘要？逐级记录让"花了多少钱、丢了哪些信息"变成可观测的。
// ============================================================================
struct CompactionEvent {
    int level = 0;              // 0..4，见 compaction.hpp 的五层级联
    int tokensBefore = 0;
    int tokensAfter  = 0;
    int keptMessages     = 0;
    int summarizedMessages = 0;
    std::string summary;        // 交接摘要（仅 L3 级非空）
    std::string detail;         // 人可读说明（给日志和 UI）
};

// ============================================================================
//  一次 turn 的完整状态
//
//  ★ 为什么放在 types.hpp 而不是 agent_loop.hpp —— 这是本讲最重要的一个"坑"：
//
//    AgentLoop 需要 Turn；ContextAssembler::assemble() 也需要 Turn。
//    如果 Turn 定义在 agent_loop.hpp，那么：
//        agent_loop.hpp  →  include context_budget.hpp
//        context_budget.hpp →  include agent_loop.hpp     ← 循环依赖！
//
//    而 agent_loop.hpp 已经需要 include context_budget.hpp（成员是 ContextAssembler&），
//    所以循环无法通过"调整包含顺序"解决。唯一正确的修法是把共享类型**下沉**。
//
//    这也解释了为什么"下沉共享类型"是分层架构的常规操作，
//    而不是"往 types.hpp 里堆东西"。
// ============================================================================
struct Turn {
    std::string input;                        // 用户输入（永远最后进上下文）
    std::string systemPrompt;                 // L0
    std::vector<Message> history;             // L5
    std::vector<Message> activeFiles;         // L4（编辑后必须立即刷新）
    std::vector<Message> retrieved;           // L3

    Usage       lastUsage;
    int         spentTokens = 0;
    std::string finalText;                    // 任务完成时的最终回答

    // ★ L7 新增：这一轮上下文被压缩过几次（供 UI / 测试断言用）
    std::vector<CompactionEvent> compactions;
};

}  // namespace af
