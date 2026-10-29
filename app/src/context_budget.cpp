// ---------------------------------------------------------------------------
//  L5 · context_budget.cpp —— 上下文分层组装与 token 预算（模块 ④ / ADR-5）
//
//  两条互相拉扯的排序原则：
//    · 稳定优先：越稳定的内容越靠前 → 前缀缓存命中（省钱省延迟）
//    · 相关优先：越相关的内容越靠后 → 模型注意力
//  解法：L0/L1/L2 严格固定在前，L3/L4 紧跟历史后，L6 用户输入永远最后。
//
//  本讲实现：
//    · assemble：分层组装 ChatRequest（system + 历史 + 检索/文件块 + 用户输入）
//    · fitTo：超预算时的降级链（全文 → 签名 → 符号名 → 路径 → 丢弃）
//    · measure：供 /context 可视化各层占用
//
//  ★ 顺序即缓存前缀：工具表顺序、system 内容、历史顺序都必须稳定。
// ---------------------------------------------------------------------------
#include "context_budget.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace af {

namespace {

// 粗估 token：中文按字、英文按 4 字节 1 token 估算。
// 真实实现应接 tokenizer（IModelProvider::countTokens），这里只用于预算与降级。
int estimateTokens(std::string_view s) {
    return int((s.size() + 3) / 4);
}

// ---------------------------------------------------------------------------
//  降级链：全文 → 签名 → 符号名 + 路径 → 仅路径 → 丢弃
//
//  ★ 为什么是链而不是一刀截断：直接切掉会产生"半个函数"这种有害内容，
//    模型会基于残缺代码做出错误判断。按签名/符号名降级至少保留语义线索。
// ---------------------------------------------------------------------------
bool isCommentOrBlank(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) return true;
    return line.compare(i, 2, "//") == 0;
}

bool looksLikeSignature(std::string_view line) {
    // 剪掉行尾空白
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    if (line.empty()) return false;
    if (line.find('#') != std::string_view::npos && line.front() == '#') return true;   // #include/#define

    // "名字(...);" 或 "名字(...) {" 或 "返回类型 名字(...) {"
    const bool hasParen  = line.find('(') != std::string_view::npos;
    const bool hasClose  = line.find(')') != std::string_view::npos;
    const bool endsBrace = !line.empty() && (line.back() == '{' || line.back() == ';' || line.back() == ':');
    if (!hasParen || !hasClose || !endsBrace) return false;

    // 过滤明显的控制语句与调用
    static const char* kControl[] = {
        "if (", "for (", "while (", "switch (", "catch (", "return ", "else",
    };
    for (const char* c : kControl) {
        if (line.find(c) != std::string_view::npos) return false;
    }
    return true;
}

}  // namespace

// ============================================================================
//  构造与配置
// ============================================================================
ContextAssembler::ContextAssembler(BudgetConfig cfg, std::shared_ptr<IUIPort> ui)
    : cfg_(cfg), ui_(std::move(ui)) {}

const BudgetConfig& ContextAssembler::config() const noexcept { return cfg_; }
IUIPort* ContextAssembler::uiPort() const noexcept { return ui_.get(); }

void ContextAssembler::setSystem(std::string s)            { system_ = std::move(s); }
void ContextAssembler::setProjectKnowledge(std::string s)  { project_ = std::move(s); }
void ContextAssembler::setRepoMap(std::string map)         { repoMap_ = std::move(map); }
void ContextAssembler::setTools(std::vector<ToolDef> tools) { tools_ = std::move(tools); }

// ============================================================================
//  降级链：fitTo
//
//  预算以 token 计（estimateTokens 粗估）；返回实际采用的那一级内容。
//  PathOnly 取块首行（块格式约定为 "[标记] path:行-行" 或 path 首行）。
// ============================================================================
std::string ContextAssembler::fitTo(std::string content, int tokenBudget, Degrade& applied) {
    applied = Degrade::Full;
    if (estimateTokens(content) <= tokenBudget) return content;

    // ---- 1. 签名 ----
    {
        const std::string sig = extractSignatures(content);
        if (!sig.empty() && estimateTokens(sig) <= tokenBudget) {
            applied = Degrade::Signatures;
            return sig;
        }
    }
    // ---- 2. 符号名 ----
    {
        const std::string sym = extractSymbolNames(content);
        if (!sym.empty() && estimateTokens(sym) <= tokenBudget) {
            applied = Degrade::SymbolNames;
            return sym;
        }
    }
    // ---- 3. 仅路径（块首行）----
    {
        const std::size_t nl = content.find('\n');
        const std::string pathLine = (nl == std::string::npos) ? content : content.substr(0, nl);
        if (!pathLine.empty() && estimateTokens(pathLine) <= tokenBudget) {
            applied = Degrade::PathOnly;
            return pathLine;
        }
    }
    // ---- 4. 丢弃 ----
    applied = Degrade::Drop;
    return {};
}

std::string ContextAssembler::extractSignatures(std::string_view src) {
    const std::string text(src);
    std::istringstream is(text);
    std::string line, out;
    while (std::getline(is, line)) {
        if (isCommentOrBlank(line)) continue;
        if (looksLikeSignature(line)) {
            out += line;
            out += '\n';
            if (out.size() > 4000) break;   // 预算上限：签名本身就够短
        }
    }
    return out;
}

std::string ContextAssembler::extractSymbolNames(std::string_view src) {
    std::set<std::string> names;
    std::string cur;
    auto flush = [&]() {
        if (cur.size() >= 3) names.insert(cur);
        cur.clear();
    };
    for (const char ch : src) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '_') cur.push_back(ch);
        else flush();
    }
    flush();

    std::string out;
    std::size_t n = 0;
    for (const auto& s : names) {
        if (!out.empty()) out += ", ";
        out += s;
        if (++n >= 200) { out += " ..."; break; }
    }
    return out;
}

// ============================================================================
//  历史裁剪：以"块"为单位（assistant + 紧随的 tool 消息）从旧往新丢弃，
//  保证 tool/assistant 配对不被拆散。
// ============================================================================
namespace {

std::size_t historyTokens(const std::vector<Message>& hist) {
    std::size_t t = 0;
    for (const auto& m : hist) t += std::size_t(estimateTokens(m.content)) + 4;   // +4：消息壳开销
    return t;
}

// 把历史按"块"切分：assistant(toolCalls) 与其后连续 tool 消息为一组，其余逐条成组。
std::vector<std::vector<Message>> splitBlocks(const std::vector<Message>& hist) {
    std::vector<std::vector<Message>> blocks;
    std::size_t i = 0;
    while (i < hist.size()) {
        std::vector<Message> b;
        if (hist[i].role == Role::Assistant && hist[i].hasToolCalls()) {
            b.push_back(hist[i]);
            std::size_t j = i + 1;
            while (j < hist.size() && hist[j].role == Role::Tool) b.push_back(hist[j++]);
            i = j;
        } else {
            b.push_back(hist[i]);
            ++i;
        }
        blocks.push_back(std::move(b));
    }
    return blocks;
}

std::size_t blockTokens(const std::vector<Message>& b) { return historyTokens(b); }

}  // namespace

// ============================================================================
//  组装
//
//  顺序（即缓存前缀）：
//      L0+L1+L2  → system（固定，永不裁剪的内容）
//      L5 历史   → 按预算从旧往新丢弃（块级）
//      L3 检索   → 用户消息块（相关优先，靠近用户输入）
//      L4 文件   → 用户消息块
//      L6 用户输入 → 永远最后、永远完整
// ============================================================================
ChatRequest ContextAssembler::assemble(const Turn& turn) {
    ChatRequest req;

    // ---- L0 系统提示：永不裁剪 ----
    std::string systemBlock = system_.empty() ? turn.systemPrompt : system_;
    if (!systemBlock.empty()) {
        if (ui_ && estimateTokens(systemBlock) > cfg_.forLayer(Layer::L0_System)) {
            ui_->notice("系统提示超出 L0 预算，已保留全文（L0 永不裁剪）");
        }
    }

    // ---- L1 项目知识 / L2 仓库地图：超预算则降级 ----
    if (!project_.empty()) {
        Degrade d;
        std::string fit = fitTo(project_, cfg_.forLayer(Layer::L1_Project), d);
        if (!fit.empty()) systemBlock += "\n\n## 项目知识\n" + fit;
        else if (ui_) ui_->notice("项目知识超出 L1 预算，已丢弃");
    }
    if (!repoMap_.empty()) {
        Degrade d;
        std::string fit = fitTo(repoMap_, cfg_.forLayer(Layer::L2_RepoMap), d);
        if (!fit.empty()) systemBlock += "\n\n## 仓库地图\n" + fit;
    }
    if (!systemBlock.empty()) req.system = systemBlock;

    // ---- L5 历史：块级裁剪 ----
    const int l5Budget = cfg_.forLayer(Layer::L5_History);
    std::vector<Message> hist = turn.history;
    {
        auto blocks = splitBlocks(hist);
        std::size_t used = 0;
        std::size_t dropped = 0;
        std::vector<std::vector<Message>> kept;
        for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
            const std::size_t bt = blockTokens(*it);
            if (used + bt > std::size_t(std::max(0, l5Budget)) && !kept.empty()) {
                dropped = std::size_t(std::distance(it, blocks.rend()));
                break;
            }
            used += bt;
            kept.push_back(*it);
        }
        std::reverse(kept.begin(), kept.end());
        std::vector<Message> rebuilt;
        for (auto& b : kept) for (auto& m : b) rebuilt.push_back(m);
        if (dropped && ui_)
            ui_->notice("上下文超预算：丢弃了最早的 " + std::to_string(dropped) + " 组历史消息");
        hist.swap(rebuilt);
    }

    // ---- L3 检索命中（降级链）----
    // Turn::retrieved 是 Message 列表（内容由检索模块拼好，此处只做预算与降级）。
    int remainingL3 = cfg_.forLayer(Layer::L3_Retrieved);
    std::vector<Message> contextBlocks;
    for (const auto& c : turn.retrieved) {
        if (remainingL3 <= 0) break;
        std::string block = "[检索命中]\n" + c.content;
        Degrade d;
        const int cap = std::min(remainingL3, estimateTokens(block));
        std::string fit = fitTo(block, cap, d);
        if (fit.empty()) continue;
        remainingL3 -= estimateTokens(fit);
        Message m;
        m.role = Role::User;
        m.content = std::move(fit);
        contextBlocks.push_back(std::move(m));
    }

    // ---- L4 活动文件（降级链）----
    int remainingL4 = cfg_.forLayer(Layer::L4_ActiveFiles);
    for (const auto& f : turn.activeFiles) {
        if (remainingL4 <= 0) break;
        std::string block = "[活动文件]\n" + f.content;
        Degrade d;
        const int cap = std::min(remainingL4, estimateTokens(block));
        std::string fit = fitTo(block, cap, d);
        if (fit.empty()) continue;
        remainingL4 -= estimateTokens(fit);
        Message m;
        m.role = Role::User;
        m.content = std::move(fit);
        contextBlocks.push_back(std::move(m));
    }

    // ---- 组装消息序列 ----
    req.messages = std::move(hist);
    for (auto& m : contextBlocks) req.messages.push_back(std::move(m));

    {
        Message input;
        input.role = Role::User;
        input.content = turn.input;       // L6：永远最后、永远完整
        req.messages.push_back(std::move(input));
    }

    // ---- 工具表：顺序稳定（缓存前缀依赖它）----
    req.tools      = tools_;
    req.toolChoice = ToolChoice::Auto;
    req.maxTokens  = cfg_.maxOutputTokens;
    return req;
}

// ============================================================================
//  测量：供 /context 可视化（与 assemble 的分配口径保持一致）
// ============================================================================
ContextAssembler::LayerUsage ContextAssembler::measure(const Turn& turn) const {
    LayerUsage u;
    u.l0 = estimateTokens(system_.empty() ? turn.systemPrompt : system_);
    u.l1 = estimateTokens(project_);
    u.l2 = estimateTokens(repoMap_);
    u.l5 = int(historyTokens(turn.history));
    u.l6 = estimateTokens(turn.input);

    std::size_t l3 = 0;
    for (const auto& c : turn.retrieved) {
        const std::string block = "[检索命中]\n" + c.content;
        l3 += std::size_t(estimateTokens(block)) + 4;
    }
    std::size_t l4 = 0;
    for (const auto& f : turn.activeFiles) {
        const std::string block = "[活动文件]\n" + f.content;
        l4 += std::size_t(estimateTokens(block)) + 4;
    }
    u.l3 = int(l3);
    u.l4 = int(l4);
    u.total = u.l0 + u.l1 + u.l2 + u.l3 + u.l4 + u.l5 + u.l6;
    return u;
}

}  // namespace af
