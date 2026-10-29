// ---------------------------------------------------------------------------
//  subagent.cpp —— 伪码 P10 的实现（环里套环）
//
//  ★ 本文件是从 `09-核心代码/subagent.cpp` 的骨架兑现来的。
//    骨架里有**两处硬伤**，本讲补上；两处都属于
//    "编译得过、跑起来静默不对"那一类（讲义 §4）：
//
//     ① 只复制了 ToolDef，execute 闭包没过去
//        → 子代理的工具"有说明书没人干活"，调 Read 直接失败，
//          而失败原因看起来像"文件不存在"。
//     ② 没调 assembler.setTools()
//        → 请求里 tools=[] → 子代理永远不产出 tool_call → 只会干聊一句就结束，
//          回传一段空摘要。★ 这是 L8 踩过的同一个坑。
// ---------------------------------------------------------------------------
#include "subagent.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace af {

using json = nlohmann::json;

// ===========================================================================
//  Task 工具定义
// ===========================================================================
ToolDef makeTaskToolDef() {
    ToolDef d;
    d.name = std::string(kTaskToolName);
    d.description =
        "派一个子代理去完成一个探索/调研型子任务（例如'这个仓库里认证是怎么处理的'）。\n"
        "子代理在**独立的上下文**里工作，只把摘要回传给你 —— 这样大量读文件的中间过程"
        "不会占用你的上下文。\n"
        "适合：大范围搜索、代码调研、需要先读很多文件才能回答的问题。\n"
        "不适合：需要精确改代码的活（那种你自己做）。";
    d.inputSchema = json::object({
        {"type", "object"},
        {"properties", {
            {"goal", {{"type", "string"}, {"description", "具体目标，越具体摘要越有用"}}},
            {"read_only", {{"type", "boolean"}, {"description", "子代理是否只读，默认 true"}}},
        }},
        {"required", json::array({"goal"})},
    });
    // ★ risk = Read：Task 确实不写文件。
    //   但正因如此，它能通过 readOnlyView() 的 risk 过滤 ——
    //   所以 readOnlyView() 里必须**按名字**再排除它一次（K-40）。
    d.risk = RiskLevel::Read;
    d.timeoutMs = 300'000;
    return d;
}

// ===========================================================================
//  run() —— 伪码 P10
// ===========================================================================
SubagentResult SubagentRunner::run(const SubagentSpec& spec) {
    SubagentResult out;
    lastSteps_ = 0;

    // ── 隔离边界 ②：工具 ──────────────────────────────────────────────
    //   ★ 用 readOnlyView()：闭包一起复制过来，工具是真的能执行的。
    ToolRegistry subRegistry = registry_.readOnlyView();

    // 在只读视图之上再按 allowedTools 收窄（spec 给了就用，没给就全留）
    if (!spec.allowedTools.empty()) {
        ToolRegistry narrowed;
        for (const auto& def : subRegistry.defs()) {
            if (std::find(spec.allowedTools.begin(), spec.allowedTools.end(), def.name) ==
                spec.allowedTools.end())
                continue;
            narrowed.registerBuiltin(
                def, [&subRegistry, name = def.name](const json& args) -> ToolResult {
                    // 转派回只读视图：真正的执行还是走同一份闭包
                    ToolCall c; c.id = "sub"; c.name = name; c.args = args;
                    NoUI ui;
                    return subRegistry.dispatch(c, ui);
                });
        }
        subRegistry = std::move(narrowed);
    }

    // ── 隔离边界 ③：UI ────────────────────────────────────────────────
    //   NoUI 是 **fail-closed**：confirm() 恒 false。
    //   子代理在后台跑，没有人会看到它的权限弹窗 ——
    //   "没有 UI 时的正确行为是拒绝，不是允许"（L4 定的规矩，本讲第一次真正用上）。
    NoUI ui;

    // ── 隔离边界 ①：上下文 ────────────────────────────────────────────
    //   ★ 不复用主会话的 system：主 system 里有用户需求/项目规则/仓库地图，
    //     把它塞给子代理等于**把主上下文又复制了一份**，隔离就白做了。
    ContextAssembler assembler(BudgetConfig{}, nullptr);
    assembler.setSystem(std::string(kSubagentSummaryPrompt));
    // ★★ 骨架漏掉的一行。没有它：请求里 tools=[] → 子代理只会干聊。
    assembler.setTools(subRegistry.defs());

    LoopLimits limits;
    limits.maxSteps       = spec.maxSteps;
    limits.maxTokensSpent = spec.maxTokens;

    AgentLoop loop(provider_, subRegistry, assembler, limits);

    Turn turn;
    turn.input = spec.goal;          // ★ 全新的空 history

    std::string text;
    int steps = 0;
    LoopHooks hooks;
    hooks.onChunk = [&](const ChatChunk& c) {
        std::visit(overloaded{
            [&](const TextDelta& d) { text += d.text; },
            [](const auto&) {},
        }, c);
    };
    hooks.onToolStart = [&](const ToolCall& tc) {
        ++steps;
        if (const json p = tc.args; p.contains("path") && p["path"].is_string())
            out.filesRead.push_back(p["path"].get<std::string>());
    };
    // ★ 子代理的通知**不冒泡**到主 UI：它的 spinner 会让主画面变成一团乱麻。
    //   只有"它跑了 N 步"这个事实冒泡（在 run() 返回后由宿主打）。
    hooks.onNotice = [](const std::string&) {};

    const LoopOutcome outcome = loop.run(turn, hooks);
    lastSteps_ = steps;

    // ★ 失败是**返回值**，不是异常（CR-2）：
    //   子代理超步/超预算时，主任务应该拿到"已有的部分信息 + 失败原因"，
    //   而不是让整个 agent 崩掉。
    if (outcome == LoopOutcome::Completed) {
        out.summary = text;
    } else {
        out.failed = true;
        out.error  = std::string(toString(outcome));
        out.summary = "[子代理未完成: " + out.error + "]\n已获得的部分信息:\n" + text;
    }

    // ── 隔离边界 ④：成本 ──────────────────────────────────────────────
    //   ★ token 不进主上下文，但**钱是花了**。
    //     子代理的用量记在它自己的 loop 上，主循环的 spentTokens() 一个数都看不到
    //     → 漏掉这条，`/cost` 报出来的数字就是**假的**（K-42）。
    out.usage = loop.totalUsage();
    out.truncated = (outcome == LoopOutcome::StepLimitReached ||
                     outcome == LoopOutcome::BudgetExceeded);
    return out;
}

}  // namespace af
