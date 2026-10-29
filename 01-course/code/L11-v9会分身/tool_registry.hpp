// ---------------------------------------------------------------------------
//  L2 · tool_registry.hpp —— 端口 ④（UI）+ 工具注册表 + 权限契约
//
//  三条核心原则：
//    1. 模型只产出**意图**，绝不直接触碰文件系统。真正的 I/O 由这里完成。
//    2. 每一步失败都是"回喂 observation"而不是"抛异常" ——
//       这是 Agent 循环能自我修复的前提（对应 FR-8）。
//    3. 权限判定：deny → ask → allow，**deny 永远赢**。
//
//  ★ L4 变更（契约规矩 CR-3：只加不改）：
//      新增 loadPlugin()，并把 Entry 扩成同时支持"内置函数"和"C ABI 插件"。
//      其余接口一个字符都没动。
// ---------------------------------------------------------------------------
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "plugin.hpp"
#include "sandbox.hpp"
#include "types.hpp"

namespace af {

// ============================================================================
//  权限契约
// ============================================================================

// ★ L11：工具名常量。放在这里（而不是 subagent.hpp）是为了**避免循环依赖** ——
//   subagent.hpp 本来就 include tool_registry.hpp，反过来 include 就成环了。
//   ★ 更本质的理由：**"系统里有哪些工具名"是注册表的职责**，不是某个调用方的。
//   readOnlyView() 要按名字排除 Task，而 makeTaskToolDef() 要按名字注册它 ——
//   两处都引用同一个常量，"改了名字忘了改另一处"这类漂移就不可能发生。
constexpr std::string_view kTaskToolName = "Task";

enum class DecisionKind { Allow, Ask, Deny };

struct Decision {
    DecisionKind kind = DecisionKind::Ask;
    std::string  reason;     // 给日志和模型看的判定理由
    std::string  prompt;     // 展示给用户的话（Ask 时用）
};

// 权限规则。有序求值：所有 deny 段规则强制排在最前。
struct PermissionRule {
    DecisionKind kind = DecisionKind::Allow;
    std::string  toolPattern;   // "*" | "Bash" | "Bash(npm test)"
    std::string  reason;

    bool matches(const std::string& toolName, const json& args) const;
};

class IPermissionGate {
public:
    virtual ~IPermissionGate() = default;
    virtual Decision evaluate(const ToolDef& tool, const json& args) const = 0;
};

// ============================================================================
//  端口 ④：UI
//
//  ★ 只有两个方法，但它是"核心不知道 UI 存在"的唯一保证。
//    多了会污染核心（渲染逻辑渗进来），少了就表达不了"需要人批准"这件事。
// ============================================================================
class IUIPort {
public:
    virtual ~IUIPort() = default;
    virtual bool confirm(const std::string& prompt) = 0;   // 阻塞式询问
    virtual void notice(const std::string& msg) = 0;       // 单向通知
};

// 空对象（P-17）：无头 / 子代理场景下没有 UI。
// ★ 安全默认必须是"拒绝"，不是"允许" ——
//   没有 UI 时的正确行为是 fail-closed。
class NoUI final : public IUIPort {
public:
    bool confirm(const std::string&) override;
    void notice(const std::string&) override;
};

// ============================================================================
//  工具注册表
//
//  职责边界：
//    它管  「注册 · Schema 校验 · 权限判定 · 派发 · 输出过滤」
//    它不管「循环」「上下文」「模型调用」
// ============================================================================
class ToolRegistry {
public:
    // 注册内置工具（进程内实现）
    void registerBuiltin(ToolDef def, std::function<ToolResult(const json&)> fn);

    // ★ L4 新增：从动态库加载 C ABI 插件（plugin.hpp 定义 ABI）。
    //   失败返回 false 并填 err —— 不抛异常（CR-2）。
    bool loadPlugin(const std::string& path, std::string& err);

    // 顺序稳定（利于提示缓存前缀命中）。
    // ★ 这个列表最终要交给 ContextAssembler::setTools()，
    //   否则请求里 tools 为空 —— 模型永远看不到工具，只会干聊。
    std::vector<ToolDef> defs() const;

    const ToolDef* find(const std::string& name) const;

    // 派发单个调用。失败一律返回 isError=true 的 ToolResult，**不抛异常**。
    ToolResult dispatch(const ToolCall& call, IUIPort& ui);

    // 一个 assistant 消息里可能有 N 个 tool_call
    std::vector<ToolResult> dispatchAll(const std::vector<ToolCall>& calls, IUIPort& ui);

    void setGate(std::shared_ptr<IPermissionGate> g);
    void setSandbox(std::shared_ptr<ISandbox> s);

    // ── ★ L11 兑现（这个落点从 L4 起就挂在这儿）────────────────────────
    //   返回一个**工具子集视图**：只保留 risk==Read 的工具，且**排除 Task**。
    //
    //   ★★ 为什么要排除 Task（K-40）：
    //     Task 的 risk 也是 Read（它确实不写文件），按属性过滤它必然通过 →
    //     子代理可以再派子代理 → **递归深度没有上限**，而且没人看得懂
    //     一个三层深的调用栈在干什么。
    //     ★ 判据：**"按属性写的过滤器"和"我真正想排除的东西"不是一回事** ——
    //       写完过滤器一定要回头问一句"有没有漏网的"。
    //     ★ 最简单的递归上限，是根本不给它递归的能力。
    //
    //   ★ 为什么返回**副本**而不是一个"视图对象"：
    //     Entry 里的 execute 闭包是 `std::function`，可拷贝；
    //     复制一份 = 子代理拿到的是"能干活的工具"，不是"有说明书没人干活的工具"
    //     （后者正是参考实现里的 bug，见 L11 讲义 §4）。
    ToolRegistry readOnlyView() const;

private:
    struct Entry {
        ToolDef def;
        std::function<ToolResult(const json&)> fn;   // 内置实现
        // ★ L4 新增：插件来源。abi 非空表示这个工具由动态库提供。
        const AFToolPlugin* abi = nullptr;
        // 持有动态库句柄，直到最后一个工具注销 —— 这是 RAII（P-14）。
        // 用 shared_ptr<void> 是为了不在头文件里暴露 windows.h 的 HMODULE。
        std::shared_ptr<void> pluginLib;
    };

    std::vector<Entry> entries_;
    std::shared_ptr<IPermissionGate> gate_;
    std::shared_ptr<ISandbox> sandbox_;

    ToolResult invoke(const Entry& e, const json& args);
};

// ============================================================================
//  默认权限门：deny 段 → ask 段 → allow 段，最后落到 fallback
//
//  ★ 构造时必须把 deny 规则稳定排到最前 ——
//    规则表里的顺序会影响结果，"先写的 allow 赢过后面的 deny"是明确的安全漏洞。
// ============================================================================
class DefaultPermissionGate : public IPermissionGate {
public:
    explicit DefaultPermissionGate(std::vector<PermissionRule> rules,
                                   DecisionKind fallback = DecisionKind::Ask);

    Decision evaluate(const ToolDef& tool, const json& args) const override;

private:
    static std::string describe(const ToolDef& t, const json& args);
    std::vector<PermissionRule> rules_;
    DecisionKind fallback_;
};

// ============================================================================
//  大结果落盘（压缩的 L0 层）
//
//  放在这里而不是 context_budget.hpp：它是**工具派发管线的最后一步**
//  （注册 → 校验 → 权限 → 执行 → **输出过滤**），属于工具系统的职责。
//
//  超过阈值就写文件，上下文里只留约 2KB 预览 + 路径。
//  ★ Read 工具的 persistThresholdChars 必须设为 SIZE_MAX 豁免，
//    否则会形成"读文件 → 落盘 → 再去读那个落盘文件"的循环依赖。
// ============================================================================
ToolResult maybePersistLargeResult(ToolResult r, const ToolDef& def,
                                   const std::string& dumpDir);

}  // namespace af
