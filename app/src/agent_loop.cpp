// ---------------------------------------------------------------------------
//  L5 · agent_loop.cpp —— Agent 循环内核（模块 ⑦ / 机制 W2）
//
//  第一性循环（全课的骨架）：
//      while (模型返回工具调用) { 执行 → 捕获 → 追加到上下文 → 再调模型 }
//
//  本讲实现：
//    · 循环主体（多步推理 + 工具执行 + observation 回喂）
//    · 基础闸门：步数上限 / token 预算 / 墙钟 / 连续错误 / 用户中断
//    · 消息合法性守卫（G8）：孤儿 tool / 配对窗口被打断 / 只回一半
//    · 观察者（LoopHooks）把事件抛给 UI，核心层不知道 UI 长什么样
//
//  ★ 循环不直接碰文件系统 —— 所有副作用都经过 ToolRegistry（权限门在它里面）。
// ---------------------------------------------------------------------------
#include "agent_loop.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <unordered_set>

namespace af {

namespace {

// 供 dispatch 使用的 UI 适配器：把 LoopHooks.confirm / onNotice 接成 IUIPort。
class HookUI : public IUIPort {
public:
    explicit HookUI(const LoopHooks& h) : h_(h) {}
    bool confirm(const std::string& prompt) override {
        return h_.confirm ? h_.confirm(prompt) : false;   // fail-closed
    }
    void notice(const std::string& msg) override {
        if (h_.onNotice) h_.onNotice(msg);
    }
private:
    const LoopHooks& h_;
};

// ---------------------------------------------------------------------------
//  消息合法性守卫（G8）
//
//  协议硬要求：每一条 tool 消息必须紧跟"请求了它那个 id 的 assistant 消息"，
//  且这个 assistant 消息不能与它的 tool 结果块被别的消息隔开；一个 assistant
//  请求了 N 个工具，就必须有 N 条 tool 消息全部回填。
//
//  三类破坏（都是 400 的来源）：
//    ① 孤儿 tool      —— 裁剪时删了 assistant，留下 tool
//    ② 配对窗口被打断 —— 中间插了一条 user（如系统错误回喂）
//    ③ 只回一半       —— 一次请求 3 个工具，只回填 1 条结果
//
//  修复策略：以"块"为单位重建历史 —— 合法块保留，非法块整段丢弃。
//  丢块比造块安全：无法凭空补出工具结果，只能移除不合法引用。
// ---------------------------------------------------------------------------
std::size_t sanitizeHistory(std::vector<Message>& hist, std::string& report) {
    std::vector<Message> out;
    out.reserve(hist.size());
    std::size_t fixes = 0;

    std::size_t i = 0;
    while (i < hist.size()) {
        const Message& m = hist[i];

        // ① 孤儿 tool：没有前置 assistant 请求它
        if (m.role == Role::Tool) {
            ++fixes;
            report += "孤儿 tool 消息被移除；";
            ++i;
            continue;
        }

        // ②③ assistant(tool_calls) 必须紧跟完整的一组 tool 回填
        if (m.role == Role::Assistant && m.hasToolCalls()) {
            std::unordered_set<std::string> want;
            for (const auto& tc : m.toolCalls) want.insert(tc.id);

            std::size_t j = i + 1;
            std::unordered_set<std::string> got;
            while (j < hist.size() && hist[j].role == Role::Tool) {
                got.insert(hist[j].toolCallId);
                ++j;
            }

            if (got == want && !want.empty()) {
                out.push_back(m);
                for (std::size_t k = i + 1; k < j; ++k) out.push_back(hist[k]);
                i = j;
            } else {
                ++fixes;
                report += "assistant 与 tool 结果不配对（只回一半/窗口被打断），整段丢弃；";
                i = j;   // 跳过 assistant 与其后所有 tool 消息
            }
            continue;
        }

        out.push_back(m);
        ++i;
    }

    hist.swap(out);
    return fixes;
}

std::uint64_t hashCall(const ToolCall& c) {
    const std::string key = c.name + '\x1f' + c.args.dump();
    return std::hash<std::string>{}(key);
}

}  // namespace

AgentLoop::AgentLoop(IModelProvider& provider,
                     ToolRegistry& registry,
                     ContextAssembler& assembler,
                     LoopLimits limits)
    : provider_(provider), registry_(registry), assembler_(assembler), limits_(limits) {}

int AgentLoop::spentTokens() const noexcept { return spentTokens_; }
Usage AgentLoop::totalUsage() const noexcept { return total_; }

bool AgentLoop::detectRepeat(const ToolCall& call) {
    const std::uint64_t h = hashCall(call);
    std::size_t same = 0;
    for (auto it = recentCallHashes_.rbegin(); it != recentCallHashes_.rend(); ++it) {
        if (*it == h) ++same; else break;
    }
    recentCallHashes_.push_back(h);
    if (recentCallHashes_.size() > std::size_t(std::max(1, limits_.maxRepeatedCalls) + 8))
        recentCallHashes_.pop_front();
    return same >= std::max(1, limits_.maxRepeatedCalls);
}

void AgentLoop::injectWarning(Turn& turn, const ToolCall& call) {
    // L5 先做"提醒不阻断"：给 UI 一个显式警告，模型在下一轮通常能自行换方法。
    // （L6 会把这段升级成"插入一条系统消息后暂停请求"，见讲义。）
    (void)turn;
    if (false) { (void)call; }
}

LoopOutcome AgentLoop::run(Turn& turn, const LoopHooks& hooks) {
    const Clock::time_point t0 = Clock::now();

    // ---- 把本次用户输入放进历史（只放一次）----
    bool inputInHistory = false;
    for (const auto& m : turn.history) {
        if (m.role == Role::User && m.content == turn.input) { inputInHistory = true; break; }
    }
    if (!inputInHistory) {
        Message u;
        u.role = Role::User;
        u.content = turn.input;
        turn.history.push_back(std::move(u));
    }

    HookUI ui(hooks);
    const LoopLimits& lim = limits_;

    for (int step = 1; step <= lim.maxSteps; ++step) {
        // ---- 闸门 6：用户中断 ----
        if (lim.stop && lim.stop->stop_requested()) return LoopOutcome::UserAborted;
        // ---- 闸门 3：墙钟 ----
        const auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - t0);
        if (elapsed > lim.maxWallClock) return LoopOutcome::WallClockExceeded;
        // ---- 闸门 2：成本 ----
        if (spentTokens_ >= lim.maxTokensSpent) return LoopOutcome::BudgetExceeded;

        // ---- G8：组装之前先保证历史合法（避免把 400 的历史发出去）----
        {
            std::string report;
            const std::size_t fixed = sanitizeHistory(turn.history, report);
            if (fixed && hooks.onNotice)
                hooks.onNotice("消息守卫修复了历史：" + report);
        }

        // ---- 组装请求（上下文分层 + 预算 + 降级）----
        ChatRequest req = assembler_.assemble(turn);

        // ---- 调模型（流式）----
        ChatResponse resp;
        try {
            resp = provider_.stream(req, [&hooks, &lim](const ChatChunk& c) {
                // 取消传播：用户中断时立刻中止传输，不等模型把整段吐完
                if (lim.stop && lim.stop->stop_requested()) return false;
                if (hooks.onChunk) hooks.onChunk(c);
                return true;
            });
        } catch (const std::exception& e) {
            if (hooks.onError) hooks.onError(std::string("模型请求失败: ") + e.what());
            return LoopOutcome::Error;
        }

        // 流式过程中被取消：立即返回，不再进入工具执行
        if (lim.stop && lim.stop->stop_requested()) return LoopOutcome::UserAborted;

        // ---- 记账 ----
        total_.inputTokens      += resp.usage.inputTokens;
        total_.outputTokens     += resp.usage.outputTokens;
        total_.cacheReadTokens  += resp.usage.cacheReadTokens;
        total_.cacheWriteTokens += resp.usage.cacheWriteTokens;
        spentTokens_ += resp.usage.total();
        turn.lastUsage    = resp.usage;
        turn.spentTokens  = spentTokens_;

        // ---- 把完整 assistant 消息写回历史（含 tool_calls，不能只存 content）----
        turn.history.push_back(resp.message);

        // ---- 模型没有要工具 → 收敛，任务完成 ----
        if (resp.message.toolCalls.empty()) {
            turn.finalText = resp.message.content;
            if (resp.finishReason == FinishReason::Length && hooks.onError)
                hooks.onError("回答被截断（finish_reason=length），可能不完整");
            return LoopOutcome::Completed;
        }

        // ---- 模型要工具 → 执行并把结果作为 observation 回喂 ----
        bool roundFailed = false;
        for (const ToolCall& call : resp.message.toolCalls) {
            if (hooks.onToolStart) hooks.onToolStart(call);

            if (detectRepeat(call)) {
                injectWarning(turn, call);
                if (hooks.onNotice)
                    hooks.onNotice("检测到重复调用 " + call.name +
                                   "（同一参数），请换一种方式推进");
            }

            // dispatch 永远返回 ToolResult，从不抛异常（失败也是 observation）
            ToolResult tr = registry_.dispatch(call, ui);
            if (hooks.onToolDone) hooks.onToolDone(tr);
            turn.history.push_back(tr.toMessage());

            if (tr.isError) { ++consecutiveErrors_; roundFailed = roundFailed || true; }
            else           { consecutiveErrors_ = 0; }
        }
        (void)roundFailed;

        // ---- 闸门 4：连续错误 ----
        if (consecutiveErrors_ > lim.maxConsecutiveErrors) return LoopOutcome::TooManyErrors;
    }

    return LoopOutcome::StepLimitReached;
}

}  // namespace af
