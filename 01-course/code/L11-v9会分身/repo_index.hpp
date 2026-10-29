// ---------------------------------------------------------------------------
//  repo_index.hpp —— 仓库理解：符号表 + 依赖图 + PageRank + 预算渲染（模块 ③）
//
//  为什么用符号图而不是纯向量检索：
//    RAG 的向量相似度 ≠ 代码相关性。process_data 可能 critical 于 load_user，
//    但两者没有任何共同关键词供向量匹配；而 `foo(int)` 和 `foo(string)` 的
//    嵌入几乎重合，可它们是**两个不同的函数**。
//    代码的相关性是**结构性**的（谁定义谁、谁调用谁），不是语义相似度。
//
//  对照：Claude Code 选了 ripgrep（正则）而非向量库 —— 模型自己会构造复杂
//  正则，不需要索引。但它没有"仓库整体长什么样"的视图。
//  本课程推荐混合：repo map 常驻 + grep 按需 + 向量兜底。
//
//  ★ 本模块在上下文里占的是 L2_RepoMap 层。
//    L0–L4 五层压缩做的事都是"**压**"（内容已经进来了，想办法变小）；
//    检索/地图做的是"**选**"（在它进上下文之前就别让它进来）。
//    "选"比"压"便宜得多 —— 没进来的东西不花 token，也不需要压缩。
// ---------------------------------------------------------------------------
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "symbol_extract.hpp"

namespace fs = std::filesystem;

namespace af {

struct RepoIndexConfig {
    int tokenBudget = 1024;          // 默认地图预算（Aider 的 --map-tokens）
    std::vector<std::string> extensions{".c", ".cc", ".cpp", ".h", ".hpp",
                                        ".py", ".ts", ".js", ".go", ".rs"};
    bool respectGitignore = true;
    double damping = 0.85;
    int pagerankIterations = 20;
    // 活动文件的偏置强度（只作用于传送向量，见 pagerank() 里的说明）
    int activeBias = 8;

    // 符号抽取配置（backend / 单文件上限 / 超时），交给 symbol_extract
    ExtractOptions extract;

    // 永远跳过的目录名。.git 与构建产物在里面 —— 不设的话，
    // 对本课程自己的目录跑 --show-repo-map 会把 25MB 的 parser.c 也扫进去。
    std::vector<std::string> skipDirs{".git", "build", "out", "cmake-build-debug",
                                      "node_modules", "__pycache__", ".vs",
                                      "thirdparty"};
};

// 一次索引的实况。C-用例要断言的就是这些数字，不是"看起来跑过了"。
struct RepoIndexStats {
    size_t files = 0;
    size_t symbols = 0;
    size_t skippedIgnored = 0;
    size_t skippedTooBig = 0;
    size_t skippedNoGrammar = 0;     // 走了词法兜底的文件数
    double buildMs = 0;
    bool   usedTreeSitter = false;
};

class RepoIndex {
public:
    explicit RepoIndex(RepoIndexConfig cfg = {}) : cfg_(cfg) {}

    // 全量构建 / 增量更新（按 mtime+size 判 dirty）
    void build(fs::path root);
    void update();

    // 渲染成放进 prompt 的地图。activeFiles 会被偏置加权。
    std::string renderMap(const std::vector<fs::path>& activeFiles = {}) const;

    // ★ 只渲染"符号名清单"的极简版，给极小预算（例如 256 token）用
    std::string renderSkeleton(const std::vector<fs::path>& activeFiles = {}) const;

    const std::vector<Symbol>& symbols() const noexcept { return symbols_; }
    const std::vector<fs::path>& files() const noexcept { return files_; }
    const RepoIndexStats& stats() const noexcept { return stats_; }

    // 调试用：--show-repo-map
    std::string debugDump() const;

    // 某个文件当前的 rank（供 --show-repo-map 打印、供用例断言偏置生效）
    double rankOf(const fs::path& p, const std::vector<fs::path>& activeFiles = {}) const;

private:
    void parseFile(const fs::path& p);
    void collectIncludes(const std::string& src, size_t fileIdx);
    void buildGraph();
    std::vector<double> pagerank(const std::vector<fs::path>& activeFiles) const;
    void loadIgnoreRules();
    bool shouldSkip(const fs::path& p) const;
    bool wanted(const fs::path& p) const;

    RepoIndexConfig cfg_;
    fs::path root_;
    std::vector<Symbol> symbols_;
    std::vector<std::vector<std::pair<size_t, double>>> adj_;   // 文件索引 → (目标, 权重)
    std::unordered_map<std::string, size_t> fileIdx_;
    std::vector<fs::path> files_;
    std::unordered_map<std::string, uint64_t> fileHashes_;
    std::unordered_set<std::string> ignoreNames_;
    // 每个文件里出现的 #include "..." 原文。建图时才能解析成边 ——
    // 因为扫到 A 的时候 B 可能还没入库。
    std::vector<std::vector<std::string>> rawIncludes_;
    mutable RepoIndexStats stats_;
};

}  // namespace af
