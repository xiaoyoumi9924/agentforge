// ---------------------------------------------------------------------------
//  L7 · context_budget.cpp —— 上下文组装 + **预算与降级链**（机制 W1）
//
//  L5 只做了**一半**的组装器，而且是刻意的一半：
//
//      做   ：L0 系统提示 / L5 历史（+ 合法性守卫）/ L6 用户输入 / 工具表下发
//      不做 ：token 预算裁剪、降级链、L3 检索命中、L4 活动文件
//
//  为什么只做一半：**预算和降级是 L7 的内容**。它们存在的理由是"上下文会满"，
//  而 L5 的历史最多十几条消息 —— 现在做预算等于在没有问题的时候解决问题。
//
//  ★ L7 补上另一半，并且同时定义了 L2 就声明、但一直没定义的三个函数。
//
//    "声明了但没定义"为什么能挂 5 讲而不炸？—— 因为**没有任何地方调用它们**。
//    LNK2019 只在符号被**引用**时才发生。这正好回收 L4 踩过的那个坑：
//    **接口可以先定，没被用到的部分不产生代价。**
//    但代价在后面：它们一直没被验证过。而 L7 一调用，就发现了两个问题：
//      ① `BudgetConfig::forLayer(L5_History)` 返回值语义是错的（见 types.hpp）
//      ② `extractSignatures` 的"词法占位"对**非 C++ 文本**（日志、二进制转储）
//         会把几乎所有行都判成签名 —— 因为那些行里有括号。见下面的注释。
// ---------------------------------------------------------------------------
#include "context_budget.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace af {

namespace {

// 粗估：4 字符 ≈ 1 token。生产实现必须接真实 tokenizer ——
// 用估算值做预算决策时，安全边际要给足（见本文件末尾的说明）。
constexpr int kCharsPerToken = 4;

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
//
//  ★ L7 补一句：**压缩器必须复用这个不变量**。压缩的切点不是"哪里 token 少切哪里"，
//    而是"哪里切完仍然合法"。所以 compaction.cpp 里也有一份同样的判定
//    （`isLegalCutPoint`），而且 verify 里专门造了"并行工具调用"这个最刁钻的用例。
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
//  ★ L7：预算与降级链
//
//  这一节回答一个问题：**一层内容放不下时怎么办？**
//
//  最直觉的答案是"截断"。它是错的，而且是**危险**的错：
//    截断会产出"半个函数"、"半条 SQL"、"只剩左括号的条件表达式" ——
//    这种内容对模型的伤害比"什么都没有"更大，因为它**看着像完整的**，
//    模型会基于残缺代码做出错误判断，而且它不会意识到自己看到的是残片。
//
//  正解是**降级链**：按"信息量从多到少、但每一级都自洽"的顺序退：
//
//      全文  →  签名  →  符号名  →  仅路径  →  丢弃
//      Full     Signatures  SymbolNames  PathOnly  Drop
//
//  每一级都保留**语义线索**：看到 "这里有个叫 foo 的函数" 比看到
//  "半个 foo 的函数体" 有用得多 —— 前者模型知道自己该去 Read，
//  后者模型会以为自己已经知道 foo 怎么写。
// ===========================================================================

int ContextAssembler::estimateTokens(std::string_view s) noexcept {
    return int(s.size() / kCharsPerToken) + 1;
}

namespace {
std::string truncateToTokens(std::string_view s, int tokens) {
    const std::size_t maxChars = std::size_t(tokens) * kCharsPerToken;
    // ★ K-21：必须按 UTF-8 边界截断 —— 否则会在中文/emoji 上切出非法字节序列
    return utf8Prefix(s, maxChars);
}
}  // namespace

std::string ContextAssembler::fitTo(std::string content, int tokenBudget, Degrade& applied) {
    applied = Degrade::Full;
    if (tokenBudget <= 0) { applied = Degrade::Drop; return {}; }

    if (estimateTokens(content) <= tokenBudget) return content;

    applied = Degrade::Signatures;
    std::string sig = extractSignatures(content);
    if (estimateTokens(sig) <= tokenBudget) return sig;

    applied = Degrade::SymbolNames;
    std::string names = extractSymbolNames(content);
    if (estimateTokens(names) <= tokenBudget) return names;

    // 仅路径：对"文件内容"降级到只剩第一行（通常是路径/标题注释）。
    // ★ 注意这里**不是**随便截 8 个 token —— 是取首行。
    //   首行对代码文件通常是对外可识别的信息，截前 32 个字符则什么都不是。
    applied = Degrade::PathOnly;
    std::string firstLine = content.substr(0, content.find('\n'));
    std::string head = truncateToTokens(firstLine, std::min(8, std::max(1, tokenBudget)));
    head += "\n[... 内容已降级为" +
            std::to_string(content.size()) + " 字符的首行摘要 ...]";
    if (estimateTokens(head) <= tokenBudget) return head;

    applied = Degrade::Drop;
    return {};
}

// ---------------------------------------------------------------------------
//  ★ 这里必须诚实：这是**词法占位**，不是语法分析。
//
//  判据是"这一行里有没有左括号 / 是不是以 class|struct|def|func|export 开头"。
//  它对 C++/Python/JS 源码大致够用（够到"能演示降级链"的程度），
//  但有两个已知的失效场景，讲义里明说：
//
//    ① 非代码文本（日志、JSON、CSV）里几乎每行都有括号 →
//       "签名"退化成"全文"，降级链在这一级**等于没降**。
//    ② 多行声明（模板、跨行的函数签名）只留第一行 → 产出语法不完整的残片。
//
//  两者都要靠 L8 接 tree-sitter 解决（用 tag 查询拿到真正的声明区间）。
//  本讲要做的是**把降级链的机制跑通并可验证**，而不是假装有了语法分析。
//  这也是"未实现的部分要明说"（L6 的 `--sandbox-info` 是同一条纪律）。
// ---------------------------------------------------------------------------
std::string ContextAssembler::extractSignatures(std::string_view src) {
    std::string out;
    std::size_t pos = 0;
    while (pos < src.size()) {
        const std::size_t nl = src.find('\n', pos);
        std::string_view line =
            src.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
        if (line.find('(') != std::string_view::npos ||
            line.starts_with("class ") || line.starts_with("struct ") ||
            line.starts_with("def ")   || line.starts_with("func ") ||
            line.starts_with("export ")) {
            out += line; out += '\n';
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    // 一行都没抽出来（比如纯数据文件）→ 退回"前 2000 字符"，
    // 而不是返回空串。返回空串会让调用方以为"这层内容没有信息"。
    // ★ 2000 是**字节**上限，所以必须 utf8Prefix（K-21）。
    if (out.empty()) return utf8Prefix(src, 2'000);
    return out;
}

std::string ContextAssembler::extractSymbolNames(std::string_view src) {
    // 只留标识符：在签名行的基础上再砍掉参数表，配合 tree-sitter 的 tag 表最准确。
    // 现在是"每行最多 80 字符"的粗暴版本 —— 见上面的诚实说明。
    std::string out;
    const std::string sigs = extractSignatures(src);
    std::size_t pos = 0;
    while (pos < sigs.size()) {
        const std::size_t nl = sigs.find('\n', pos);
        std::string_view line =
            sigs.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
        // 砍掉 '(' 之后的部分 —— 那就是"参数列表"，属于实现细节
        const std::size_t lp = line.find('(');
        if (lp != std::string_view::npos) line = line.substr(0, lp);
        out += utf8Prefix(line, 80);          // ★ K-21：80 是字节，不是字符
        out += '\n';
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return out;
}

// ===========================================================================
//  组装
//
//  顺序是**固定**的，因为顺序 = 缓存前缀 = 省钱 + 省延迟（NFR-2）：
//
//      L0 system ─ L1/L2（并入 system）─ L5 history ─ L3 检索 ─ L4 活动文件 ─ L6 用户输入
//
//  两条互相拉扯的原则：
//    · 稳定优先：越稳定的内容越靠前 → 利于前缀缓存命中
//    · 相关优先：越相关的内容越靠后 → 利于模型注意力
//
//  ★ L7 新增的是**每一层的预算**，不是顺序。顺序从 L5 起没变过 ——
//    这很重要：顺序一变，所有人攒下来的提示缓存全部作废。
//    "压缩会改变内容，但不该改变顺序" 是这一讲必须守住的边界。
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
    //   ★ 这里**不做**预算裁剪。历史的预算由压缩器（Compactor）负责，
    //     因为"压缩历史"可能花一次模型调用，必须由循环记账（闸门 2 要算这笔钱）。
    //     组装器只负责"把手上这份历史合法地摆好"。
    for (auto& m : guardMessageLegality(turn.history)) req.messages.push_back(std::move(m));

    // ---- ③ L3：检索命中（★ L7 起受预算约束）----
    //   预算在**逐条**之间均分：一条超大的命中不该把其他命中全挤掉。
    //   ★ 注意 `Turn::retrieved` 是 `Message` 的列表（不是 RetrievedChunk）——
    //     组装器只负责"把资料摆进去"，把检索结果包成 Message 是上层的事。
    {
        const int n = std::max<int>(1, int(turn.retrieved.size()));
        const int per = cfg_.forLayer(Layer::L3_Retrieved) / n;
        for (const auto& c : turn.retrieved) {
            Degrade d = Degrade::Full;
            std::string s = fitTo(c.content, per, d);
            if (s.empty()) continue;             // Drop：这一条放弃，但其他的还在
            Message m;
            m.role    = Role::User;              // 检索资料是"给模型看的"，不是既有对话
            m.content = "[检索命中]\n" + s;
            req.messages.push_back(std::move(m));
        }
    }

    // ---- ④ L4：活动文件（★ L7 起受预算约束）----
    //   ★ 活动文件必须**编辑后立即刷新**，否则模型会基于旧内容继续改 ——
    //     这是"改了但没生效"类问题的头号来源。所以它排在 L3 之后（更靠后 = 更相关）。
    {
        const int n = std::max<int>(1, int(turn.activeFiles.size()));
        const int per = cfg_.forLayer(Layer::L4_ActiveFiles) / n;
        for (const auto& f : turn.activeFiles) {
            Degrade d = Degrade::Full;
            std::string s = fitTo(f.content, per, d);
            if (s.empty()) continue;
            Message m;
            m.role    = Role::User;
            m.content = "[活动文件]\n" + s;
            req.messages.push_back(std::move(m));
        }
    }

    // ---- ⑤ L6：用户输入永远最后、永远完整 ----
    //   ★ 用户输入**永不裁剪**。它可以超预算 —— 超预算时该做的是压缩别的东西，
    //     或者直接告诉用户"你这条输入太长了"，而不是偷偷截掉一半。
    //     "悄悄截断用户输入"是能把一个好好的框架变成不可用的那种设计。
    req.messages.push_back(Message{});
    req.messages.back().role    = Role::User;
    req.messages.back().content = turn.input;

    // ---- ⑥ 工具表：属于"模型能看到什么"的一部分，由组装器统一负责 ----
    //      顺序必须稳定 —— ToolRegistry::defs() 保证只 push_back 不排序。
    req.tools = tools_;

    req.cacheControl = CacheControl{true, 1024};
    return req;
}

// ===========================================================================
//  各层占用统计（供 L12 的 /context 可视化，也是压缩器的触发判据）
// ===========================================================================
ContextAssembler::LayerUsage ContextAssembler::measure(const Turn& turn) const {
    LayerUsage u;
    u.l0 = estimateTokens(system_);
    u.l1 = estimateTokens(project_);
    u.l2 = estimateTokens(repoMap_);
    for (const auto& c : turn.retrieved)   u.l3 += estimateTokens(c.content);
    for (const auto& f : turn.activeFiles) u.l4 += estimateTokens(f.content);
    for (const auto& m : turn.history) {
        u.l5 += estimateTokens(m.content);
        // ★ L6 忘了一件事：tool_call 的**参数**也是要发出去的 token。
        //   只统计 content 会系统性低估（一个带 200 行新内容的 Edit 调用，
        //   参数可能比 content 大一个数量级）。低估的后果是压缩永不触发。
        for (const auto& tc : m.toolCalls) u.l5 += estimateTokens(tc.args.dump());
    }
    u.l6 = estimateTokens(turn.input);
    u.total = u.l0 + u.l1 + u.l2 + u.l3 + u.l4 + u.l5 + u.l6;
    return u;
}

}  // namespace af
