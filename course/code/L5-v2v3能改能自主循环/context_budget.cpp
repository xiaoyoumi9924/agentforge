// ---------------------------------------------------------------------------
//  L5 · context_budget.cpp —— 上下文组装（机制 W1 的前半截）
//
//  本讲只做**一半**的组装器，而且是刻意的一半：
//
//      做   ：L0 系统提示 / L5 历史（+ 合法性守卫）/ L6 用户输入 / 工具表下发
//      不做 ：token 预算裁剪、降级链、L3 检索命中、L4 活动文件
//
//  为什么只做一半：**预算和降级是 L7 的内容**。它们存在的理由是"上下文会满"，
//  而 L5 的历史最多十几条消息 —— 现在做预算等于在没有问题的时候解决问题。
//
//  ★ 注意：本文件**故意不定义** `fitTo` / `extractSignatures` /
//    `extractSymbolNames` 三个函数（它们在 context_budget.hpp 里已声明）。
//    这不影响链接 —— 因为**没有任何地方调用它们**。
//
//    这正好回收 L4 踩过的那个坑（LNK2019）：
//    "声明了但没定义"只有在**被调用**时才会炸。
//    → 接口可以先定，没被用到的部分不产生代价。
// ---------------------------------------------------------------------------
#include "context_budget.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace af {

namespace {

// 粗估：4 字符 ≈ 1 token。生产实现必须接真实 tokenizer ——
// 用估算值做预算决策时，安全边际要给足（见 L7）。
constexpr int kCharsPerToken = 4;

int estimateTokens(std::string_view s) {
    return int(s.size() / kCharsPerToken) + 1;
}

// ---------------------------------------------------------------------------
//  ★ G8 —— 消息合法性守卫
//
//  协议硬要求：每一条 `tool` 消息必须紧跟"请求了它那个 id 的 assistant 消息"，
//  而且这个 assistant 消息**不能**被别的消息隔开。
//
//  三类破坏方式（后果都是 400，但报错完全指不到根因）：
//    ① 孤儿 tool       —— 裁剪时删了 assistant，留下 tool
//    ② 配对窗口被打断   —— 中间插了一条 user（比如系统的错误回喂）
//    ③ 只回一半        —— 一次请求了 3 个工具，只回填了 1 条结果
//
//  ★ 判据必须是**集合**，不能是"前一条是不是 assistant"。
//    很多实现（含本课的参考实现）写成：
//        if (m.isToolResult() && !out.back().hasToolCalls()) continue;
//    这在"一次只调一个工具"时是对的，但并行调用会**吃掉合法数据**：
//        [user] [assistant(tc1,tc2)] [tool(tc1)] [tool(tc2)]
//                                       ↑ 到这里 back()=assistant，留下
//                                                  ↑ 到这里 back()=tool(tc1)，
//                                                    没有 tool_calls → 被丢弃！
//    症状是"模型一次读 3 个文件，只看到 1 个结果"，看起来像模型记性不好。
// ---------------------------------------------------------------------------
std::vector<Message> guardMessageLegality(const std::vector<Message>& msgs) {
    std::vector<Message> out;
    out.reserve(msgs.size());

    std::set<std::string> waiting;   // 待配对的 tool_call_id 集合

    for (const auto& m : msgs) {
        if (m.role == Role::Assistant && !m.toolCalls.empty()) {
            // 新的一批工具调用开始：登记整批 id，重置配对窗口
            waiting.clear();
            for (const auto& tc : m.toolCalls) waiting.insert(tc.id);
            out.push_back(m);

        } else if (m.isToolResult()) {
            // ★ 只有"引用的 id 还在待配对集合里"才算合法
            if (waiting.find(m.toolCallId) == waiting.end()) {
                continue;                       // 孤儿 / 重复 / 伪造 → 丢弃
            }
            out.push_back(m);
            waiting.erase(m.toolCallId);        // 消掉一个：并行调用要消 N 次

        } else {
            // 任何其他消息都会**关闭配对窗口**。
            // 看起来"保留集合"能让更多历史活下来，但那会造出
            // "本地能跑、服务端 400"的历史 —— 本地宽松是负债，不是宽容。
            waiting.clear();
            out.push_back(m);
        }
    }
    return out;
}

}  // namespace

// ===========================================================================
//  构造与访问器
// ===========================================================================

ContextAssembler::ContextAssembler(BudgetConfig cfg, std::shared_ptr<IUIPort> ui)
    : cfg_(cfg), ui_(std::move(ui)) {}

const BudgetConfig& ContextAssembler::config() const noexcept { return cfg_; }

IUIPort* ContextAssembler::uiPort() const noexcept { return ui_.get(); }

// 以下三个 setter 的内容都是"会话内基本不变"的 → 属于**缓存前缀**部分。
// 顺序即缓存：改这些内容会让缓存前缀失效，所以要尽量少改、且改完别再动。
void ContextAssembler::setSystem(std::string s)           { system_   = std::move(s); }
void ContextAssembler::setProjectKnowledge(std::string s) { project_  = std::move(s); }
void ContextAssembler::setRepoMap(std::string map)        { repoMap_  = std::move(map); }

// ★ 工具表必须注册进来，否则请求里 tools 为空 ——
//   模型永远看不到工具，也就永远不会产出 tool_call。
//   "模型不听话"最常见的假象，根因在这里（L4 的教训，这里再钉一次）。
void ContextAssembler::setTools(std::vector<ToolDef> tools) { tools_ = std::move(tools); }

// ===========================================================================
//  组装
//
//  顺序是**固定**的，因为顺序 = 缓存前缀 = 省钱 + 省延迟（NFR-2）：
//
//      L0 system ─ L5 history ─ L6 user_input   （L1/L2/L3/L4 在 L7/L8 插入）
//
//  两条互相拉扯的原则：
//    · 稳定优先：越稳定的内容越靠前 → 利于前缀缓存命中
//    · 相关优先：越相关的内容越靠后 → 利于模型注意力
// ===========================================================================
ChatRequest ContextAssembler::assemble(const Turn& turn) {
    ChatRequest req;
    // ★ 不要在这里设 req.model。
    //   provider_->id() 是**厂商标识**（"openai"），不是模型名（"mock-1"）。
    //   写成 req.model = provider.id() 会让所有请求发 model="openai"，
    //   而报错看起来像"模型名不存在"—— 你会去查配置和密钥，就是不会怀疑这一行。
    //   留空 → provider 自己回退到 cfg_.model。

    // ---- ① L0：系统提示（缓存前缀，必须字节级稳定）----
    req.system = system_;
    if (!project_.empty()) req.system = *req.system + "\n\n# 项目知识\n" + project_;
    if (!repoMap_.empty()) req.system = *req.system + "\n\n# 仓库地图\n" + repoMap_;

    // ---- ② L5：历史（★ 过一遍合法性守卫）----
    for (auto& m : guardMessageLegality(turn.history)) req.messages.push_back(std::move(m));

    // ---- ③ L6：用户输入永远最后、永远完整 ----
    req.messages.push_back(Message{});
    req.messages.back().role    = Role::User;
    req.messages.back().content = turn.input;

    // ---- ④ 工具表：属于"模型能看到什么"的一部分，由组装器统一负责 ----
    //      顺序必须稳定 —— ToolRegistry::defs() 保证只 push_back 不排序。
    req.tools = tools_;

    req.cacheControl = CacheControl{true, 1024};
    return req;
}

// ===========================================================================
//  各层占用统计（供 L12 的 /context 可视化）
// ===========================================================================
ContextAssembler::LayerUsage ContextAssembler::measure(const Turn& turn) const {
    LayerUsage u;
    u.l0 = estimateTokens(system_);
    u.l1 = estimateTokens(project_);
    u.l2 = estimateTokens(repoMap_);
    for (const auto& c : turn.retrieved)   u.l3 += estimateTokens(c.content);
    for (const auto& f : turn.activeFiles) u.l4 += estimateTokens(f.content);
    for (const auto& m : turn.history)     u.l5 += estimateTokens(m.content);
    u.l6 = estimateTokens(turn.input);
    u.total = u.l0 + u.l1 + u.l2 + u.l3 + u.l4 + u.l5 + u.l6;
    return u;
}

}  // namespace af
