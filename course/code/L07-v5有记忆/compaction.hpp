// ---------------------------------------------------------------------------
//  L7 · compaction.hpp —— 上下文压缩（模块 ④ 的后半截，机制 W1）
//
//  ===========================================================================
//  这一讲为什么**必须**新增文件（而 L6 为什么不该新增）
//  ===========================================================================
//
//  L6 一句话把六道闸门塞进了 `agent_loop.cpp`，一个字节的头文件都没动。
//  那一讲的说法是"好架构允许你加能力而不加模块"。如果 L7 也照抄这个说法，
//  就会得出一个错误的结论：**"加能力都不该加模块"**。那是错的。
//
//  判据不是"改了几行"，而是：**这次加的东西，是否引入了一个需要独立演进的
//  概念？**
//
//      L6 的"闸门"：它是循环的**属性**（循环的边界条件），
//                   它的状态（步数、起始时刻、错误计数）本来就该和循环同生共死，
//                   它的唯一调用点就是循环体的开头。→ 不该新增模块。
//
//      L7 的"压缩"：它有**自己的状态**（上一次的摘要、切点边界、抖动计数）、
//                   自己的**策略**（五层级联、摘要器可换）、自己的**测试面**
//                   （切点合法性、级联逐级、摘要不丢路径），
//                   而且它的调用点不止循环一处（L10 的 --resume 也要用它把
//                   落盘的历史装回上下文）。→ **必须新增模块**。
//
//  一句话：**闸门是循环的一部分；压缩不是循环的一部分，循环只是它的调用者。**
//
//  ===========================================================================
//  五层级联：能不压就不压，能便宜压就便宜压
//  ===========================================================================
//
//    级   手段                 成本      放在哪
//    ---- -------------        ------    ------------------------------------
//    L0   大结果落盘           ≈ 0       上游：tool_registry 的输出过滤
//    L1   单条消息降级链       ≈ 0       本文件（调 context_budget 的 fitTo）
//    L2   折叠老的工具结果     ≈ 0       本文件
//    L3   一串历史 → 交接摘要   低/高     本文件（摘要器可换：本地 / LLM）
//    L4   只留锚点 + 最近窗口   ≈ 0       本文件
//
//  ★ 为什么必须是"级联"而不是"一上来就总结"：
//    压缩**本身就贵**。LLM 摘要那一次调用，输入恰恰就是你想压掉的那段上下文 ——
//    你为了省 40k token，先花掉 30k token 去生成摘要。
//    如果每次都直接走 L3，那么"上下文满了"这件事会变成**越压越贵**。
//    所以顺序永远是：**先用免费的手段，不够再花钱。**
//
//  ★ L0 不在本文件里，这是刻意的 —— 也是本讲一个反直觉的点：
//    **同一个级联可以横跨多个模块。** L0（落盘）属于"工具输出过滤"，
//    它必须发生在结果进入上下文的**那一刻**（工具派发管线的最后一步），
//    而不是事后由压缩器去找。级联的每一级只要求"知道自己是谁"，
//    不要求"都住在一个文件里"。
//
//  ===========================================================================
//  切点铁律（本文件存在的最大理由）
//  ===========================================================================
//
//      **只在 user / assistant 消息之前切，绝不在 tool_result 处切。**
//
//  违反它会发生什么：被切下来的 `tool` 消息失去了它的
//  `assistant(tool_calls)`，变成**孤儿 tool_result** → 服务端直接 400。
//  而且报错信息完全指不到"是压缩干的"。
//
//  这就是 L5 的 G8（消息合法性）与 L6 的 K-11（警告必须以 tool 角色回喂）
//  的**第三次出现**。三次的根因是同一个：
//
//      `assistant(tool_calls)` 与它的结果之间，不允许出现任何非 tool 消息；
//      反过来说，`tool` 消息也不允许失去它的 assistant。
//
//  所以压缩器的切点判定**不是"哪里 token 少切哪里"，而是"哪里切完仍然合法"**。
//  这也是为什么压缩必须知道协议 —— 它是"上下文管理"能踩到协议层的唯一入口。
// ---------------------------------------------------------------------------
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "context_budget.hpp"     // BudgetConfig（复用同一份分层预算）/ Degrade
#include "model_provider.hpp"     // IModelProvider / Usage
#include "types.hpp"              // Message / CompactionEvent

namespace af {

// ===========================================================================
//  摘要器端口
//
//  ★ 为什么摘要要是一个**端口**而不是"直接调 provider"：
//    三个理由，每一个都够单独成立：
//      ① 离线可用：验证脚本没有 API Key。如果摘要只能走模型，
//         那么"压缩"这条最重要的链路就**永远无法被验证** ——
//         参考实现正是这么写的，而它的验证报告里写着
//         "压缩：有实现，但没在真实长会话里跑过"。这就是代价。
//      ② 可降级：L3 是级联里唯一花钱的一级。它失败时应当能退到 L4，
//         而不是让整个 turn 崩掉（CR-2：失败是返回值，不是异常）。
//      ③ 可换策略：本地抽取式摘要 / LLM 摘要 / "总结成 checklist"……
//         它们是**同一种机制的三种策略**（P-04 策略模式）。
// ===========================================================================
struct SummarizeRequest {
    const std::vector<Message>* messages = nullptr;   // 要被压掉的那段历史
    std::string                 previousSummary;      // 上一次的摘要（迭代压缩）
    std::string                 goal;                 // ★ 锚点原文（最初的需求），摘要必须原样带上
    std::optional<std::string>  focus;                // "只关注 X"
    std::optional<std::stop_token> stop;              // ★ 摘要也要能被 Ctrl-C 打断
};

struct SummaryResult {
    std::string text;
    Usage       usage;        // ★ 摘要这次调用的用量**必须回报** —— 它也是钱
    std::string note;         // 人可读：这次摘要由谁做的
};

class ISummarizer {
public:
    virtual ~ISummarizer() = default;
    virtual SummaryResult summarize(const SummarizeRequest& req) = 0;
};

// ---------------------------------------------------------------------------
//  实现 A：LLM 摘要（真实生产路径）
//
//  一次**非流式**请求。注意这里与主循环的三个不同：
//    ① 非流式：摘要不需要边写边看，而且我们只要最终文本
//    ② temperature = 0：摘要必须稳定，同样输入要得到同样的输出
//    ③ 不写提示缓存：这是一次性 prompt，写缓存是纯亏
// ---------------------------------------------------------------------------
class LlmSummarizer final : public ISummarizer {
public:
    explicit LlmSummarizer(IModelProvider& provider, int maxTokens = 2'048)
        : provider_(provider), maxTokens_(maxTokens) {}

    SummaryResult summarize(const SummarizeRequest& req) override;

private:
    IModelProvider& provider_;
    int maxTokens_;
};

// ---------------------------------------------------------------------------
//  实现 B：本地抽取式摘要（离线 / 降级路径）
//
//  ★ 它不是"假实现"，它是一个**正确的降级策略**：
//    不调用模型，只用规则把"交接必须保住的信息"挑出来 ——
//      原始需求 / 改过哪些文件 / 调过哪些工具 / 最近的结论
//    这正是交接摘要里**最不能丢**的四类信息（丢了就得重新探索一遍仓库）。
//    代价是它不"理解"内容，所以只适合当 L3 的降级路径，
//    而不是替代 LLM 摘要。
// ===========================================================================
class ExtractiveSummarizer final : public ISummarizer {
public:
    explicit ExtractiveSummarizer(int maxChars = 4'000) : maxChars_(maxChars) {}
    SummaryResult summarize(const SummarizeRequest& req) override;

private:
    int maxChars_;
};

// ===========================================================================
//  压缩配置
//
//  ★ 它直接**复用** `BudgetConfig` 而不是另造一套窗口/预留参数 ——
//    "上下文能装多少"这件事只能有一个真相来源。两套参数的结果是：
//    组装器按 200k 装、压缩器按 100k 压，然后你花一整天去查
//    "为什么压缩老是触发但上下文看起来还很空"。
// ===========================================================================
struct CompactionConfig {
    bool          enabled = true;
    BudgetConfig  budget;             // ★ 与组装器共用同一份
    int  keepRecentTokens  = 20'000;  // 最近多少 token 逐字保留（L2/L3 的切点）
    int  foldThresholdChars = 1'200;  // 单条工具结果超过它就折叠（L2）
    int  perMessageCapDiv  = 2;       // L1 的单条上限 = 历史预算 / 这个数
    int  maxSummaryChars   = 4'000;   // 摘要自身也要有上限，否则它又会撑爆
    int  maxAttempts       = 3;       // 抖动保护：连压 N 次仍满 → 报告而不是死循环
};

struct CompactionOutcome {
    bool triggered = false;
    bool thrashing = false;                     // 压完立刻又满
    std::vector<CompactionEvent> events;        // ★ 逐级记录，见 types.hpp
    Usage  summaryUsage;                        // ★ 必须计入成本（闸门 2）
    int tokensBefore = 0, tokensAfter = 0;
    int keptMessages = 0, droppedMessages = 0;

    bool anyLlmSummary() const noexcept {
        for (const auto& e : events) if (e.level == 3 && !e.summary.empty()) return true;
        return false;
    }
};

// ===========================================================================
//  压缩器
// ===========================================================================
class Compactor {
public:
    Compactor(CompactionConfig cfg, std::shared_ptr<ISummarizer> summarizer)
        : cfg_(std::move(cfg)), summarizer_(std::move(summarizer)) {}

    const CompactionConfig& config() const noexcept { return cfg_; }

    // 估算一段历史的 token（与组装器的口径**必须一致**，否则两边会打架）
    static int estimateHistoryTokens(const std::vector<Message>& history);

    // 该压了吗：历史超过 L5 层的预算就压
    bool shouldCompact(int historyTokens) const noexcept;

    // ★ 切点铁律的落地：user / assistant 合法，tool_result **不合法**。
    //   判据只有一行，但它决定了压缩后的历史会不会让服务端直接 400。
    static bool isLegalCutPoint(const Message& m) noexcept;

    // 主入口。**原地**修改 history。
    // 不会抛异常（CR-2）：摘要失败 → 退到 L4；抖动 → 置 thrashing 标志后返回。
    //
    // ★ `goalHint`（L7 真跑时补上的一个参数）：
    //   摘要必须**自包含** —— 一份"不知道自己要干什么"的交接文档没有用。
    //   摘要里的原始需求优先取 `goalHint`，其次才回退到"锚点"（history[0] 是
    //   user 消息时）。
    //
    //   为什么需要它：**锚点不一定存在**。本课的 CLI 是"用户输入永远由组装器
    //   追加在最后"（L6 的规矩），所以 `turn.history` 里**根本没有 user 消息**，
    //   `history[0]` 是 assistant。也就是说锚点机制在这里是"正确的但够不着"。
    //   历史里没有、而调用方知道的原始需求，只能由调用方传进来。
    //   （真实的多轮会话 / L10 的 --resume 里 history[0] 就是 user，那时锚点生效。）
    CompactionOutcome compact(std::vector<Message>& history,
                              int historyTokens,
                              const std::optional<std::string>& focus = std::nullopt,
                              std::optional<std::stop_token> stop = std::nullopt,
                              const std::string& goalHint = std::string());

private:
    // 从尾部往前累积，找"保留最近 keepTokens 够用"的那个合法切点
    std::size_t findCutPoint(const std::vector<Message>& h,
                             std::size_t searchFrom, int keepTokens) const;

    // 找"最靠后的、使 history[k..] 放得进 budget"的合法切点（L4 用）
    std::size_t findTailStartByBudget(const std::vector<Message>& h,
                                      std::size_t searchFrom, int budget) const;

    int foldOldToolResults(std::vector<Message>& h, std::size_t begin, std::size_t end);

    CompactionConfig cfg_;
    std::shared_ptr<ISummarizer> summarizer_;
    std::string lastSummary_;        // 迭代压缩：上一次的摘要当作这一次的输入
    int attempts_ = 0;               // 抖动计数
};

// ---------------------------------------------------------------------------
//  交接摘要的提示词。
//
//  ★ 第 3 条（绝对路径）是单独一条，因为它对应**压缩后最常见的失败模式**：
//    模型知道要改什么，却不知道改哪个文件，只好把整个仓库重新探索一遍 ——
//    于是刚省下来的 40k token，用 60k token 的探索补回去了。
//    压缩如果丢掉"路径"，它省下的钱是假的。
// ---------------------------------------------------------------------------
inline constexpr std::string_view kCompactionPrompt =
R"(你正在执行 CONTEXT CHECKPOINT COMPACTION（上下文检查点压缩）。
请为另一个将接手此任务的 LLM 生成交接摘要，务必包含：

1. 当前进展与已经做出的关键决策（含"为什么这么定"）
2. 重要的约束：用户偏好、项目约定、不能碰的东西
3. 所有读取过或修改过的文件的**绝对路径**（这一条最重要，一条都不能漏）
4. 还没做完的事（清晰的下一步）
5. 继续工作所必需的关键数据、示例或引用

要求：简洁、结构化、只写事实。目标是让下一个 LLM 能无缝接手，不必重读文件。)";

}  // namespace af
