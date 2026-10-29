// ---------------------------------------------------------------------------
//  L5 · agent_loop.cpp —— 循环内核（模块⑨，机制 W2）
//
//  这是全课唯一的"环"：
//
//      while (模型返回工具调用) { 执行 → 捕获 → 追加到上下文 → 再调模型 }
//
//  ★ 本讲**故意不加六道闸门**。现在只有一个 `maxSteps` 粗上限当保险丝。
//
//    为什么故意：闸门存在的理由是"它会停不下来"，
//    而"它会停不下来"这件事**必须先被看见**。
//    验证用例 C5 就真的让它转到上限，让你看见代价（N 次请求、N 倍 token）。
//
//    L6 会往**这个文件**里补上六道闸门 —— 不新增任何文件。
//    这是刻意的教学安排：**好架构允许你"加能力而不加模块"。**
//
//  本讲真正新增的设计是**观察者**：
//    循环在 6 个固定点把事件抛给 hooks，自己完全不知道 UI 长什么样。
//    一旦在这里写 std::cout，分层就破了 —— 而且不会立刻报错。
// ---------------------------------------------------------------------------
#include "agent_loop.hpp"

#include <algorithm>
#include <chrono>
#include <string>

namespace af {

AgentLoop::AgentLoop(IModelProvider& provider, ToolRegistry& registry,
                     ContextAssembler& assembler, LoopLimits limits)
    : provider_(provider), registry_(registry), assembler_(assembler),
      limits_(std::move(limits)) {}

int AgentLoop::spentTokens() const noexcept { return spentTokens_; }

Usage AgentLoop::totalUsage() const noexcept { return total_; }

// ===========================================================================
//  跑一个 turn
// ===========================================================================
LoopOutcome AgentLoop::run(Turn& turn, const LoopHooks& hooks) {
    for (int step = 0; step < limits_.maxSteps; ++step) {

        // ---- A. 组装上下文 ------------------------------------------------
        //  ★ 必须在**环内**，每一步都重新组装。
        //    挪到环外会变成"模型每一步看到的都是第一步的上下文"，
        //    于是它永远不知道自己已经改过文件 → 无限重复同一个调用，
        //    而你会以为是"模型太笨"。
        //
        //    顺带：assemble() 里做了 G8 合法性守卫 ——
        //    放在环内意味着"每步都重新过一遍合法性检查"。
        //    这是刻意的：不要缓存一个"合法"的判断，因为历史在变。
        ChatRequest req = assembler_.assemble(turn);
        req.maxTokens = std::min(req.maxTokens, assembler_.config().maxOutputTokens);

        // ---- B. 调模型（唯一的"慢"操作）-----------------------------------
        ChatResponse resp;
        try {
            resp = provider_.stream(req, [&](const ChatChunk& c) -> bool {
                // ★ 观察者的接入点：核心只抛事件，不渲染
                if (hooks.onChunk) hooks.onChunk(c);
                return true;      // 取消还没接 —— L6 的闸门 6 才会把它传下去
            });
        } catch (const std::exception& e) {
            // ★ 失败是 observation，不是"这个 turn 结束了"。
            //   回喂给模型，让它自己决定怎么办（换方法 / 说明卡在哪）。
            if (hooks.onError) hooks.onError(e.what());
            if (hooks.onNotice) hooks.onNotice("模型请求失败，已回喂给模型，继续下一步");

            Message m;
            m.role    = Role::User;
            // 注意第一个操作数必须是 std::string，否则 "..." + e.what() 是指针加法
            m.content = std::string("[系统] 模型请求失败: ") + e.what() +
                        "。请继续，或换一种方式。";
            turn.history.push_back(std::move(m));
            continue;             // ★ L5 没有"连续错误上限"（L6 的闸门 4）
        }

        // ---- C. 记账 + 存历史 ---------------------------------------------
        //  ★ 必须存**整个** resp.message。只存 content 会丢掉 tool_calls，
        //    模型下一轮就不知道自己已经调过了 → 重复调用（L4 的老坑，这里更致命）。
        turn.history.push_back(resp.message);

        spentTokens_ += resp.usage.total();
        total_.inputTokens      += resp.usage.inputTokens;
        total_.outputTokens     += resp.usage.outputTokens;
        total_.cacheReadTokens  += resp.usage.cacheReadTokens;
        total_.cacheWriteTokens += resp.usage.cacheWriteTokens;

        // ---- D. 结束判定：模型说"我不要工具了" -----------------------------
        if (!resp.message.hasToolCalls()) {
            turn.finalText = resp.message.content;
            return LoopOutcome::Completed;
        }

        // ---- E. 执行工具并回喂（这就是"环的回边"）--------------------------
        //  无 UI 时一律走 NoUI（confirm 恒返回 false）—— fail-closed。
        //  子代理 / CI 场景下"没有界面"绝不能被解释成"所以不用批准"。
        static NoUI kNoUI;
        IUIPort& ui = assembler_.uiPort() ? *assembler_.uiPort()
                                          : static_cast<IUIPort&>(kNoUI);

        int done = 0;
        for (const auto& tc : resp.message.toolCalls) {
            if (hooks.onToolStart) hooks.onToolStart(tc);

            // dispatch 永远返回 ToolResult，从不抛异常 —— 失败也是 observation
            ToolResult r = registry_.dispatch(tc, ui);

            if (hooks.onToolDone) hooks.onToolDone(r);

            // ★ 必须与 assistant 的 tool_call 成对。
            //   少一条，下一圈的请求就是非法历史（G8 的三类之一）。
            turn.history.push_back(r.toMessage());
            ++done;
        }

        // 进度通知：让 UI 知道"又转了一圈"。这是 L5 里"环可观察"的最小实现。
        if (hooks.onNotice) {
            hooks.onNotice("第 " + std::to_string(step + 1) + " 步：执行了 " +
                           std::to_string(done) + " 个工具");
        }
    }

    // 转满上限仍未收敛。L5 只能如实报告 —— 没有任何闸门能告诉我们"为什么"。
    // (L6 会区分：超预算 / 死循环 / 连续错误 / 用户中断)
    return LoopOutcome::StepLimitReached;
}

// ===========================================================================
//  ★ 以下两个函数在 L2 的 agent_loop.hpp 里**已声明**，但本讲不定义。
//
//    它们是死循环检测（L6 的闸门 5）。这里刻意留空 ——
//    "声明了但没定义"只要**没有地方调用**就不会出错
//    （对比 L4 的 LNK2019：那次是被调用了）。
//
//    L6 会在这里填上 hashCall + detectRepeat + injectWarning，
//    顺便解释"为什么要给模型一次改过自新的机会，而不是立刻失败"。
// ===========================================================================

}  // namespace af
