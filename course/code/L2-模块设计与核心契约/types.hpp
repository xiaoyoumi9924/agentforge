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
//  ★ 这个文件会在后续讲次里"长大"：
//      L7 加 CompactionEvent      L10 加 CheckpointEvent
//    本讲只放 L2 阶段确定下来的内容 —— 这是刻意的，
//    因为"提前定义用不上的类型"和"提前建抽象"一样是过度设计。
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
    int forLayer(Layer l) const noexcept {
        const int a = available();
        switch (l) {
            case Layer::L0_System:      return int(double(a) * sysFrac);
            case Layer::L1_Project:     return int(double(a) * projectFrac);
            case Layer::L2_RepoMap:     return int(double(a) * repoMapFrac);
            case Layer::L3_Retrieved:   return int(double(a) * retrievedFrac);
            case Layer::L4_ActiveFiles: return int(double(a) * activeFrac);
            default:                    return a;      // L5 拿剩下的
        }
    }
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
};

}  // namespace af
