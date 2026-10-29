// ---------------------------------------------------------------------------
//  repo_index.cpp —— 符号表 → 依赖图 → PageRank → 按预算渲染
//
//  L6 之前的占位实现在这里被换成了真解析器；换的只有 parseFile() 里调用
//  extractSymbols() 的那一处，建图 / PageRank / 渲染一行没动 ——
//  这就是"把解析隔离在一个函数里"的全部价值。
// ---------------------------------------------------------------------------
#include "repo_index.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>

namespace af {
namespace {

uint64_t hashFile(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    auto mt = fs::last_write_time(p, ec).time_since_epoch().count();
    return uint64_t(sz) ^ (uint64_t(mt) << 32);
}

std::string readAll(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream os; os << ifs.rdbuf();
    return os.str();
}

// 名字归一化：KeyManager / key_manager / keymanager 视作同一个。
// ★ 不归一化会怎样：C++ 仓库里文件名基本是 snake_case、类名是 CamelCase，
//   直接字符串比较的话**一条边都建不起来**，PageRank 退化成均匀分布，
//   地图就变成"按目录顺序截一段"。这个 bug 不会报错，只会让排序悄悄失效。
std::string normalizeName(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_' || c == '-') continue;
        out.push_back(char(std::tolower((unsigned char)c)));
    }
    return out;
}

std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && issp((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && issp((unsigned char)s[i])) ++i;
    return s.substr(i);
}

}  // namespace

// ---------------------------------------------------------------------------
//  忽略规则
// ---------------------------------------------------------------------------

void RepoIndex::loadIgnoreRules() {
    ignoreNames_.clear();
    if (!cfg_.respectGitignore) return;
    std::ifstream ifs(root_ / ".gitignore");
    if (!ifs) return;
    std::string line;
    while (std::getline(ifs, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        // 只认"纯名字"条目（build/ 、*.o 、a/b/c 一律不认）。
        // 完整 gitignore 语法需要通配与目录语义，本课刻意不做 —— 见讲义 K-。
        if (s.find('*') != std::string::npos || s.find('/') != std::string::npos) continue;
        ignoreNames_.insert(s);
    }
}

bool RepoIndex::shouldSkip(const fs::path& p) const {
    std::error_code ec;
    fs::path rel = fs::relative(p, root_, ec);
    if (ec) rel = p;
    for (const auto& part : rel) {
        const std::string name = part.string();
        if (name == "." || name == "..") continue;
        if (std::find(cfg_.skipDirs.begin(), cfg_.skipDirs.end(), name) != cfg_.skipDirs.end())
            return true;
        if (ignoreNames_.count(name)) return true;
    }
    return false;
}

bool RepoIndex::wanted(const fs::path& p) const {
    return std::find(cfg_.extensions.begin(), cfg_.extensions.end(),
                     p.extension().string()) != cfg_.extensions.end();
}

// ---------------------------------------------------------------------------
//  构建 / 更新
// ---------------------------------------------------------------------------

void RepoIndex::build(fs::path root) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    root_ = std::move(root);
    symbols_.clear(); files_.clear(); fileIdx_.clear(); fileHashes_.clear();
    rawIncludes_.clear();
    stats_ = {};
    loadIgnoreRules();

    std::error_code ec;
    for (fs::recursive_directory_iterator it(root_, ec), end; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const auto& p = it->path();
        if (!wanted(p)) continue;
        if (shouldSkip(p)) { ++stats_.skippedIgnored; continue; }
        parseFile(p);
    }
    buildGraph();

    stats_.buildMs = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

void RepoIndex::update() {
    bool dirty = false;
    for (const auto& p : files_) {
        uint64_t h = hashFile(p);
        if (fileHashes_[p.string()] != h) { parseFile(p); fileHashes_[p.string()] = h; dirty = true; }
    }
    // 新文件：重新扫一遍（可换成 inotify/FSEvents 监听）
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root_, ec), end; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const auto& p = it->path();
        if (!wanted(p)) continue;
        if (shouldSkip(p)) continue;
        if (!fileIdx_.count(p.string())) { parseFile(p); dirty = true; }
    }
    if (dirty) buildGraph();
}

void RepoIndex::parseFile(const fs::path& p) {
    const std::string key = p.string();
    if (fileIdx_.find(key) == fileIdx_.end()) {
        fileIdx_.emplace(key, files_.size());
        files_.push_back(p);
        ++stats_.files;
    }
    fileHashes_[key] = hashFile(p);

    // 先量尺寸再读盘：25MB 的生成文件不该被整个读进内存
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (!ec && sz > cfg_.extract.maxBytes) { ++stats_.skippedTooBig; return; }

    const std::string src = readAll(p);
    if (src.size() > cfg_.extract.maxBytes) { ++stats_.skippedTooBig; return; }
    collectIncludes(src, fileIdx_[key]);

    const bool hasGrammar =
        cfg_.extract.backend != SymbolBackend::Lexical && grammarRegistered(p.extension().string());
    if (hasGrammar) stats_.usedTreeSitter = true; else ++stats_.skippedNoGrammar;

    auto syms = extractSymbols(p, src, cfg_.extract);
    // ★ 重解析必须先清掉这个文件的旧符号，否则 update() 每跑一次符号数就翻一倍。
    //   （增量索引里这是最容易埋的 bug：build() 看不出来，只有 update() 才爆发。）
    symbols_.erase(std::remove_if(symbols_.begin(), symbols_.end(),
                                  [&](const Symbol& s) { return s.file.string() == key; }),
                   symbols_.end());
    for (auto& s : syms) symbols_.push_back(std::move(s));
    stats_.symbols = symbols_.size();
}

// ---------------------------------------------------------------------------
//  依赖图：文件 A 里出现了文件 B 的符号名 → A → B
//
//  这是"文件名匹配"的近似（省掉了真正的 include/引用解析）。
//  它对 C++ 出奇地好用，因为 C++ 的文件名通常就等于主类名。
// ---------------------------------------------------------------------------
// 抓 #include "..."（只抓引号形式：尖括号是系统头，不在这个仓库里）
void RepoIndex::collectIncludes(const std::string& src, size_t idx) {
    if (rawIncludes_.size() <= idx) rawIncludes_.resize(idx + 1);
    size_t pos = 0;
    while (pos < src.size()) {
        const size_t nl = src.find('\n', pos);
        const std::string_view line =
            std::string_view(src).substr(pos, nl == std::string::npos ? nl : nl - pos);
        size_t h = line.find("#include");
        if (h != std::string_view::npos) {
            size_t q1 = line.find('"', h);
            if (q1 != std::string_view::npos) {
                size_t q2 = line.find('"', q1 + 1);
                if (q2 != std::string_view::npos)
                    rawIncludes_[idx].emplace_back(line.substr(q1 + 1, q2 - q1 - 1));
            }
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
}

void RepoIndex::buildGraph() {
    adj_.assign(files_.size(), {});

    // ---- 边①：#include（真正的依赖）--------------------------------------
    //  解析必须在**所有文件都入库之后**做：扫到 A 的时候 B 可能还没出现。
    for (size_t a = 0; a < files_.size() && a < rawIncludes_.size(); ++a) {
        const fs::path dir = files_[a].parent_path();
        for (const auto& inc : rawIncludes_[a]) {
            std::error_code ec;
            fs::path cand = (dir / inc).lexically_normal();
            auto it = fileIdx_.find(fs::absolute(cand, ec).lexically_normal().string());
            if (it == fileIdx_.end()) {
                cand = (root_ / inc).lexically_normal();
                it = fileIdx_.find(fs::absolute(cand, ec).lexically_normal().string());
            }
            if (it != fileIdx_.end() && it->second != a)
                adj_[a].push_back({it->second, 1.0});
        }
    }

    // ---- 边②：符号名匹配文件名的启发式 ------------------------------------
    //  它对"没有 include 关系、但靠命名约定互相索引"的仓库仍然有用
    //  （比如 Python 的 import 写法花样太多时）。权重低于 include 边。
    std::vector<std::string> normStem(files_.size()), normName(files_.size());
    for (size_t b = 0; b < files_.size(); ++b) {
        normStem[b] = normalizeName(files_[b].stem().string());
        normName[b] = normalizeName(files_[b].filename().string());
    }
    for (const auto& s : symbols_) {
        auto a = fileIdx_.find(s.file.string());
        if (a == fileIdx_.end() || s.name.size() < 3) continue;
        const std::string nn = normalizeName(s.name);
        if (nn.size() < 3) continue;
        for (size_t b = 0; b < files_.size(); ++b) {
            if (b == a->second) continue;
            if (normStem[b] == nn || normName[b].find(nn) != std::string::npos) {
                adj_[a->second].push_back({b, 0.5});
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  PageRank（带活动文件偏置）—— 伪码见讲义 P7
// ---------------------------------------------------------------------------
std::vector<double> RepoIndex::pagerank(const std::vector<fs::path>& activeFiles) const {
    const size_t n = files_.size();
    if (n == 0) return {};
    std::vector<double> rank(n, 1.0 / n), next(n);

    // ------------------------------------------------------------------------
    //  ★★ L8 在这里修掉了**两个**真 bug，它们都属于同一族：
    //     "代码能跑、排序看起来也对，但数字是废的"。
    //
    //  bug ① 偏置乘错了地方。
    //     原来是 `contrib = damping * rank[i] * bias[i]` —— 把偏置乘进了**转移项**。
    //     这让迭代算子不再是随机矩阵（每一行的和不再等于 1），总质量逐轮放大：
    //     实测 20 轮后所有 rank 之和从 1 涨到 **153**，单个文件打印出 153.6。
    //     它不改变相对排序，所以**没人会发现** —— 直到有人拿 rank 去设阈值。
    //
    //  bug ② 悬挂节点的质量被算了两次（均分了一次，循环后又加了一次）。
    //
    //  ★ 正确的做法（个性化 PageRank）：偏置只进**传送向量** p，不碰转移项：
    //        rank[t+1] = d · Pᵀ·rank[t]  +  (d · dangling + (1-d)) · p
    //    其中 Σp = 1。这样 Σrank[t+1] = d·(1-dangling) + d·dangling + (1-d) = 1。
    // ------------------------------------------------------------------------
    std::vector<double> p(n, 1.0);
    for (const auto& afp : activeFiles) {
        auto it = fileIdx_.find(afp.string());
        if (it != fileIdx_.end()) p[it->second] = static_cast<double>(cfg_.activeBias);
    }
    const double sumP = std::accumulate(p.begin(), p.end(), 0.0);
    for (double& v : p) v /= sumP;

    std::vector<double> outDeg(n, 0);
    for (size_t i = 0; i < n; ++i)
        for (auto& [to, w] : adj_[i]) outDeg[i] += w;

    for (int it = 0; it < cfg_.pagerankIterations; ++it) {
        std::fill(next.begin(), next.end(), 0.0);
        double dangling = 0;
        for (size_t i = 0; i < n; ++i) if (outDeg[i] == 0) dangling += rank[i];

        for (size_t i = 0; i < n; ++i) {
            if (outDeg[i] == 0) continue;                 // 悬挂节点的质量走下面的传送项
            const double contrib = cfg_.damping * rank[i];
            for (auto& [to, w] : adj_[i]) next[to] += contrib * w / outDeg[i];
        }
        const double teleport = cfg_.damping * dangling + (1.0 - cfg_.damping);
        for (size_t j = 0; j < n; ++j) next[j] += teleport * p[j];

        double delta = 0;
        for (size_t j = 0; j < n; ++j) delta += std::fabs(next[j] - rank[j]);
        rank.swap(next);
        if (delta < 1e-6) break;      // 提前收敛
    }
    return rank;
}

double RepoIndex::rankOf(const fs::path& p, const std::vector<fs::path>& activeFiles) const {
    auto it = fileIdx_.find(p.string());
    if (it == fileIdx_.end()) return 0.0;
    auto r = pagerank(activeFiles);
    return it->second < r.size() ? r[it->second] : 0.0;
}

// ---------------------------------------------------------------------------
//  渲染：把符号按"所属文件的 rank"排序，按字符预算截断
// ---------------------------------------------------------------------------
namespace {
// 逐字符预算：1 token ≈ 4 字符（英文代码的经验值，中文注释会偏，
// 但地图里几乎全是标识符，够用；见讲义 K-）
size_t budgetChars(int tokenBudget) { return size_t(tokenBudget) * 4; }
}

std::string RepoIndex::renderMap(const std::vector<fs::path>& activeFiles) const {
    auto rank = pagerank(activeFiles);
    if (rank.empty()) return {};

    std::vector<size_t> order(symbols_.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        auto ia = fileIdx_.find(symbols_[a].file.string());
        auto ib = fileIdx_.find(symbols_[b].file.string());
        double ra = ia == fileIdx_.end() ? 0 : rank[ia->second];
        double rb = ib == fileIdx_.end() ? 0 : rank[ib->second];
        return ra > rb;
    });

    const size_t maxChars = budgetChars(cfg_.tokenBudget);
    std::ostringstream os;
    size_t used = 0;
    std::string currentFile;
    for (size_t idx : order) {
        const auto& s = symbols_[idx];
        if (s.file.string() != currentFile) {
            currentFile = s.file.string();
            std::error_code ec;
            const std::string rel = fs::relative(s.file, root_, ec).generic_string();
            os << "\n" << rel << ":\n";
            used += rel.size() + 3;
        }
        // 签名过长要截断，否则 1024 token 装不下几个
        std::string sig = s.signature.size() > 100 ? s.signature.substr(0, 97) + "..."
                                                   : s.signature;
        const size_t lineChars = 2 + s.kind.size() + 1 + sig.size() + 1;
        if (used + lineChars > maxChars) break;      // 按预算截断，移除排名最低的
        os << "  " << s.kind << " " << sig << "\n";
        used += lineChars;
    }
    return os.str();
}

// 极简骨架：只给 kind + 全限定名，不给签名。预算吃紧时的第二档。
std::string RepoIndex::renderSkeleton(const std::vector<fs::path>& activeFiles) const {
    auto rank = pagerank(activeFiles);
    if (rank.empty()) return {};

    std::vector<size_t> order(symbols_.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        auto ia = fileIdx_.find(symbols_[a].file.string());
        auto ib = fileIdx_.find(symbols_[b].file.string());
        double ra = ia == fileIdx_.end() ? 0 : rank[ia->second];
        double rb = ib == fileIdx_.end() ? 0 : rank[ib->second];
        return ra > rb;
    });

    const size_t maxChars = budgetChars(cfg_.tokenBudget);
    std::ostringstream os;
    size_t used = 0;
    std::string currentFile;
    for (size_t idx : order) {
        const auto& s = symbols_[idx];
        if (s.file.string() != currentFile) {
            currentFile = s.file.string();
            std::error_code ec;
            const std::string rel = fs::relative(s.file, root_, ec).generic_string();
            os << "\n" << rel << ":\n";
            used += rel.size() + 3;
        }
        const size_t lineChars = 2 + s.kind.size() + 1 + s.qualName.size() + 1;
        if (used + lineChars > maxChars) break;
        os << "  " << s.kind << " " << s.qualName << "\n";
        used += lineChars;
    }
    return os.str();
}

std::string RepoIndex::debugDump() const {
    std::ostringstream os;
    os << "files: " << stats_.files << "  symbols: " << stats_.symbols
       << "  backend: " << (stats_.usedTreeSitter ? "tree-sitter" : "lexical")
       << "  buildMs: " << (int64_t)stats_.buildMs
       << "  skipped(ignored/tooBig/noGrammar): " << stats_.skippedIgnored << "/"
       << stats_.skippedTooBig << "/" << stats_.skippedNoGrammar << "\n";
    os << renderMap({});
    return os.str();
}

}  // namespace af
