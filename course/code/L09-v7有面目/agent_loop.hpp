// ---------------------------------------------------------------------------
//  L2 · agent_loop.hpp —— Agent 循环内核（模块 ⑨）
//
//  第一性循环（全课的骨架）：
//      while (模型返回工具调用) { 执行 → 捕获 → 追加到上下文 → 再调模型 }
//
//  六道闸门，一道都不能少（L6 逐个兑现）：
//      1 maxSteps   2 maxTokensSpent   3 maxWallClock
//      4 连续错误   5 重复调用（死循环） 6 用户中断
//
//  ★ 本讲只有声明。实现在 L5 —— 而 L6 会直接在这个文件里加闸门，
//    不新增任何文件。这是刻意的教学安排：
//    **好架构允许你"加能力而不加模块"。**
//
//  ★ L7 反过来做：新增了 `compaction.{hpp,cpp}`，并在这里加**两个方法**。
//    判据（讲义 §2.1）不是"改几行"，而是**这次加的东西是否引入了一个
//    需要独立演进的概念**：
//      · 闸门 = 循环的**属性**（边界条件）→ 长在循环里
//      · 压缩 = 循环的**协作者**（有自己的状态/策略/测试面，
//               而且 L10 的 --resume 也要用它）→ 独立成模块
//    所以 L7 对头文件的改动是**只加不改**（CR-3）：
//      + setCompactor()        —— 依赖注入（P-16），而不是让循环去 new 一个
//      + compactionEvents()    —— 把它报给 UI（可观测性）
//    构造函数**一个字没动** —— 加"可选依赖"的正确方式是 setter，不是第 5 个参数。
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "compaction.hpp"
#include "context_budget.hpp"
#include "model_provider.hpp"
#include "tool_registry.hpp"
#include "types.hpp"

namespace af {

// Turn 定义在 types.hpp（避免与 ContextAssembler 循环依赖，见那边的注释）

// ---------------------------------------------------------------------------
//  供 UI 订阅的钩子。
//
//  ★ 这是"分层"最直观的体现：核心层完全不知道 UI 长什么样，
//    它只在这一处把事件抛出去。终端 / Web / IDE 各自订阅。
//
//    注意这里**没有**"渲染"相关的字段（光标位置、颜色、窗口大小……）——
//    一旦有了，分层就破了。
// ---------------------------------------------------------------------------
struct LoopHooks {
    std::function<void(const ChatChunk&)>    onChunk;      // 流式增量
    std::function<void(const ToolCall&)>     onToolStart;
    std::function<void(const ToolResult&)>   onToolDone;
    std::function<void(const std::string&)>  onNotice;
    std::function<bool(const std::string&)>  confirm;      // 权限询问（返回是否允许）

    // 模型请求失败。
    // ★ 错误要**同时**做两件事：回喂给模型（进上下文）+ 通知 UI。
    //   只回喂不通知的话，用户看到的是"agent 沉默几秒后突然放弃"，完全无法排查。
    std::function<void(const std::string&)>  onError;
};

class AgentLoop {
public:
    // 依赖都是**引用**：AgentLoop 不持有它们，生命周期由调用方管。
    // 这样 UI 换个实现、registry 换个配置，都不需要重建循环。
    AgentLoop(IModelProvider& provider,
              ToolRegistry&   registry,
              ContextAssembler& assembler,
              LoopLimits limits = {});

    // 跑一个 turn。会修改 turn.history（追加 assistant / tool 消息）。
    LoopOutcome run(Turn& turn, const LoopHooks& hooks = {});

    int   spentTokens() const noexcept;
    Usage totalUsage() const noexcept;

    // ---- ★ L7 新增（只加不改）--------------------------------------------
    //  压缩器是**可选**依赖：不设就等于"上下文永不压缩"（L6 的行为）。
    //  为什么不设成构造函数参数：构造函数已经 4 个参数了，而压缩只有在
    //  "上下文比窗口还大"时才需要 —— 让不需要它的人也必须提供它，
    //  就是在用类型系统惩罚简单用例。
    void setCompactor(std::shared_ptr<Compactor> c);

    //  报给 UI / 测试：这个 turn 里发生过的每次压缩（逐级）。
    const std::vector<CompactionEvent>& compactionEvents() const noexcept;

private:
    // 死循环检测：连续 N 次相同 (tool, args) 就插入一次警告而不是直接退出
    // ★ 是"给一次改过自新的机会"，不是"立刻失败" —— 见 L6
    bool detectRepeat(const ToolCall& call);
    void injectWarning(Turn& turn, const ToolCall& call);

    // ★ L7：在组装之前把上下文压到预算内。
    //   返回本次压缩的用量（要计入闸门 2）。
    void compactIfNeeded(Turn& turn, const LoopHooks& hooks);

    IModelProvider&   provider_;
    ToolRegistry&     registry_;
    ContextAssembler& assembler_;
    LoopLimits        limits_;

    int   spentTokens_ = 0;
    int   consecutiveErrors_ = 0;
    Usage total_;

    std::deque<std::uint64_t> recentCallHashes_;

    // ★ L7 新增
    std::shared_ptr<Compactor>   compactor_;
    std::vector<CompactionEvent> compactionEvents_;
};

}  // namespace af
