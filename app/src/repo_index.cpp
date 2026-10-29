// ---------------------------------------------------------------------------
//  L7-L8 · repo_index.cpp —— 轻量符号索引 + 检索 + 仓库地图
//
//  抽取规则（词法级，不追求 100% 准确，追求"对上下文足够有用"）：
//    · #include <...> / "..."            → 依赖边
//    · class/struct/enum/namespace NAME  → 类型/命名空间符号
//    · <返回类型> NAME(...) { 或 ;       → 函数符号（含构造/析构的粗略识别）
//
//  ★ 词法级抽取的权衡：误报（把调用当声明）比漏报便宜 ——
//    误报只是把多一行签名放进检索候选，漏报会让模型看不到关键代码。
//    课程的教学重点是"检索→组装"的链路，不是语法分析的完备性。
// ---------------------------------------------------------------------------
#include "repo_index.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace af {

namespace {

bool isSourceExt(const std::string& ext) {
    static const char* kExts[] = {".h", ".hpp", ".hh", ".c", ".cc", ".cpp", ".cxx"};
    for (const char* e : kExts)
        if (ext == e) return true;
    return false;
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return std::string(s.substr(b, e - b));
}

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string identAt(std::string_view s, std::size_t i) {
    std::string out;
    while (i < s.size() && isIdentChar(s[i])) { out.push_back(s[i]); ++i; }
    return out;
}

std::size_t skipSpace(std::string_view s, std::size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

// 抽取一行里的符号（返回空表示没有）
Symbol parseLine(std::string_view line, int lineNo) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == '/' || t[0] == '*') return {};

    // class/struct/enum/namespace NAME
    {
        static const char* kTypes[] = {"class ", "struct ", "enum ", "namespace "};
        for (const char* k : kTypes) {
            if (t.compare(0, std::strlen(k), k) == 0) {
                std::string name = identAt(t, std::strlen(k));
                if (name.empty()) continue;
                Symbol s;
                s.name = name;
                s.kind = (t.compare(0, 9, "namespace") == 0) ? "namespace" : "type";
                s.line = lineNo;
                return s;
            }
        }
    }

    // FUNC(...)  形态：必须同时有 '(' 和 匹配 ')'，且以 { 或 ; 结尾（或行尾）
    const std::size_t lp = t.find('(');
    if (lp == std::string::npos) return {};
    const std::size_t rp = t.find(')', lp);
    if (rp == std::string::npos) return {};

    // 函数名 = '(' 之前最后一个标识符
    std::size_t nameEnd = lp;
    while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(t[nameEnd - 1]))) --nameEnd;
    if (nameEnd == 0) return {};
    std::size_t nameStart = nameEnd;
    while (nameStart > 0 && isIdentChar(t[nameStart - 1])) --nameStart;
    const std::string name = t.substr(nameStart, nameEnd - nameStart);
    if (name.empty()) return {};

    // 过滤明显的控制语句/调用
    static const char* kNoise[] = {"if", "for", "while", "switch", "catch", "return",
                                   "sizeof", "alignof", "decltype", "new", "delete"};
    for (const char* n : kNoise)
        if (name == n) return {};

    // 行尾判定：{ ; ) : 结尾（或行尾）才算声明/定义候选
    const std::string tail = trim(t.substr(rp + 1));
    if (!tail.empty() && tail.find('{') == std::string::npos &&
        tail.find(';') == std::string::npos &&
        tail.find(':') == std::string::npos) {
        return {};
    }

    Symbol s;
    s.name = name;
    s.kind = "func";
    s.line = lineNo;
    return s;
}

void parseFile(const std::string& absPath, RepoFile& rf) {
    std::ifstream ifs(absPath, std::ios::binary);
    if (!ifs) return;

    std::string line;
    int lineNo = 0;
    while (std::getline(ifs, line)) {
        ++lineNo;
        const std::string t = trim(line);

        // #include 依赖
        if (t.compare(0, 8, "#include") == 0) {
            std::size_t i = 8;
            while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
            if (i < t.size() && (t[i] == '<' || t[i] == '"')) {
                const char close = (t[i] == '<') ? '>' : '"';
                const std::size_t end = t.find(close, i + 1);
                if (end != std::string::npos) rf.includes.push_back(t.substr(i + 1, end - i - 1));
            }
            continue;
        }

        if (Symbol s = parseLine(t, lineNo); !s.name.empty())
            rf.symbols.push_back(std::move(s));
    }
}

std::vector<std::string> splitWords(std::string_view query) {
    std::vector<std::string> words;
    std::string cur;
    for (char ch : query) {
        if (isIdentChar(ch)) cur.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
        else if (!cur.empty()) { words.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) words.push_back(cur);
    // 去重
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    return words;
}

bool containsCI(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(hay[i + k])) !=
                std::tolower(static_cast<unsigned char>(needle[k]))) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

}  // namespace

std::string readLinesWithNumbers(const std::string& absPath, int startLine, int endLine) {
    std::ifstream ifs(absPath, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream os;
    std::string line;
    int n = 0;
    while (std::getline(ifs, line)) {
        ++n;
        if (n < startLine) continue;
        if (n > endLine) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        os << n << '\t' << line << '\n';
    }
    return os.str();
}

bool RepoIndex::build(const std::string& root) {
    root_ = root;
    files_.clear();

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            const std::string name = it->path().filename().string();
            if (name == "build" || name == ".git" || name == "thirdparty" || name == "staging")
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        if (!isSourceExt(ext)) continue;

        RepoFile rf;
        rf.path = fs::relative(it->path(), root, ec).generic_string();
        parseFile(it->path().string(), rf);
        files_.push_back(std::move(rf));
    }

    std::sort(files_.begin(), files_.end(),
              [](const RepoFile& a, const RepoFile& b) { return a.path < b.path; });
    return !files_.empty();
}

std::vector<CodeChunk> RepoIndex::retrieve(std::string_view query,
                                           int maxChunks,
                                           int maxLines) const {
    const std::vector<std::string> words = splitWords(query);
    std::vector<CodeChunk> hits;

    for (const RepoFile& rf : files_) {
        double fileScore = 0.0;
        // 文件名命中（如 "agent_loop" / "main"）
        for (const auto& w : words)
            if (containsCI(rf.path, w)) fileScore += 2.0;

        // 符号命中：为每个命中的符号产出一个围绕它的片段
        int emitted = 0;
        for (const Symbol& sym : rf.symbols) {
            double symScore = 0.0;
            for (const auto& w : words)
                if (containsCI(sym.name, w)) symScore += 3.0;
            if (symScore <= 0.0) continue;

            const std::string abs = (fs::path(root_) / rf.path).string();
            const int start = std::max(1, sym.line - 1);
            const int end   = sym.line + maxLines;
            CodeChunk c;
            c.path      = rf.path;
            c.startLine = start;
            c.endLine   = end;
            c.content   = readLinesWithNumbers(abs, start, end);
            c.score     = fileScore + symScore;
            hits.push_back(std::move(c));
            if (++emitted >= 2) break;   // 一个文件最多两个片段
        }

        // 没有任何符号命中但文件名命中：给文件头部片段
        if (fileScore > 0.0 && emitted == 0) {
            const std::string abs = (fs::path(root_) / rf.path).string();
            CodeChunk c;
            c.path      = rf.path;
            c.startLine = 1;
            c.endLine   = maxLines;
            c.content   = readLinesWithNumbers(abs, 1, maxLines);
            c.score     = fileScore;
            hits.push_back(std::move(c));
        }
    }

    std::sort(hits.begin(), hits.end(),
              [](const CodeChunk& a, const CodeChunk& b) { return a.score > b.score; });
    if (hits.size() > std::size_t(std::max(0, maxChunks))) hits.resize(std::size_t(maxChunks));
    return hits;
}

std::string RepoIndex::repoMap(std::size_t maxChars) const {
    std::ostringstream os;
    for (const RepoFile& rf : files_) {
        os << rf.path << "：";
        std::size_t n = 0;
        for (const Symbol& s : rf.symbols) {
            if (n++ >= 6) { os << " …"; break; }
            os << (n > 1 ? "、" : "") << s.kind << " " << s.name;
        }
        os << '\n';
        if (os.str().size() > maxChars) break;
    }
    std::string out = os.str();
    if (out.size() > maxChars) out.resize(maxChars);
    return out;
}

std::vector<std::string> RepoIndex::dependencyEdges() const {
    std::vector<std::string> edges;
    for (const RepoFile& rf : files_)
        for (const auto& inc : rf.includes)
            edges.push_back(rf.path + " -> " + inc);
    return edges;
}

std::string RepoIndex::stats() const {
    std::size_t syms = 0, edges = 0;
    for (const RepoFile& rf : files_) {
        syms += rf.symbols.size();
        edges += rf.includes.size();
    }
    return "索引：" + std::to_string(files_.size()) + " 个文件 / " +
           std::to_string(syms) + " 个符号 / " +
           std::to_string(edges) + " 条依赖边（root=" + root_ + "）";
}

}  // namespace af
