// ---------------------------------------------------------------------------
//  subagent.hpp —— 子代理：隔离上下文的第一手段（模块 ⑦ 的协作者）
//
//  ★★ 核心价值只有一条（也是唯一充分的理由）：
//     子代理获得自己的**全新上下文**，完全与主对话隔离。
//     它的工作不会让主上下文膨胀。完成后只返回摘要。
//
//     **不是为了"多个 AI 一起想"，而是为了不让探索过程的垃圾留在主上下文里。**
//     典型场景："这个仓库里认证怎么处理？" → 子代理读 20 个文件，回传 500 字。
//
//  ★ 为什么独立成模块（判据与 L7 的压缩同一条）：
//    子代理有自己的状态（SubagentSpec / Result）、自己的策略（工具子集、
//    步数上限、可选模型）、自己的测试面 → 长在 AgentLoop 里会让那个类
//    同时承担"循环"和"生成循环"两件事。
//
//  ★ 它是 L8「选 vs 压」那条判据的第三次出现：
//     压缩 = 进来之后变小；地图 = 进来之前别进来；
//     子代理 = **根本不让过程进来**（在另一个上下文里做完，只回传结论）。
//
//  ★ types.hpp 本讲**零改动**（CR-3，连续第四讲）：
//    SubagentSpec / SubagentResult 只有子代理用 → 放这里。
//    与 L9 的 UiEvent（放 ui/tui.hpp）、L10 的 CheckpointEvent（放 session_store.hpp）
//    是同一个判断：**谁用得多就把类型放哪儿，不要什么都往契约层塞。**
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent_loop.hpp"
#include "tool_registry.hpp"
#include "types.hpp"

namespace af {

struct SubagentSpec {
    std::string goal;                           // ★ 目标要具体：越具体摘要越有用
    std::vector<std::string> allowedTools;      // 额外白名单（在只读视图之上再收窄）
    int maxSteps  = 30;
    int maxTokens = 100'000;
    std::optional<std::string> modelOverride;   // 探索型任务可换便宜模型（本讲留接口）
    bool readOnly = true;
};

struct SubagentResult {
    std::string summary;        // ★ 只有这个进主上下文
    Usage usage;                // ★ 不进上下文，但**钱要记账**（讲义 §3.4）
    std::vector<std::string> filesRead;
    bool truncated = false;     // 子代理自己的上下文也爆了
    bool failed    = false;
    std::string error;
};

// ===========================================================================
//  子代理运行器
//
//  ★ 依赖全是**引用**（P-16 依赖注入）：它不持有 provider / registry，
//    因此"换一个 provider"或"换一套工具"都不需要重建它。
// ===========================================================================
class SubagentRunner {
public:
    SubagentRunner(IModelProvider& provider, ToolRegistry& registry)
        : provider_(provider), registry_(registry) {}

    SubagentResult run(const SubagentSpec& spec);

    // ★ 最近一次运行的统计（给 UI / 验证用：可观测性不是事后加的）
    int lastSteps() const noexcept { return lastSteps_; }

private:
    IModelProvider& provider_;
    ToolRegistry&   registry_;
    int lastSteps_ = 0;
};

// ===========================================================================
//  摘要契约 —— 绝对路径是单独一条
//
//  丢路径是"摘要类操作"最常见的失败模式：
//    模型知道要改什么，却不知道改哪个文件，只好重新探索一遍，
//    **把省下的 token 原封不动地还回去**。
//
//  ★ 同一条规矩管三个地方：L7 压缩摘要 / L10 交接摘要 / L11 子代理摘要。
//    本讲的自检 I-70 会断言这段文本里含"绝对路径" ——
//    **契约写在那儿没人看，写成断言才有人守。**
// ===========================================================================
constexpr std::string_view kSubagentSummaryPrompt = R"(
你是完成一个子任务的编码助手。完成后输出一份结构化摘要，必须包含：

1. 结论（直接回答目标问题）
2. 关键决策与依据
3. 涉及文件的**绝对路径**
4. 发现但尚未处理的事项
5. 主任务继续时需要知道的关键数据

要求：不超过 800 字。摘要会被注入主对话，不要写过程流水账。
)";

// ---------------------------------------------------------------------------
//  暴露给主模型作为一个**普通工具**：Task
//
//  ★ 它就叫工具，不是什么"Agent 间通信协议"。
//    对主模型来说，派子代理和读一个文件没有区别 ——
//    **这个设计让"要不要派子代理"变成模型自己的判断**，而不是我们写死的规则。
// ---------------------------------------------------------------------------
ToolDef makeTaskToolDef();

}  // namespace af
