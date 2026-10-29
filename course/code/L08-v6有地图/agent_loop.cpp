// ---------------------------------------------------------------------------
//  L6 · agent_loop.cpp —— 循环内核（模块⑨，机制 W2）—— 六道闸门
//  L7 · 增补 —— 在组装之前加一步"上下文压缩"（**只加不改**）
//
//  L5 的版本**故意一道闸门都没有**，只有一个 `maxSteps` 粗上限当保险丝。
//  那一讲的验证用例 C5 就真的让它转到上限 —— 让"停不下来"这件事被**看见**：
//  6 步、6 次请求、in 576 token，什么都没产出。
//
//  L6 把六道闸门全部补上。请注意三件事，它们比闸门本身更重要：
//
//    ① 六道闸门**全部落在这个 .cpp 里** ——
//       不改 `agent_loop.hpp`、不改 `types.hpp`、不新增任何模块。
//       这就是"好架构允许你加能力而不加模块"的实证。
//
//    ② 头文件里那两个"已声明未定义"的函数（detectRepeat / injectWarning）
//       在 L6 终于给出定义 —— 而且**只有到了需要它们的时候**才定义。
//
//    ③ 每道闸门都对应一种**真实见过的**失败模式，
//       不是为了"完整性"凑数（讲义 §1 逐个讲）。
//
//  ★ L7 加进来的东西只有两处，都是"插入"而不是"改写"：
//        · `compactIfNeeded()`：闸门之后、组装之前的一次调用
//        · 它自己把摘要的用量记进 `spentTokens_`（**闸门 2 的账要算全**）
//    没有一行 L6 的逻辑被修改。这也是一种"零改动"——
//    **只加不改的可验证形式，是"改动的行数 == 新增的行数"。**
//
//  六道闸门与它们的失败模式（L6 定稿，本讲未动）：
//      1 maxSteps           模型在两个状态间来回横跳（A→B→A→B…）
//      2 maxTokensSpent     任务不可能完成，但模型永远"再试一次"
//      3 maxWallClock       一条卡住的命令把整个 turn 拖到天亮
//      4 连续错误           地址/密钥配错（401/500），无限重试烧钱
//      5 重复调用           模型看不到自己的历史 → 原样重发同一个调用
//      6 用户中断           Ctrl-C 之后只能杀进程
//
//  ★ 为什么是"六道"而不是"一道超时"：
//    它们的**触发条件互不重叠**。超时只能挡住"慢"，
//    挡不住"很快但绕圈"（闸门 5）、"很快但一直失败"（闸门 4）、
//    "很快且每次都不一样但没意义"（闸门 1）。少一道就有一类死法漏网。
// ---------------------------------------------------------------------------
#include "agent_loop.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace af {

namespace {

// 把一次工具调用压成一个可比较的指纹。
// 用 0x1f（单元分隔符）当分隔，避免 name+args 拼接时产生歧义
// （"ab"+"c" 与 "a"+"bc" 会撞成同一个 key）。
std::uint64_t hashCall(const ToolCall& c) {
    const std::string key = c.name + '\x1f' + c.args.dump();
    return static_cast<std::uint64_t>(std::hash<std::string>{}(key));
}

// 幂等：同样的 callId、同样的文案，构造"重复调用警告"。
// 之所以要抽成函数，是因为它会被**两处**用到（hooks 通知 + 回喂历史），
// 两边文案必须一致 —— 否则学员会看到"通知说 A、模型看到 B"。
std::string repeatWarningText(const ToolCall& c) {
    return std::string("★ 重复调用被拦下（闸门 5）：你已经用**完全相同的参数**"
                       "调用过 `") + c.name + "` 若干次。\n"
                       "原样重试不会改变结果。请换一种做法：换参数、换工具，"
                       "或直接说明你卡在哪一步、需要什么信息。";
}

}  // namespace

AgentLoop::AgentLoop(IModelProvider& provider, ToolRegistry& registry,
                     ContextAssembler& assembler, LoopLimits limits)
    : provider_(provider), registry_(registry), assembler_(assembler),
      limits_(std::move(limits)) {}

int AgentLoop::spentTokens() const noexcept { return spentTokens_; }

Usage AgentLoop::totalUsage() const noexcept { return total_; }

// ===========================================================================
//  ★ L7：压缩器接线（只加不改）
// ===========================================================================
void AgentLoop::setCompactor(std::shared_ptr<Compactor> c) {
    compactor_ = std::move(c);
}

const std::vector<CompactionEvent>& AgentLoop::compactionEvents() const noexcept {
    return compactionEvents_;
}

// ---------------------------------------------------------------------------
//  在组装之前把上下文压到预算内
//
//  ★ 这里只有三件事，但每一件都不能少：
//    ① 估算当前历史占用（**与组装器同一把尺子**：Compactor 与
//       ContextAssembler 共用 estimateTokens，否则两边会打架）
//    ② 要压就压，把逐级事件收集起来报给 UI
//    ③ ★★ 把摘要那一次模型调用的用量**计入 spentTokens_ / total_**
//
//  第 ③ 条是本函数存在的主要理由。如果把压缩塞进组装器里，
//  摘要的账单就会漏掉 —— 而"漏掉的账单"比"多花的钱"危险得多：
//  用户会看到一个永远触发不了的成本上限，然后在下个月收到账单。
// ---------------------------------------------------------------------------
void AgentLoop::compactIfNeeded(Turn& turn, const LoopHooks& hooks) {
    if (!compactor_) return;

    const int histTokens = Compactor::estimateHistoryTokens(turn.history);
    if (!compactor_->shouldCompact(histTokens)) return;

    // ★ 第五个参数是 `goalHint = turn.input`：本次任务的需求。
    //   历史里没有 user 消息（用户输入由组装器追加在最后），所以摘要器
    //   拿不到"原始需求" —— 只能由这里传进去。见 compaction.hpp 的说明。
    CompactionOutcome out = compactor_->compact(turn.history, histTokens,
                                                std::nullopt, std::nullopt, turn.input);

    // ---- 记账：摘要也是钱 ------------------------------------------------
    spentTokens_ += out.summaryUsage.total();
    total_.inputTokens      += out.summaryUsage.inputTokens;
    total_.outputTokens     += out.summaryUsage.outputTokens;
    total_.cacheReadTokens  += out.summaryUsage.cacheReadTokens;
    total_.cacheWriteTokens += out.summaryUsage.cacheWriteTokens;

    // ---- 观测：把每一次压缩报出去 ----------------------------------------
    for (const auto& e : out.events) {
        compactionEvents_.push_back(e);
        if (hooks.onNotice)
            hooks.onNotice("上下文压缩 " + e.detail + "（" +
                           std::to_string(e.tokensBefore) + " → " +
                           std::to_string(e.tokensAfter) + " token）");
    }
    if (out.triggered && out.events.empty() && hooks.onNotice)
        hooks.onNotice("上下文超预算，但没有可压缩的内容（可能只有锚点 + 最近窗口）");

    if (out.thrashing && hooks.onNotice)
        hooks.onNotice("★ 压缩后上下文仍然立刻填满（thrashing）——"
                       "通常是单个工具结果或单个文件过大，"
                       "建议收窄任务范围或提高 L1 降级的力度");
}

// ===========================================================================
//  闸门 5 的两半：检测 + 注射
// ===========================================================================
//
//  ★ 设计要点：检测到重复**不直接退出**，而是"给一次改过自新的机会"。
//    原因很实际：模型重复调用常常是因为**上一轮工具结果它没读懂**，
//    或者参数缺了一个字段。直接判死会让本来能救的任务被判死。
//    先注入一次明确的警告，它自己多半就换路子了 ——
//    这比"报错退出"多花一次请求，但救回来的任务远多于浪费的这一次。
//
//  ★ 检测口径：不是"严格连续 N 次相同"，而是"**最近 64 次里出现过 N 次**"。
//    为什么放宽：真实死循环长这样 —— Read(a) → Edit(a) → Read(a) → Edit(a)…
//    每一步都和上一步不同，严格连续判定会**完全失效**。
//    而窗口计数能抓住这种交替循环（它其实是最常见的形态）。
// ---------------------------------------------------------------------------
bool AgentLoop::detectRepeat(const ToolCall& call) {
    const std::uint64_t h = hashCall(call);

    // 数一数窗口里已经出现过几次（不含本次）
    std::size_t seen = 0;
    for (const auto prev : recentCallHashes_)
        if (prev == h) ++seen;

    recentCallHashes_.push_back(h);
    // 窗口上限 64：足够覆盖"读改读改"这种长交替，又不至于把很久以前的
    // 合法重复（例如隔了很多步又读一次同一个文件）算成死循环。
    while (recentCallHashes_.size() > 64) recentCallHashes_.pop_front();

    if (limits_.maxRepeatedCalls <= 0) return false;      // 0 = 关闭该闸门
    return static_cast<int>(seen) + 1 >= limits_.maxRepeatedCalls;
}

void AgentLoop::injectWarning(Turn& turn, const ToolCall& call) {
    Message m;
    m.role       = Role::Tool;
    m.toolCallId = call.id;
    m.content    = repeatWarningText(call);

    // ★ 警告必须以 **tool 角色**回喂，绝不能插一条 user 消息。
    //   原因在 G8（消息合法性）：assistant(tool_calls) 之后，
    //   在它的 tool 结果到达之前插入任何非 tool 消息，都会**关闭配对窗口**，
    //   于是那条姗姗来迟的 tool 结果变成孤儿 → 下一圈请求直接 400。
    //   这是一个"看起来更自然、实际上是错的"的写法，讲义 §3 有完整对照。
    turn.history.push_back(std::move(m));
}

// ===========================================================================
//  跑一个 turn
// ===========================================================================
LoopOutcome AgentLoop::run(Turn& turn, const LoopHooks& hooks) {
    const auto tStart = Clock::now();
    int repeatStrikes = 0;           // 闸门 5 已经警告过几次

    for (int step = 0; step < limits_.maxSteps; ++step) {

        // =====================================================================
        //  闸门前置检查（在**每一次请求之前**，不是之后）
        //
        //  ★ 位置很关键：放在请求之前 → 最坏情况是"多花一次请求"；
        //    放在请求之后 → 那些闸门只能"事后发现"，超支已经发生。
        // =====================================================================

        // ---- 闸门 6（前置）：用户已要求中断 -------------------------------
        if (limits_.stop && limits_.stop->stop_requested()) {
            if (hooks.onNotice)
                hooks.onNotice("用户中断：停止（还没开始第 " +
                               std::to_string(step + 1) + " 步）");
            return LoopOutcome::UserAborted;
        }

        // ---- 闸门 3：墙钟 -------------------------------------------------
        if (limits_.maxWallClock.count() > 0 &&
            std::chrono::duration_cast<Ms>(Clock::now() - tStart) >
                limits_.maxWallClock) {
            if (hooks.onNotice)
                hooks.onNotice("墙钟超时：已运行超过 " +
                               std::to_string(limits_.maxWallClock.count()) +
                               " ms，停止（闸门 3）");
            return LoopOutcome::WallClockExceeded;
        }

        // ---- 闸门 2：成本 -------------------------------------------------
        if (limits_.maxTokensSpent > 0 && spentTokens_ >= limits_.maxTokensSpent) {
            if (hooks.onNotice)
                hooks.onNotice("token 预算耗尽（" + std::to_string(spentTokens_) +
                               "/" + std::to_string(limits_.maxTokensSpent) +
                               "），停止（闸门 2）");
            return LoopOutcome::BudgetExceeded;
        }

        // ---- ★ L7：上下文压缩（在**组装之前**，不是在组装里面）-------------
        //  ★ 位置有两个讲究：
        //    ① 在闸门之后 —— 已经该停的时候不要再花钱去做摘要
        //    ② 在组装之前 —— 压缩改的是"历史本身"，组装只负责把历史摆好。
        //       把压缩塞进 assemble() 会有一个隐蔽的后果：摘要那次模型调用
        //       发生在**组装器内部**，循环看不见它 → 账单少算一笔 →
        //       **闸门 2 会撒谎**。所以压缩必须由拥有 provider_ 的循环来驱动。
        compactIfNeeded(turn, hooks);

        // ---- A. 组装上下文 ------------------------------------------------
        //  ★ 必须在**环内**，每一步都重新组装（L5 的教训，这里不能忘）。
        ChatRequest req = assembler_.assemble(turn);
        req.maxTokens = std::min(req.maxTokens, assembler_.config().maxOutputTokens);
        // 取消信号也随请求下发（支持它的 provider 会据此提前收尾）
        if (limits_.stop) req.stop = limits_.stop;

        // ---- B. 调模型 ----------------------------------------------------
        ChatResponse resp;
        try {
            resp = provider_.stream(req, [&](const ChatChunk& c) -> bool {
                if (hooks.onChunk) hooks.onChunk(c);

                // ---- 闸门 6 的第一级：把取消**一路传到传输层** --------
                //  返回 false 之后，调用链是：
                //    StreamCtx::emit → feed → postStream 回调
                //      → http_winhttp.cpp 的 onData 返回 false
                //      → doRequest 里 break，连接立刻关掉
                //  没有这条链，"用户按了 Ctrl-C 还要等模型把整段吐完"。
                if (limits_.stop && limits_.stop->stop_requested()) return false;
                return true;
            });
        } catch (const std::exception& e) {
            // =================================================================
            //  闸门 4：连续错误
            //
            //  ★ "连续"两个字是重点：**成功一次就清零**（见下面）。
            //    不清零的话，一整天里零星失败 5 次也会把任务掐死 ——
            //    那是"总错误数上限"，不是"连续错误上限"，行为完全不同。
            // =================================================================
            ++consecutiveErrors_;
            if (hooks.onError) hooks.onError(e.what());

            if (limits_.maxConsecutiveErrors > 0 &&
                consecutiveErrors_ >= limits_.maxConsecutiveErrors) {
                if (hooks.onNotice)
                    hooks.onNotice("连续 " + std::to_string(consecutiveErrors_) +
                                   " 次请求失败，停止（闸门 4）");
                return LoopOutcome::TooManyErrors;
            }

            if (hooks.onNotice)
                hooks.onNotice("模型请求失败（连续第 " +
                               std::to_string(consecutiveErrors_) +
                               " 次），已回喂给模型，继续下一步");

            // ★ 失败是 observation，不是"这个 turn 结束了"。
            Message m;
            m.role    = Role::User;
            m.content = std::string("[系统] 模型请求失败: ") + e.what() +
                        "。请继续，或换一种方式。";
            turn.history.push_back(std::move(m));
            continue;
        }
        consecutiveErrors_ = 0;    // ★ 成功一次，连续计数归零

        // ---- 闸门 6 的第二级：取消发生在**流中途** ------------------------
        //  此时已经收到部分内容。不丢弃它 —— 半截回答往往比"什么都没有"有用。
        if (limits_.stop && limits_.stop->stop_requested()) {
            turn.finalText = resp.message.content;
            if (hooks.onNotice)
                hooks.onNotice("用户中断：已保留收到的 " +
                               std::to_string(resp.message.content.size()) + " 字符");
            return LoopOutcome::UserAborted;
        }

        // ---- C. 记账 + 存历史 ---------------------------------------------
        turn.history.push_back(resp.message);

        spentTokens_ += resp.usage.total();
        total_.inputTokens      += resp.usage.inputTokens;
        total_.outputTokens     += resp.usage.outputTokens;
        total_.cacheReadTokens  += resp.usage.cacheReadTokens;
        total_.cacheWriteTokens += resp.usage.cacheWriteTokens;

        // ---- D. 结束判定 ---------------------------------------------------
        if (!resp.message.hasToolCalls()) {
            turn.finalText = resp.message.content;
            return LoopOutcome::Completed;
        }

        // ---- E. 执行工具并回喂（环的回边）----------------------------------
        static NoUI kNoUI;
        IUIPort& ui = assembler_.uiPort() ? *assembler_.uiPort()
                                          : static_cast<IUIPort&>(kNoUI);

        int done = 0;
        for (const auto& tc : resp.message.toolCalls) {

            // ---- 闸门 5：重复调用检测 --------------------------------------
            if (detectRepeat(tc)) {
                ++repeatStrikes;

                const ToolResult warn = ToolResult::error(tc.id, repeatWarningText(tc));
                if (hooks.onToolStart) hooks.onToolStart(tc);
                if (hooks.onToolDone)  hooks.onToolDone(warn);
                injectWarning(turn, tc);          // 回喂（保持 G8 合法性）
                ++done;

                if (hooks.onNotice)
                    hooks.onNotice("检测到重复调用（第 " +
                                   std::to_string(repeatStrikes) +
                                   " 次），已插入警告而不是直接退出（闸门 5）");

                // ★ 已经给过机会了还不改 → 判定死循环。
                //   第 1 次：警告（软）。第 2 次：停（硬）。
                //   这个"一软一硬"是刻意的：见上面 detectRepeat 的设计说明。
                if (repeatStrikes > 1) {
                    if (hooks.onNotice)
                        hooks.onNotice("警告之后仍在重复调用，判定为死循环，停止（闸门 5）");
                    return LoopOutcome::StepLimitReached;
                }
                continue;
            }

            if (hooks.onToolStart) hooks.onToolStart(tc);
            ToolResult r = registry_.dispatch(tc, ui);
            if (hooks.onToolDone) hooks.onToolDone(r);

            // ★ 必须与 assistant 的 tool_call 成对（每个 tc 恰好一条结果）
            turn.history.push_back(r.toMessage());
            ++done;
        }

        if (hooks.onNotice) {
            hooks.onNotice("第 " + std::to_string(step + 1) + " 步：执行了 " +
                           std::to_string(done) + " 个工具");
        }
    }

    // ---- 闸门 1：步数上限 ------------------------------------------------
    //  能走到这里，说明前五道闸门都没命中：它既不重复、也不报错、
    //  也没超时超预算 —— 只是在**不停地产出不同的调用**。
    //  这正是闸门 1 存在的理由：它是最后一道，专门兜住"其他的都兜不住"。
    if (hooks.onNotice)
        hooks.onNotice("达到步数上限 " + std::to_string(limits_.maxSteps) +
                       "，停止（闸门 1）");
    return LoopOutcome::StepLimitReached;
}

}  // namespace af
