// ---------------------------------------------------------------------------
//  L2 · context_budget.hpp —— 上下文分层组装与 token 预算（ADR-5）
//
//  两条互相拉扯的排序原则：
//    · 稳定优先：越稳定的内容越靠前 → 利于前缀缓存命中（省钱省延迟）
//    · 相关优先：越相关的内容越靠后 → 利于模型注意力
//  解法：L0/L1/L2 严格固定在前，L3/L4 放中间，L6 永远最后。
//
//  ★ L2 只有声明；组装逻辑在 L5 落地；**预算与降级链在 L7 落地**；
//    真正的语法抽取（把"看起来像声明"换成 tree-sitter）在 L8。
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tool_registry.hpp"
#include "types.hpp"

namespace af {

struct RetrievedChunk {
    std::string path;
    int         startLine = 0, endLine = 0;
    std::string content;
    double      score = 0.0;
};

// 降级链：全文 → 签名 → 符号名+路径 → 仅路径 → 丢弃
//
// ★ 为什么降级必须是"链"而不是"一刀截断"：
//   直接切掉会产生"半个函数"这种**有害**内容 —— 模型会基于残缺代码做出错误判断。
//   按"签名 → 符号名 → 路径"降级，至少保留语义线索（"这里有个叫 foo 的函数"）。
enum class Degrade { Full = 0, Signatures = 1, SymbolNames = 2, PathOnly = 3, Drop = 4 };

class ContextAssembler {
public:
    ContextAssembler(BudgetConfig cfg, std::shared_ptr<IUIPort> ui);

    const BudgetConfig& config() const noexcept;
    IUIPort* uiPort() const noexcept;    // 可空：无头场景下没有 UI

    // 组装一次请求。顺序固定，保证缓存前缀稳定。
    ChatRequest assemble(const Turn& turn);

    // 以下三个 setter 的内容都是"会话内基本不变"的 → 属于缓存前缀部分
    void setSystem(std::string s);
    void setProjectKnowledge(std::string s);   // CLAUDE.md / rules（L10 加载）
    void setRepoMap(std::string map);          // 仓库地图（L8 生成）

    // ★ 工具表必须注册进组装器，否则请求里 tools 为空 ——
    //   模型永远看不到工具，也就永远不会产出 tool_call。
    //   "模型不听话"最常见的假象，根因在这里。
    void setTools(std::vector<ToolDef> tools);

    // 统计各层占用，供 /context 可视化（L12）
    struct LayerUsage {
        int l0 = 0, l1 = 0, l2 = 0, l3 = 0, l4 = 0, l5 = 0, l6 = 0;
        int total = 0;
    };
    LayerUsage measure(const Turn& turn) const;

    // =======================================================================
    //  ★ L7 新增：把"单条内容的降级链"公开出来
    //
    //  为什么从 private 提到 public：压缩器（compaction.cpp）也要用它。
    //
    //  划分必须清楚，否则会写出两条互相漂移的实现：
    //      · 这三个函数管的是 **单条内容** 的降级 —— 局部、无状态、纯粹
    //      · Compactor 管的是 **整段历史** 的压缩策略 —— 全局、有状态、
    //        可能花一次模型调用
    //  前者是机制，后者是策略。"按预算把一条内容降级"这件事，
    //  在"组装一次请求"和"压缩一段历史"两个场景下**必须是同一份代码** ——
    //  否则迟早出现"组装时降级成符号名、压缩时降级成路径"这种不一致。
    // =======================================================================
    static int estimateTokens(std::string_view s) noexcept;

    // 按预算裁剪一段内容，必要时降级；applied 回报实际用到了哪一级
    static std::string fitTo(std::string content, int tokenBudget, Degrade& applied);

    // 以下两个需要语法分析（L8 接 tree-sitter 后才有意义，现在是词法占位）
    static std::string extractSignatures(std::string_view src);
    static std::string extractSymbolNames(std::string_view src);

private:
    BudgetConfig cfg_;
    std::shared_ptr<IUIPort> ui_;
    std::string system_, project_, repoMap_;
    std::vector<ToolDef> tools_;
};

}  // namespace af
