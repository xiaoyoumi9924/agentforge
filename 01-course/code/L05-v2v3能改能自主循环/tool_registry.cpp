// ---------------------------------------------------------------------------
//  L4 · tool_registry.cpp —— 注册 · 校验 · 权限 · 派发 · 输出过滤（机制 W3）
//
//  这是 L2 定下的契约的第一个实现。
//
//  一条贯穿全文的原则：
//      **每一步失败都是"回喂 observation"，而不是"抛异常"。**
//  模型看到错误信息会自己改。而抛异常意味着这个 turn 直接结束，
//  模型连"我错在哪"都看不到 —— 这条循环永远学不会自我修复（对应 FR-8）。
//
//  派发管线（顺序固定，五步）：
//      注册 → 校验 → 权限 → 执行 → 输出过滤
// ---------------------------------------------------------------------------
#include "tool_registry.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace af {
namespace {

// ---------------------------------------------------------------------------
//  极简 JSON Schema 校验：只检查 required 与 type。
//
//  为什么自己写而不是接一个 draft-07 validator：
//    我们要的错误信息是**给模型看的**（"缺少必需字段: path"），
//    而不是给程序员看的（"instance failed to validate against schema"）。
//    生产实现两者都要：先用完整校验器保证正确性，再翻译成模型能读的话。
// ---------------------------------------------------------------------------
struct ValidationResult {
    bool        ok = true;
    std::string error;
    explicit operator bool() const { return ok; }
};

ValidationResult validate(const json& args, const json& schema) {
    if (!schema.is_object()) return {true, {}};

    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& key : schema["required"]) {
            if (!key.is_string()) continue;
            const std::string k = key.get<std::string>();
            if (!args.is_object() || !args.contains(k))
                return {false, "缺少必需字段 `" + k + "`"};
        }
    }

    if (schema.contains("properties") && schema["properties"].is_object() && args.is_object()) {
        for (const auto& [k, spec] : schema["properties"].items()) {
            if (!args.contains(k)) continue;
            const auto want = spec.value("type", "");
            const auto& v = args[k];
            const bool bad = (want == "string"  && !v.is_string()) ||
                             (want == "number"  && !v.is_number()) ||
                             (want == "integer" && !v.is_number_integer()) ||
                             (want == "boolean" && !v.is_boolean()) ||
                             (want == "array"   && !v.is_array()) ||
                             (want == "object"  && !v.is_object());
            if (bad) return {false, "字段 `" + k + "` 的类型应为 " + want};
        }
    }
    return {true, {}};
}

// "Bash(npm test)" 这类带参数模式的匹配。
// 注意这不是安全机制 —— 它只是"要不要弹窗问人"的判据。
// 真正的安全边界在沙箱（L6，ADR-4）。
bool toolPatternMatches(const std::string& pattern, const std::string& toolName,
                        const json& args) {
    if (pattern == "*") return true;

    const std::size_t lp = pattern.find('(');
    if (lp == std::string::npos) return pattern == toolName;   // 纯工具名
    if (pattern.substr(0, lp) != toolName) return false;

    std::string argPattern = pattern.substr(lp + 1);
    if (!argPattern.empty() && argPattern.back() == ')') argPattern.pop_back();

    // 取第一个字符串参数来比（"Bash(npm test)" 关心的是 command）
    std::string first;
    if (args.is_object()) {
        for (const auto& [k, v] : args.items()) {
            if (v.is_string()) { first = v.get<std::string>(); break; }
        }
    }
    if (!argPattern.empty() && argPattern.back() == '*')
        return first.rfind(argPattern.substr(0, argPattern.size() - 1), 0) == 0;
    return first == argPattern;
}

}  // namespace

// 规则匹配：把"模式串"的解析细节关在上面那个匿名命名空间里
bool PermissionRule::matches(const std::string& toolName, const json& args) const {
    return toolPatternMatches(toolPattern, toolName, args);
}

// ===========================================================================
//  装配：注入权限门与沙箱
//
//  ★ 用 setter 注入而不是构造函数参数，是为了让 ToolRegistry 可以
//    "先建好、再装配" —— 装配顺序与依赖顺序解耦（P-16 工厂 + 依赖注入）。
//    代价是"忘了 setGate"变成运行期行为（无门 = 全部放行），
//    所以真正的实现里应当把默认值设成"最保守的拒绝"。
// ===========================================================================
void ToolRegistry::setGate(std::shared_ptr<IPermissionGate> g) {
    gate_ = std::move(g);
}

void ToolRegistry::setSandbox(std::shared_ptr<ISandbox> s) {
    sandbox_ = std::move(s);
}

// ===========================================================================
//  权限门（P-05 责任链的落地）
//
//  ★ 构造时用 stable_sort 把 deny 规则**强制**排到最前。
//    为什么不能靠配置写对顺序：规则表是人写的，"先写的 allow * 覆盖了后面的
//    deny rm -rf" 是一个**明确的安全漏洞**，而它看起来完全正常。
//    让正确的用法成为默认，比写一份文档告诉人要小心更可靠。
// ===========================================================================
DefaultPermissionGate::DefaultPermissionGate(std::vector<PermissionRule> rules,
                                             DecisionKind fallback)
    : rules_(std::move(rules)), fallback_(fallback) {
    std::stable_sort(rules_.begin(), rules_.end(), [](const PermissionRule& a,
                                                      const PermissionRule& b) {
        const bool aDeny = (a.kind == DecisionKind::Deny);
        const bool bDeny = (b.kind == DecisionKind::Deny);
        return aDeny && !bDeny;
    });
}

std::string DefaultPermissionGate::describe(const ToolDef& t, const json& args) {
    std::ostringstream os;
    os << "工具 `" << t.name << "` 想要执行。\n参数: " << args.dump(2);
    return os.str();
}

Decision DefaultPermissionGate::evaluate(const ToolDef& tool, const json& args) const {
    for (const auto& r : rules_) {
        if (r.matches(tool.name, args))
            return {r.kind, r.reason, describe(tool, args)};
    }
    return {fallback_, "无规则命中，走默认", describe(tool, args)};
}

// ===========================================================================
//  空对象（P-17）—— 没有 UI 的场景
//
//  ★ 安全默认是**拒绝**，不是允许。
//    "没有界面所以没人能批准" → 那就不能执行需要批准的事。
//    反过来写（返回 true）会让子代理/CI 场景悄悄变成"不受限"。
// ===========================================================================
bool NoUI::confirm(const std::string& /*prompt*/) { return false; }
void NoUI::notice(const std::string& /*msg*/) {}

// ===========================================================================
//  注册
// ===========================================================================
void ToolRegistry::registerBuiltin(ToolDef def, std::function<ToolResult(const json&)> fn) {
    Entry e;
    e.def = std::move(def);
    e.fn  = std::move(fn);
    entries_.push_back(std::move(e));
}

// ★ L4 新增：动态库插件
//   两个容易写错的地方：
//     ① ABI 版本必须**先校验再使用**。不校验的话，老插件加载进来会以
//        未定义行为崩掉整个进程，而且崩在插件内部，栈回溯完全指不到根因。
//     ② pluginLib 必须被 Entry 持有（shared_ptr<void>）。谁分配谁释放 ——
//        只要还有一个工具引用着这个库，就不能卸载（P-14 RAII）。
bool ToolRegistry::loadPlugin(const std::string& path, std::string& err) {
    auto h = PluginHandle::load(path, err);
    if (!h) return false;

    auto lib = std::make_shared<PluginHandle>(std::move(*h));
    for (const AFToolPlugin* t : lib->tools()) {
        if (!t) continue;
        if (t->abiVersion != AF_PLUGIN_ABI_VERSION) {
            err = "插件内工具的 ABI 版本不匹配";
            return false;
        }
        Entry e;
        e.def.name        = t->name ? t->name() : "";
        e.def.description = t->description ? t->description() : "";
        if (t->inputSchemaJson) {
            e.def.inputSchema = json::parse(t->inputSchemaJson(), nullptr, false);
            if (e.def.inputSchema.is_discarded()) e.def.inputSchema = json::object();
        }
        e.def.risk   = t->riskLevel ? RiskLevel(t->riskLevel()) : RiskLevel::Read;
        e.def.timeoutMs = t->timeoutMs ? int(t->timeoutMs()) : 30'000;
        if (t->persistThresholdChars)
            e.def.persistThresholdChars = std::size_t(t->persistThresholdChars());
        e.abi       = t;
        e.pluginLib = lib;        // 持有动态库句柄
        entries_.push_back(std::move(e));
    }
    return true;
}

std::vector<ToolDef> ToolRegistry::defs() const {
    std::vector<ToolDef> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) out.push_back(e.def);
    return out;   // ★ 顺序稳定 —— 提示缓存的前缀命中依赖它（NFR-2）
}

const ToolDef* ToolRegistry::find(const std::string& name) const {
    for (const auto& e : entries_)
        if (e.def.name == name) return &e.def;
    return nullptr;
}

// ===========================================================================
//  执行（内置函数 或 C ABI 插件）
// ===========================================================================
ToolResult ToolRegistry::invoke(const Entry& e, const json& args) {
    if (e.abi) {
        // ---- 插件路径：唯一的跨 ABI 边界 ----
        char* errOut = nullptr;
        char* raw = e.abi->execute(args.dump().c_str(), &errOut);

        if (!raw) {
            std::string msg = errOut ? errOut : "插件返回了未知错误";
            // ★ 谁分配谁释放：错误串也是插件分配的，必须由插件的 freeString 释放
            if (errOut && e.abi->freeString) e.abi->freeString(errOut);
            return ToolResult::error("", std::move(msg));
        }
        std::string content(raw);
        if (e.abi->freeString) e.abi->freeString(raw);
        return ToolResult::ok("", std::move(content));
    }
    if (e.fn) return e.fn(args);
    return ToolResult::error("", "工具未实现");
}

// ===========================================================================
//  派发 —— 五步管线，每一步失败都回喂
// ===========================================================================
ToolResult ToolRegistry::dispatch(const ToolCall& call, IUIPort& ui) {
    const Entry* entry = nullptr;
    for (const auto& e : entries_)
        if (e.def.name == call.name) { entry = &e; break; }

    // ---- 0. 未知工具：把可用工具列表一起回喂 ----
    // 只说"unknown tool"模型只能瞎猜；带上列表它下一轮基本能改对。
    if (!entry) {
        std::string names;
        for (const auto& e : entries_) {
            if (!names.empty()) names += ", ";
            names += e.def.name;
        }
        return ToolResult::error(
            call.id, "未知工具 `" + call.name + "`。可用工具: " + names);
    }

    // ---- 1. 参数校验：错误信息要能指导修正 ----
    auto v = validate(call.args, entry->def.inputSchema);
    if (!v)
        return ToolResult::error(call.id, "参数不合法: " + v.error + "\n请修正参数后重试。");

    // ---- 2. 权限判定（deny → ask → allow）----
    if (gate_) {
        Decision d = gate_->evaluate(entry->def, call.args);
        if (d.kind == DecisionKind::Deny)
            return ToolResult::error(call.id, "被策略拒绝: " + d.reason);
        if (d.kind == DecisionKind::Ask && !ui.confirm(d.prompt))
            return ToolResult::error(
                call.id, "用户拒绝了这次操作。请换一种方式，或说明为什么必须要它。");
    }

    // ---- 3. 执行 ----
    // L6 会在这里接入 ISandbox + 超时控制；本讲只有 Read，没有副作用。
    ToolResult r = invoke(*entry, call.args);

    // ---- 4. ★ 文本卫生：工具输出是**外部字节** ----
    // 仓库里混着 GBK 源文件、命令输出带控制符 —— 不消毒的话，
    // 下一轮请求 json::dump() 会抛 type_error.316。
    // 症状特别迷惑："这一步明明成功了，下一轮却整个崩掉"。
    r.content = sanitizeUtf8(r.content);

    // ---- 5. 输出过滤：超限落盘，上下文里只留预览 ----
    r.callId = call.id;
    return maybePersistLargeResult(std::move(r), entry->def, ".af-dump");
}

std::vector<ToolResult> ToolRegistry::dispatchAll(const std::vector<ToolCall>& calls,
                                                  IUIPort& ui) {
    std::vector<ToolResult> out;
    out.reserve(calls.size());
    for (const auto& c : calls) out.push_back(dispatch(c, ui));
    // 顺序与 calls 一致，但**回填时按 call.id 匹配，不依赖顺序**（P-07）
    return out;
}

// ===========================================================================
//  大结果落盘（压缩的 L0 层）
//
//  超阈值就写文件，上下文里只留约 2KB 预览 + 路径。
//
//  ★ Read 工具必须把 persistThresholdChars 设为 SIZE_MAX 豁免，
//    否则会形成"读文件 → 落盘 → 再去读那个落盘文件"的死循环依赖。
//    这个豁免不是优化，是**正确性**要求。
// ===========================================================================
ToolResult maybePersistLargeResult(ToolResult r, const ToolDef& def,
                                   const std::string& dumpDir) {
    if (def.persistThresholdChars == SIZE_MAX) return r;            // 豁免
    if (r.content.size() <= def.persistThresholdChars) return r;

    std::error_code ec;
    fs::create_directories(dumpDir, ec);

    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name =
        "toolout-" + std::to_string(now) + "-" + def.name + ".txt";
    const fs::path path = fs::path(dumpDir) / name;

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        // 落盘失败也不能让这个 turn 崩 —— 退化成"截断 + 说明"
        r.content = r.content.substr(0, 2'000) +
                    "\n\n[... 输出过大且落盘失败，以上为前 2000 字符 ...]";
        return r;
    }
    ofs.write(r.content.data(), std::streamsize(r.content.size()));
    ofs.close();

    const std::size_t total = r.content.size();
    r.content = r.content.substr(0, 2'000) +
                "\n\n[... 共 " + std::to_string(total) +
                " 字符，已落盘到 " + path.string() +
                "，如需完整内容请用 Read 打开该文件 ...]";
    r.persisted     = true;
    r.persistedPath = path.string();
    return r;
}

}  // namespace af
