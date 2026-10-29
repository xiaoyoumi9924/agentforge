// ---------------------------------------------------------------------------
//  L7 · compaction.cpp —— 五层级联、切点铁律、两种摘要器
//
//  ★ 本文件是"参考实现有实现、但从来没人跑过"的那一块。
//    `09-核心代码/真跑验证报告.md` 里对它的原话是：
//
//        压缩（compaction） | LLM 摘要 + 切点查找有实现，但没在真实长会话里跑过
//
//    本讲把它接进循环、真跑起来，于是暴露出三个问题（讲义 §4 详述）：
//      K-16  `req.model = provider_.id()` —— 摘要请求发出去了 model="openai"
//      K-17  摘要失败时 `throw` —— CR-2 说得清清楚楚：失败是返回值，不是异常。
//            摘要是一次**网络调用**，它失败的概率比谁都高；
//            一个失败的网络调用把整个 turn 炸掉，是最不该有的设计。
//      K-18  切点只判了"不是 tool_result"，没判"**这个位置之后**是否完整" ——
//            见下面 findCutPoint 的注释。
//
//    这三条都是**只有真跑才会出现**的问题。这正是本课那句硬要求的来源：
//    "验证脚本必须真跑并留下日志"——"应该能跑"和"跑过了"之间，平均藏着 2~3 个 bug。
// ---------------------------------------------------------------------------
#include "compaction.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

namespace af {

namespace {

int est(std::string_view s) { return ContextAssembler::estimateTokens(s); }

int estMessage(const Message& m) {
    int t = est(m.content);
    // ★ 工具的**参数**也要算。一个带 200 行新内容的 Edit 调用，
    //   它的 args 比 content 大一个数量级 —— 漏算会让"该压的时候没压"。
    for (const auto& tc : m.toolCalls) t += est(tc.args.dump());
    return t;
}

const char* roleTag(Role r) {
    switch (r) {
        case Role::User:      return "用户";
        case Role::Assistant: return "助手";
        case Role::Tool:      return "工具结果";
        case Role::System:    return "系统";
    }
    return "?";
}

std::string renderForSummary(const Message& m) {
    std::ostringstream os;
    os << "[" << roleTag(m.role);
    if (m.role == Role::Tool && !m.toolCallId.empty()) os << " " << m.toolCallId;
    os << "] ";
    if (!m.content.empty()) os << m.content;
    os << "\n";
    for (const auto& tc : m.toolCalls)
        os << "  → 调用 " << tc.name << "(" << tc.args.dump() << ")\n";
    return os.str();
}

}  // namespace

// ===========================================================================
//  LlmSummarizer —— 真实路径（一次非流式请求）
// ===========================================================================
SummaryResult LlmSummarizer::summarize(const SummarizeRequest& req) {
    std::string body;
    if (!req.goal.empty())
        body += "# 本任务的原始需求（**必须原样保留在摘要里**）\n" + req.goal + "\n\n";
    if (!req.previousSummary.empty())
        body += "# 此前的会话摘要（请并入新摘要，不要丢信息）\n" +
                req.previousSummary + "\n\n";
    body += "# 需要摘要的对话\n";
    if (req.messages)
        for (const auto& m : *req.messages) body += renderForSummary(m);
    if (req.focus) body += "\n\n# 聚焦要求\n" + *req.focus;

    ChatRequest cr;
    // ★ K-16：不要写 `cr.model = provider_.id()`。
    //   那是**厂商标识**（"openai" / "anthropic"），不是模型名（"gpt-4o-mini"）。
    //   参考实现里正是这么写的 —— 后果是摘要请求发出去 model="openai"，
    //   而报错长得像"模型名不存在"，你会去查配置和密钥，就是不会怀疑这一行。
    //   留空 → provider 回退到 cfg_.model。**这与 L5 组装器里那条注释是同一个坑。**
    cr.system = std::string(kCompactionPrompt);

    Message u;
    u.role    = Role::User;
    u.content = body;
    cr.messages.push_back(std::move(u));

    cr.maxTokens    = maxTokens_;
    cr.temperature  = 0.0;                       // 摘要必须稳定：同输入同输出
    cr.cacheControl = CacheControl{false, 0};    // 一次性 prompt，写缓存是纯亏
    cr.stop         = req.stop;                  // ★ Ctrl-C 必须能打断摘要
    cr.trace.turnId = "compaction";

    ChatResponse resp = provider_.chat(cr);

    SummaryResult r;
    r.text  = resp.message.content;
    r.usage = resp.usage;
    r.note  = std::string("llm:") + std::string(provider_.id());
    return r;
}

// ===========================================================================
//  ExtractiveSummarizer —— 离线路径（也是 L3 的降级策略）
//
//  它挑的是**交接口诀的四件事**：
//      原始需求 / 改过哪些文件 / 用过哪些工具 / 最近的结论
//  ★ 注意它刻意**不**总结"工具结果的内容" —— 那些内容要么已经落盘（L0），
//    要么可以用 Read 重取。真正不可再生的信息是"需求和决策"。
//    这一点决定了本地摘要**够用**：它不是"劣化版的 LLM 摘要"，
//    而是一条**成本为零的正经路径**。
// ===========================================================================
SummaryResult ExtractiveSummarizer::summarize(const SummarizeRequest& req) {
    SummaryResult r;
    r.note = "extractive(local)";
    if (!req.messages) { r.text = "(无可摘要内容)"; return r; }
    const auto& msgs = *req.messages;

    std::string firstUser, lastAssistant;
    int nUser = 0, nAsst = 0, nTool = 0;
    std::map<std::string, int> toolUse;
    std::vector<std::string> touched;

    for (const auto& m : msgs) {
        switch (m.role) {
            case Role::User:
                ++nUser;
                if (firstUser.empty()) firstUser = m.content;
                break;
            case Role::Assistant:
                ++nAsst;
                if (!m.content.empty()) lastAssistant = m.content;
                for (const auto& tc : m.toolCalls) {
                    ++toolUse[tc.name];
                    // 四个原语的参数形状都是 {path/command: string, ...} ——
                    // 取"第一个短字符串参数"，它就是最可能的那条路径。
                    if (!tc.args.is_object()) continue;
                    for (const auto& [k, v] : tc.args.items()) {
                        (void)k;
                        if (!v.is_string()) continue;
                        const std::string s = v.get<std::string>();
                        if (s.empty() || s.size() > 200) continue;
                        if (s.find('\n') != std::string::npos) continue;
                        if (std::find(touched.begin(), touched.end(), s) == touched.end())
                            touched.push_back(s);
                        break;   // 每个调用只取一个
                    }
                }
                break;
            case Role::Tool: ++nTool; break;
            case Role::System: break;
        }
    }

    std::ostringstream os;
    os << "【本地抽取式交接摘要】已压缩 " << msgs.size() << " 条消息"
       << "（user=" << nUser << " / assistant=" << nAsst << " / tool=" << nTool << "）\n";
    if (!req.previousSummary.empty())
        os << "- 更早会话的摘要（沿用以避免层层丢失）：\n  "
           << utf8Prefix(req.previousSummary, 600) << "\n";
    // ★ 原始需求一律**原样带上**：优先用锚点里的原文（那是用户亲手写的、最准），
    //   其次才是被摘要区间里出现的第一条 user 消息。
    //   为什么它必须出现在摘要里（而不是靠"反正锚点还在"）：
    //     · 摘要要能独立成立 —— L10 的 --resume 从磁盘装回上下文时，
    //       未必还有那条锚点消息
    //     · 摘要是给"另一个模型"看的交接文档，一份不知道自己要干什么的
    //       交接文档是没有用的
    const std::string goal = !req.goal.empty() ? req.goal : firstUser;
    if (!goal.empty())
        os << "- 原始需求：" << utf8Prefix(goal, 400) << "\n";
    if (!toolUse.empty()) {
        os << "- 已用过的工具：";
        bool first = true;
        for (const auto& [k, v] : toolUse) {
            if (!first) os << "、";
            os << k << "×" << v;
            first = false;
        }
        os << "\n";
    }
    if (!touched.empty()) {
        os << "- 涉及的文件/参数（★ 交接口诀：路径一条都不能丢）：\n";
        for (const auto& t : touched) os << "  · " << t << "\n";
    }
    if (!lastAssistant.empty())
        os << "- 压缩前最后的结论：" << utf8Prefix(lastAssistant, 400) << "\n";

    std::string s = os.str();
    // ★ 摘要自身也要有上限（否则它又会撑爆上下文），
    //   而且**同样必须按字符边界截断**（K-21）。
    utf8Resize(s, std::size_t(maxChars_));
    r.text = std::move(s);
    return r;
}

// ===========================================================================
//  Compactor
// ===========================================================================
int Compactor::estimateHistoryTokens(const std::vector<Message>& history) {
    int t = 0;
    for (const auto& m : history) t += estMessage(m);
    return t;
}

bool Compactor::shouldCompact(int historyTokens) const noexcept {
    return cfg_.enabled && historyTokens > cfg_.budget.forLayer(Layer::L5_History);
}

// ★ 铁律，一行代码，决定压缩后的历史会不会让服务端直接 400。
bool Compactor::isLegalCutPoint(const Message& m) noexcept {
    // ❌ tool_result 不合法：它与自己那条 assistant(tool_calls) 是**一个整体**，
    //    切开就产生孤儿 tool_result → 400，且报错指不到"是压缩干的"。
    return m.role != Role::Tool;
}

// ---------------------------------------------------------------------------
//  找切点**不是**"哪里 token 少切哪里"，而是"哪里切完仍然合法"。
//
//  ★ K-18：参考实现只把 tool_result 排除在"候选切点"之外。这**不够** ——
//    它还漏了一件事：**计费要算全部消息，候选只能取合法位置**。
//    参考实现写成"跳过 tool_result，同时也不把它计入 acc"，于是
//    当尾部都是 tool_result（最常见的情况！）时，acc 只累加了那几条
//    很小的 assistant/user → 永远到不了 keepRecentTokens →
//    切点被推到最前面 → 等于**什么都没保留**，
//    而日志上还写着"压缩成功"。这类错误不会报错，只会让模型失忆。
//
//    本实现把两件事分开：
//      ① 从尾部往前**累加所有消息**（真实的保留量）→ 得到 boundary
//      ② 把 boundary **向前**挪到最近的合法切点
//    "向前"是多保留几条，是安全方向；"向后"会切掉某条 assistant、
//    让它的结果变孤儿 —— 那是 400。
//    方向选错，症状从"上下文没变短"直接跳到"服务端拒绝请求"。
// ---------------------------------------------------------------------------
std::size_t Compactor::findCutPoint(const std::vector<Message>& h,
                                    std::size_t searchFrom, int keepTokens) const {
    if (searchFrom >= h.size()) return h.size();

    // ① 累加全部消息，找到"保留这么多就够"的位置
    int acc = 0;
    std::size_t boundary = h.size();
    for (std::size_t i = h.size(); i-- > searchFrom; ) {
        acc += estMessage(h[i]);
        boundary = i;
        if (acc >= keepTokens) break;
    }

    // ② 向前挪到合法切点（多保留，绝不会切出孤儿）
    for (std::size_t i = boundary + 1; i-- > searchFrom; ) {
        if (isLegalCutPoint(h[i])) return i;
    }
    // 一个合法切点都找不到（整段都是 tool_result）→ 这一轮不切。
    // 返回 h.size() 是"放弃切分"的信号，调用方据此退到下一级。
    return h.size();
}

// 找"最靠后的、使 history[k..] 放得进 budget"的合法切点（L4 丢弃时用）。
std::size_t Compactor::findTailStartByBudget(const std::vector<Message>& h,
                                             std::size_t from, int budget) const {
    if (budget < 0) budget = 0;
    int acc = 0;
    std::size_t best = h.size();
    for (std::size_t i = h.size(); i-- > from; ) {
        const int t = estMessage(h[i]);
        if (acc + t > budget) break;
        acc += t;
        if (isLegalCutPoint(h[i])) best = i;
    }
    // ★ 至少保住最后一条消息（并向前挪到合法切点）。
    //   不然"丢弃"会退化成"失忆"—— 那比压缩失败更糟：
    //   模型会理直气壮地基于空上下文重新开始，而用户以为任务还在继续。
    if (best >= h.size() && !h.empty()) {
        best = h.size() - 1;
        while (best > from && !isLegalCutPoint(h[best])) --best;
    }
    return best;
}

int Compactor::foldOldToolResults(std::vector<Message>& h, std::size_t begin,
                                  std::size_t end) {
    int n = 0;
    for (std::size_t i = begin; i < end && i < h.size(); ++i) {
        Message& m = h[i];
        if (m.role != Role::Tool) continue;
        if (int(m.content.size()) <= cfg_.foldThresholdChars) continue;

        const std::size_t total = m.content.size();
        // ★ K-21：300 是**字节**上限。中文源码里第 300 字节几乎必然落在某个
        //   汉字中间 —— 直接 substr 会产出非法 UTF-8，然后下一轮 dump() 抛异常。
        //   这个 bug 只在"中文内容 + 触发折叠"时出现，纯英文仓库永远看不到。
        const std::string preview = utf8Prefix(m.content, 300);
        m.content = "[早前工具结果已折叠] 原始 " + std::to_string(total) +
                    " 字符 → 仅保留前 300 字符\n" + preview +
                    "\n[... 已折叠。需要完整内容请用 Read 重新读取 —— "
                    "路径在对应的工具调用参数里，那份参数没有丢 ...]";
        ++n;
    }
    return n;
}

// ===========================================================================
//  主入口：五层级联
// ===========================================================================
CompactionOutcome Compactor::compact(std::vector<Message>& history, int historyTokens,
                                     const std::optional<std::string>& focus,
                                     std::optional<std::stop_token> stop,
                                     const std::string& goalHint) {
    CompactionOutcome out;
    out.tokensBefore = historyTokens;
    if (!shouldCompact(historyTokens)) return out;
    out.triggered = true;

    auto cur = [&] { return estimateHistoryTokens(history); };
    const int budget = cfg_.budget.forLayer(Layer::L5_History);

    // ★ 锚点：`history[0]` 如果是 user 消息，那是**最初的需求**，永不压缩。
    //   "连续读 50 个文件之后还记得最初要干什么" —— 靠的就是这一个变量。
    //   它比任何摘要技巧都可靠：摘要可能写歪，锚点不会。
    const std::size_t anchorEnd =
        (!history.empty() && history[0].role == Role::User) ? 1u : 0u;

    // 切点（L1/L2/L3 共用）。prefixEnd 之前的 [anchorEnd, prefixEnd) 就是"老历史"。
    // ★ cut == history.size() 表示"找不到合法切点" → 这一轮不切（交给 L4）。
    //
    // ★ 注意它现在被提到 L1 **之前**：因为 L1 也必须在"老历史"上工作，
    //   见下面 L1 的说明。
    const std::size_t cut = findCutPoint(history, anchorEnd, cfg_.keepRecentTokens);
    const bool canSplit = (cut < history.size());
    const std::size_t prefixEnd = canSplit ? std::max(cut, anchorEnd) : anchorEnd;

    // 已经"不许再动"的前缀长度。
    //   · 锚点（最初的需求）永远不许动
    //   · 摘要**也不许被后面的级别删掉** —— 否则 L3 刚花掉的那次模型调用
    //     会被 L4 一笔勾销，钱白花，而且日志上还写着"压缩成功"两次。
    std::size_t protectedEnd = anchorEnd;

    // ---------------------------------------------------------------- L1
    //  单条消息降级（免费）。触发条件是"某一条特别大"——
    //  典型场景：刚 Read 了一个 3000 行的文件，那一**条**就顶得上整个历史预算。
    //
    //  ★ K-19（真跑才发现的坑）：L1 **只能动"老历史"，绝不能动最近窗口**。
    //
    //    第一版把 L1 写成"扫过所有消息，谁超上限就降谁"，看起来更彻底。
    //    真跑起来立刻出问题：模型刚 Read 完一个文件，那条 tool_result 就是
    //    "最近窗口"里的、而且是它**正在用**的东西。把它降级成签名表 →
    //    模型发现"这个文件我怎么只看到函数名" → **再读一遍同一个文件** →
    //    闸门 5 判定重复调用 → 整个 turn 被判死。
    //
    //    症状是"压缩之后 agent 卡住了"，而根因是"压缩把正在用的东西弄坏了"。
    //    规律：**降级只能作用于"已经可以牺牲"的内容。**
    //    "最近窗口"的定义就是"还没牺牲的内容" —— 两者必须是同一个边界，
    //    否则压缩就会一边省 token 一边制造返工。
    if (cur() > budget && prefixEnd > anchorEnd) {
        const int before = cur();
        const int cap = std::max(64, budget / std::max(1, cfg_.perMessageCapDiv));
        int changed = 0;
        for (std::size_t i = anchorEnd; i < prefixEnd; ++i) {
            Message& m = history[i];
            if (est(m.content) <= cap) continue;
            Degrade d = Degrade::Full;
            m.content = ContextAssembler::fitTo(std::move(m.content), cap, d);
            if (d != Degrade::Full) ++changed;
        }
        if (changed > 0) {
            CompactionEvent ev;
            ev.level          = 1;
            ev.tokensBefore   = before;
            ev.tokensAfter    = cur();
            ev.keptMessages   = int(history.size());
            ev.detail = "L1 单条降级：老历史里超预算的 " + std::to_string(changed) +
                        " 条消息走了降级链（全文→签名→符号名→首行）";
            out.events.push_back(std::move(ev));
        }
    }

    // ---------------------------------------------------------------- L2
    //  折叠老的工具结果（免费）。注意它只动 **tool_result**，而且只动
    //  "老"的那一段 —— 最近窗口里的结果原样保留，因为模型多半正在用它。
    if (cur() > budget && prefixEnd > anchorEnd) {
        const int before = cur();
        const int folded = foldOldToolResults(history, anchorEnd, prefixEnd);
        if (folded > 0) {
            CompactionEvent ev;
            ev.level              = 2;
            ev.tokensBefore       = before;
            ev.tokensAfter        = cur();
            ev.keptMessages       = int(history.size());
            ev.summarizedMessages = folded;
            ev.detail = "L2 折叠：" + std::to_string(folded) +
                        " 条老工具结果压成「前 300 字符 + 说明」";
            out.events.push_back(std::move(ev));
        }
    }

    // ---------------------------------------------------------------- L3
    //  摘要（唯一花钱的一级，也是唯一"有损但保语义"的一级）。
    if (cur() > budget && prefixEnd > anchorEnd && summarizer_) {
        const int before = cur();
        std::vector<Message> toSummarize(history.begin() + std::ptrdiff_t(anchorEnd),
                                         history.begin() + std::ptrdiff_t(prefixEnd));

        SummarizeRequest sreq;
        sreq.messages        = &toSummarize;
        sreq.previousSummary = lastSummary_;
        // ★ 摘要的"原始需求"有两个来源，优先级如下：
        //   ① goalHint —— 调用方（循环）知道、而历史里没有的那份需求
        //   ② 锚点原话 —— history[0] 是 user 消息时（多轮会话 / --resume）
        //   为什么要按这个顺序：锚点更"原始"但**不一定存在**（本课 CLI 里
        //   history 全是 assistant/tool，见 compaction.hpp 的说明），
        //   而 goalHint 一定存在。两者都拿不到时，摘要就只能靠 firstUser 兜底，
        //   那已经是最差情况了。
        if (!goalHint.empty())
            sreq.goal = goalHint;
        else if (anchorEnd > 0 && !history.empty())
            sreq.goal = history[0].content;
        sreq.focus           = focus;
        sreq.stop            = stop;

        bool ok = true;
        SummaryResult sr;
        try {
            sr = summarizer_->summarize(sreq);
        } catch (const std::exception& e) {
            // ★ K-17：参考实现在这里 `throw`。那违反 CR-2，而且后果很重：
            //   摘要是一次网络调用，失败概率最高；让一个失败的网络调用
            //   把整个 turn 炸掉，等于把"没网"变成"不能用"。
            //   正确做法：**降级**。摘要做不了，还有 L4 丢弃这一手。
            ok = false;
            CompactionEvent ev;
            ev.level        = 3;
            ev.tokensBefore = before;
            ev.tokensAfter  = before;
            ev.keptMessages = int(history.size());
            ev.detail = std::string("L3 摘要失败（") + e.what() +
                        "）→ 降级到 L4 丢弃（失败是返回值，不是异常）";
            out.events.push_back(std::move(ev));
        }
        if (ok && sr.text.empty()) ok = false;      // 空摘要 == 没摘要

        if (ok) {
            out.summaryUsage = sr.usage;
            lastSummary_     = sr.text;

            std::vector<Message> rebuilt;
            rebuilt.reserve(history.size() + 1);
            for (std::size_t i = 0; i < anchorEnd; ++i) rebuilt.push_back(history[i]);

            Message sm;
            sm.role    = Role::User;
            sm.content = "[上下文压缩 · 交接摘要] 更早的 " +
                         std::to_string(toSummarize.size()) +
                         " 条消息已压缩为下面的摘要。它保留了原始需求、"
                         "已改动的文件与关键决策 —— 继续工作时以它为准。\n"
                         "（摘要来源：" + sr.note + "）\n\n" + sr.text;
            rebuilt.push_back(std::move(sm));
            for (std::size_t i = prefixEnd; i < history.size(); ++i)
                rebuilt.push_back(history[i]);

            // ★ 铁律兜底：摘要插在锚点之后，所以"摘要的下一条"不能是 tool_result。
            //   正常情况下不会发生（prefixEnd 是合法切点），但这里刻意保留这段 ——
            //   因为"切点合法性"是别处的保证，而**别处的保证随时可能被改坏**。
            //   廉价的不变量检查，比一个只在生产环境出现的 400 划算太多。
            while (rebuilt.size() > anchorEnd + 1 &&
                   rebuilt[anchorEnd + 1].isToolResult()) {
                rebuilt.erase(rebuilt.begin() + std::ptrdiff_t(anchorEnd + 1));
            }
            history.swap(rebuilt);
            protectedEnd = anchorEnd + 1;      // ★ 摘要也不许再被删

            CompactionEvent ev;
            ev.level              = 3;
            ev.tokensBefore       = before;
            ev.tokensAfter        = cur();
            ev.keptMessages       = int(history.size());
            ev.summarizedMessages = int(toSummarize.size());
            ev.summary            = sr.text;
            ev.detail = "L3 摘要：" + std::to_string(toSummarize.size()) +
                        " 条老消息 → 一份交接摘要（" + sr.note + "，" +
                        std::to_string(sr.text.size()) + " 字符）";
            out.events.push_back(std::move(ev));
            out.droppedMessages += int(toSummarize.size());
        }
    }

    // ---------------------------------------------------------------- L4
    //  丢弃：最后一手。只留"受保护的前缀 + 放得下的最近窗口"。
    if (cur() > budget) {
        const int before = cur();
        int headTokens = 0;
        for (std::size_t i = 0; i < protectedEnd && i < history.size(); ++i)
            headTokens += estMessage(history[i]);
        const std::size_t keepFrom =
            findTailStartByBudget(history, protectedEnd, budget - headTokens);
        if (keepFrom > protectedEnd) {
            const int dropped = int(keepFrom - protectedEnd);
            history.erase(history.begin() + std::ptrdiff_t(protectedEnd),
                          history.begin() + std::ptrdiff_t(keepFrom));
            CompactionEvent ev;
            ev.level              = 4;
            ev.tokensBefore       = before;
            ev.tokensAfter        = cur();
            ev.keptMessages       = int(history.size());
            ev.summarizedMessages = dropped;
            ev.detail = "L4 丢弃：" + std::to_string(dropped) +
                        " 条老消息被丢弃（保留锚点/摘要 + 最近窗口）";
            out.events.push_back(std::move(ev));
            out.droppedMessages += dropped;
        }
    }

    out.tokensAfter  = cur();
    out.keptMessages = int(history.size());

    // --------------------------------------------------------- 抖动保护
    //  压完立刻又满（比如单个超大文件被读入，或工具在疯狂产出）
    //  → 计数，N 次后报告 thrashing。
    //  ★ 注意：**报告**，不是 throw（K-17）。由循环决定怎么处理 ——
    //    它是"这一轮的结果"，不是"这个进程的意外"。
    if (out.tokensAfter > budget && !out.events.empty()) {
        if (++attempts_ >= cfg_.maxAttempts) {
            out.thrashing = true;
            attempts_ = 0;
        }
    } else {
        attempts_ = 0;
    }
    return out;
}

}  // namespace af
