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
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

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

private:
    // 死循环检测：连续 N 次相同 (tool, args) 就插入一次警告而不是直接退出
    // ★ 是"给一次改过自新的机会"，不是"立刻失败" —— 见 L6
    bool detectRepeat(const ToolCall& call);
    void injectWarning(Turn& turn, const ToolCall& call);

    IModelProvider&   provider_;
    ToolRegistry&     registry_;
    ContextAssembler& assembler_;
    LoopLimits        limits_;

    int   spentTokens_ = 0;
    int   consecutiveErrors_ = 0;
    Usage total_;

    std::deque<std::uint64_t> recentCallHashes_;
};

}  // namespace af
