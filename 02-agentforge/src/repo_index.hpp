// ---------------------------------------------------------------------------
//  L7-L8 · repo_index.hpp —— 仓库理解与索引（模块 ③ / W1 上下文闭环的检索端）
//
//  第一性需求：在有限 token 预算内，把"最相关的代码片段"放进上下文。
//  本讲实现一个**轻量符号索引**（不接 tree-sitter 也能用）：
//
//    · 扫描仓库里的源码文件（.h/.hpp/.c/.cpp/.cc）
//    · 词法级抽取：函数签名 / 类/结构体/枚举 / namespace / #include 依赖
//    · 依据用户输入的关键词（符号名、文件名）检索命中片段
//    · 产出仓库地图（RepoMap），供 L2 层做"符号骨架"注入
//
//  ★ 设计原则：索引是"可重建的缓存"，不持有真相。
//    文件变更后重新 build 即可；增量更新（mtime 差量）留到需要时再加。
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace af {

struct Symbol {
    std::string name;
    std::string kind;          // "func" | "type" | "namespace"
    int         line = 1;      // 1-based
};

struct RepoFile {
    std::string path;          // 相对 root 的路径
    std::vector<Symbol> symbols;
    std::vector<std::string> includes;   // 依赖（#include 目标）
};

struct CodeChunk {
    std::string path;
    int         startLine = 0;
    int         endLine = 0;
    std::string content;       // 带行号的片段文本
    double      score = 0.0;
};

class RepoIndex {
public:
    // 扫描 root 下所有源码文件建索引；无文件返回 false。
    bool build(const std::string& root);

    // 检索：把 query 拆词后按 符号名/文件名 打分，返回最相关的片段。
    std::vector<CodeChunk> retrieve(std::string_view query,
                                    int maxChunks = 5,
                                    int maxLines   = 60) const;

    // 仓库地图：每个文件列出前几个符号（给 L2 层注入用，限长）。
    std::string repoMap(std::size_t maxChars = 3000) const;

    // 依赖图边："a.cpp -> b.hpp"（含 include 的所有边）
    std::vector<std::string> dependencyEdges() const;

    std::string stats() const;

    const std::vector<RepoFile>& files() const noexcept { return files_; }
    const std::string& root() const noexcept { return root_; }
    bool empty() const noexcept { return files_.empty(); }

private:
    std::string root_;
    std::vector<RepoFile> files_;
};

// 带行号读取文件的一段（startLine/endLine 为 1-based，闭区间）
std::string readLinesWithNumbers(const std::string& absPath, int startLine, int endLine);

}  // namespace af
