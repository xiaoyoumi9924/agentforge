// ---------------------------------------------------------------------------
//  symbol_extract.cpp —— 树-sitter 真抽取 + 词法兜底
//
//  编译形态：
//    定义了 AF_HAVE_TREESITTER → 链接 tree-sitter 与 C++ 文法，走真解析器
//    没定义                   → 只有词法兜底，照样能跑、能出地图（质量差一些）
//
//  运行时仍可用 ExtractOptions::backend 强制走兜底 —— 这让"真解析器到底强在哪"
//  变成可自动对比的 A/B，而不是口头宣称。
// ---------------------------------------------------------------------------
#include "symbol_extract.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <utility>

#ifdef AF_HAVE_TREESITTER
#include <tree_sitter/api.h>
// 文法入口：每个语言仓库导出一个 tree_sitter_xxx()
extern "C" const TSLanguage* tree_sitter_cpp(void);
#endif

namespace af {
namespace {

// ---------------------------------------------------------------------------
//  通用小工具
// ---------------------------------------------------------------------------

// 把任意空白压成一个空格。签名里出现换行会让 token 数和可读性都失控。
std::string squeeze(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isspace(u)) { pendingSpace = true; continue; }
        if (pendingSpace && !out.empty()) { out.push_back(' '); pendingSpace = false; }
        out.push_back(c);
    }
    return out;
}

bool eq(const char* a, const char* b) noexcept { return std::strcmp(a, b) == 0; }

std::string firstWord(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    size_t j = i;
    while (j < s.size() && !std::isspace((unsigned char)s[j]) &&
           s[j] != '(' && s[j] != '{' && s[j] != ':') ++j;
    return std::string(s.substr(i, j - i));
}

std::string lastWord(std::string_view s) {
    size_t e = s.find_last_not_of(" \t*&:<>,");
    if (e == std::string_view::npos) return {};
    s = s.substr(0, e + 1);
    size_t b = s.find_last_of(" \t*&:<>,");
    return std::string(b == std::string_view::npos ? s : s.substr(b + 1));
}

// ---------------------------------------------------------------------------
//  词法兜底：识别"看起来像声明"的行
//
//  它会在这些地方漏：模板函数、命名空间内的方法、返回类型在上一行的声明、
//  构造函数、operator 重载。留着它就是为了能在验收里量出这些漏。
// ---------------------------------------------------------------------------
std::vector<Symbol> extractLexical(const fs::path& file, std::string_view src) {
    std::vector<Symbol> out;
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= src.size()) {
        size_t nl = src.find('\n', pos);
        std::string_view line =
            src.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
        ++lineNo;

        // 关键字出现在行首，或前面是空格（避免把 "myclass foo" 判成类）
        auto kwAt = [&](std::string_view kw) -> size_t {
            if (line.starts_with(kw)) return 0;
            size_t p = line.find(kw);
            if (p != std::string_view::npos && p > 0 && line[p - 1] == ' ') return p;
            return std::string_view::npos;
        };

        std::string kind;
        size_t kwPos = std::string_view::npos, kwLen = 0;
        size_t p1 = kwAt("class "), p2 = kwAt("struct ");
        if (p1 != std::string_view::npos) { kind = "class"; kwPos = p1; kwLen = 6; }
        else if (p2 != std::string_view::npos) { kind = "class"; kwPos = p2; kwLen = 7; }
        else if (kwAt("def ") != std::string_view::npos) { kind = "function"; }
        else if (line.find('(') != std::string_view::npos &&
                 (line.find("void ") != std::string_view::npos ||
                  line.find("int ") != std::string_view::npos ||
                  line.find("auto ") != std::string_view::npos ||
                  line.find("std::") != std::string_view::npos ||
                  line.find("static ") != std::string_view::npos)) kind = "function";

        if (!kind.empty()) {
            std::string sig = squeeze(std::string(line));
            // 行尾的 '{' / ';' 不是签名的一部分。留着它，名字会取成
            // "AudioComp00 {" —— A/B 对照里就多出一堆假"误报"。
            while (!sig.empty() && (sig.back() == '{' || sig.back() == ';' ||
                                    sig.back() == ' ')) sig.pop_back();

            std::string name;
            if (kind == "class" && kwPos != std::string_view::npos) {
                name = firstWord(line.substr(kwPos + kwLen));
            } else {
                const size_t lp = line.find('(');
                std::string_view head =
                    line.substr(0, lp == std::string_view::npos ? line.size() : lp);
                name = lastWord(head);
            }
            if (name.empty()) name = sig;

            Symbol s;
            s.file = file;
            s.line = lineNo;
            s.kind = kind;
            s.signature = sig;
            s.name = name;
            s.qualName = name;
            out.push_back(std::move(s));
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return out;
}

#ifdef AF_HAVE_TREESITTER

// ---------------------------------------------------------------------------
//  tree-sitter 部分
// ---------------------------------------------------------------------------

std::string nodeText(const std::string& src, TSNode n) {
    uint32_t a = ts_node_start_byte(n);
    uint32_t b = ts_node_end_byte(n);
    if (b > src.size()) b = static_cast<uint32_t>(src.size());
    if (b <= a) return {};
    return src.substr(a, b - a);
}

bool isNameType(const char* t) noexcept {
    return eq(t, "identifier") || eq(t, "field_identifier") ||
           eq(t, "type_identifier") || eq(t, "operator_name") ||
           eq(t, "destructor_name") || eq(t, "namespace_identifier");
}

// ★★ 空节点守卫（本讲最贵的一个 bug，见文件末尾的 K 说明）
//
//  ts_node_type(NULL) 会解引用 self.tree->language → 段错误 0xC0000005。
//  这条契约写在 node.c 的源码里，**没有**写在 api.h 的注释里。
//  实测触发场景：嵌套 struct / enum 内部的成员声明没有 declarator 字段，
//  ts_node_child_by_field_name 返回空节点，然后被喂进 ts_node_type。
//  症状是"某些文件一解析就整个进程消失"，没有任何日志 —— 定位成本极高。
//
//  ★ 所以：**每一个接受 TSNode 的递归函数，第一行必须是判空。**
//    这不是防御性编程，是这条 API 的前置条件。
static bool nullNode(TSNode n) { return ts_node_is_null(n); }

// 末段名：ns::Foo::bar → bar。建图时用它去匹配文件名。
std::string leafName(const std::string& src, TSNode n, int depth = 0) {
    if (nullNode(n) || depth > 32) return {};
    const char* t = ts_node_type(n);
    if (isNameType(t)) return squeeze(nodeText(src, n));

    // qualified_identifier 的 "name" 字段才是内层；其余（pointer/reference/
    // function/parenthesized declarator）都挂 "declarator"。
    TSNode d;
    if (eq(t, "qualified_identifier")) {
        d = ts_node_child_by_field_name(n, "name", 4);
    } else {
        d = ts_node_child_by_field_name(n, "declarator", 10);
        if (ts_node_is_null(d)) d = ts_node_child_by_field_name(n, "name", 4);
    }
    if (!ts_node_is_null(d)) return leafName(src, d, depth + 1);

    uint32_t c = ts_node_named_child_count(n);
    if (c > 0) return leafName(src, ts_node_named_child(n, 0), depth + 1);
    return squeeze(nodeText(src, n));
}

// 沿 declarator 链找到真正的 function_declarator（可能隔着指针/引用/括号）
bool findFnDeclarator(TSNode n, TSNode& out, int depth = 0) {
    if (nullNode(n) || depth > 32) return false;
    if (eq(ts_node_type(n), "function_declarator")) { out = n; return true; }
    TSNode d = ts_node_child_by_field_name(n, "declarator", 10);
    if (ts_node_is_null(d)) return false;
    return findFnDeclarator(d, out, depth + 1);
}

// 沿 declarator 链找到"名字节点"：identifier / qualified_identifier / destructor_name
bool findNameNode(TSNode n, TSNode& out, int depth = 0) {
    if (nullNode(n) || depth > 32) return false;
    const char* t = ts_node_type(n);
    if (isNameType(t) || eq(t, "qualified_identifier")) { out = n; return true; }
    TSNode d = ts_node_child_by_field_name(n, "declarator", 10);
    if (ts_node_is_null(d)) return false;
    return findNameNode(d, out, depth + 1);
}

// 命名约定启发式：C++ 里类名基本都大写开头。
// ★ 判错了的代价只是把 function 写成 method，不影响地图可用性；
//   真判准需要跨文件的符号表（那是另一讲的事）。明说是启发式，别装作精确。
bool looksLikeClassName(std::string_view s) {
    return !s.empty() && s.front() >= 'A' && s.front() <= 'Z';
}

// 不用 TSNode{} 当空值 —— ts_node_is_null 的判定细节不该被依赖。
bool childByType(TSNode n, const char* type, TSNode& out) {
    uint32_t c = ts_node_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
        TSNode ch = ts_node_child(n, i);
        if (eq(ts_node_type(ch), type)) { out = ch; return true; }
    }
    return false;
}

// 解析器池：ts_parser_new/set_language 不便宜，200 个文件不能建 200 个。
// 函数内 static → 进程退出时析构，不用管释放顺序。
class ParserPool {
public:
    ~ParserPool() { for (auto& kv : pool_) ts_parser_delete(kv.second); }

    TSParser* get(const TSLanguage* lang, uint64_t timeoutMicros) {
        auto it = pool_.find(lang);
        if (it != pool_.end()) return it->second;
        TSParser* p = ts_parser_new();
        if (!ts_parser_set_language(p, lang)) {   // 文法版本不匹配会失败
            ts_parser_delete(p);
            return nullptr;
        }
        ts_parser_set_timeout_micros(p, timeoutMicros);
        pool_.emplace(lang, p);
        return p;
    }
private:
    std::unordered_map<const TSLanguage*, TSParser*> pool_;
};

ParserPool& parsers() { static ParserPool p; return p; }

struct Ctx {
    const std::string* src;
    const fs::path* file;
    std::vector<Symbol>* out;
    std::string scope;     // "ns::Foo::"
    bool inClass = false;
    int depth = 0;
};

void emit(Ctx& c, TSNode n, const char* kind, std::string qual, std::string sig) {
    Symbol s;
    s.file = *c.file;
    s.line = static_cast<int>(ts_node_start_point(n).row) + 1;
    s.kind = kind;
    s.qualName = std::move(qual);
    s.signature = std::move(sig);
    s.name = s.qualName;
    size_t cut = s.name.rfind("::");
    if (cut != std::string::npos) s.name = s.name.substr(cut + 2);
    c.out->push_back(std::move(s));
}

// 函数/方法：返回类型 + 全限定名 + 参数列表
void emitFunction(Ctx& c, TSNode n) {
    TSNode declNode = ts_node_child_by_field_name(n, "declarator", 10);
    TSNode fd;
    if (!findFnDeclarator(declNode, fd)) return;
    TSNode nameNode;
    if (!findNameNode(fd, nameNode)) return;

    const bool qualified = eq(ts_node_type(nameNode), "qualified_identifier");
    const std::string ntext = squeeze(nodeText(*c.src, nameNode));
    const std::string last  = leafName(*c.src, nameNode);
    if (last.empty()) return;

    // ★ 限定名必须由"名字节点"决定，不是由外层 declarator 决定。
    //   写成后者会得到 app::audio::init（丢了类名）—— 因为外层是 function_declarator，
    //   而 qualified_identifier 藏在它里面。这是"树遍历里最常见的一类错"：
    //   你在错误的层级上读了文本。
    std::string qual;
    if (qualified) {
        // 源码里已带限定：只有它没以当前 scope 开头时才补，防止拼成 app::app::x
        qual = (c.scope.empty() || ntext.rfind(c.scope, 0) == 0) ? ntext : c.scope + ntext;
    } else {
        qual = c.scope + last;
    }

    const char* kind = "function";
    if (c.inClass) {
        kind = "method";
    } else if (qualified) {
        // Xxx::foo —— 取紧邻末段之前的那一段当"类名"
        const size_t cut = ntext.rfind("::");
        if (cut != std::string::npos) {
            const size_t prev = (cut == 0) ? std::string::npos : ntext.rfind("::", cut - 1);
            const size_t start = (prev == std::string::npos) ? 0 : prev + 2;
            if (looksLikeClassName(ntext.substr(start, cut - start))) kind = "method";
        }
    }

    TSNode typeNode = ts_node_child_by_field_name(n, "type", 4);
    std::string type = ts_node_is_null(typeNode) ? std::string()
                                                 : squeeze(nodeText(*c.src, typeNode));
    TSNode params = ts_node_child_by_field_name(fd, "parameters", 10);
    std::string ps = ts_node_is_null(params) ? std::string("()")
                                             : squeeze(nodeText(*c.src, params));

    std::string sig = type.empty() ? qual + ps : type + " " + qual + ps;
    emit(c, n, kind, qual, sig);
}

// 类 / 结构体 / 联合体
void emitClass(Ctx& c, TSNode n, const char* kind) {
    TSNode nameNode = ts_node_child_by_field_name(n, "name", 4);
    if (ts_node_is_null(nameNode)) return;
    std::string qual = c.scope + squeeze(nodeText(*c.src, nameNode));
    std::string sig = qual;
    TSNode base;
    if (childByType(n, "base_class_clause", base))
        sig += " " + squeeze(nodeText(*c.src, base));
    emit(c, n, kind, qual, sig);
}

void walk(Ctx& c, TSNode n) {
    if (nullNode(n) || c.depth > 200) return;  // 判空 + 防病态深嵌套
    const char* t = ts_node_type(n);

    // 函数定义：抽完就走，不进函数体（否则局部声明会污染符号表）
    if (eq(t, "function_definition")) {
        emitFunction(c, n);
        return;
    }
    // 类内声明（可能是方法，也可能是成员变量）
    if (eq(t, "field_declaration")) {
        TSNode fd;
        if (findFnDeclarator(ts_node_child_by_field_name(n, "declarator", 10), fd))
            emitFunction(c, n);
        return;
    }
    // 顶层声明（函数声明 或 变量）
    if (eq(t, "declaration")) {
        TSNode fd;
        if (findFnDeclarator(ts_node_child_by_field_name(n, "declarator", 10), fd))
            emitFunction(c, n);
        return;
    }
    if (eq(t, "class_specifier") || eq(t, "struct_specifier") || eq(t, "union_specifier")) {
        const char* kind = eq(t, "struct_specifier") ? "struct" : "class";
        emitClass(c, n, kind);

    TSNode body = ts_node_child_by_field_name(n, "body", 4);
    if (!nullNode(body)) {
        TSNode nameNode = ts_node_child_by_field_name(n, "name", 4);
        std::string saved = c.scope;
        bool savedIn = c.inClass;
        if (!nullNode(nameNode)) c.scope += squeeze(nodeText(*c.src, nameNode)) + "::";
            c.inClass = true;
            c.depth++;
            walk(c, body);
            c.depth--;
            c.scope = saved;
            c.inClass = savedIn;
        }
        return;
    }
    if (eq(t, "namespace_definition")) {
        std::string saved = c.scope;
        bool savedIn = c.inClass;
        TSNode nameNode = ts_node_child_by_field_name(n, "name", 4);
        if (!ts_node_is_null(nameNode))
            c.scope += squeeze(nodeText(*c.src, nameNode)) + "::";
        c.inClass = false;
        c.depth++;
        uint32_t cnt = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < cnt; ++i) walk(c, ts_node_named_child(n, i));
        c.depth--;
        c.scope = saved;
        c.inClass = savedIn;
        return;
    }
    if (eq(t, "enum_specifier")) {
        TSNode nameNode = ts_node_child_by_field_name(n, "name", 4);
        if (!ts_node_is_null(nameNode))
            emit(c, n, "enum", c.scope + squeeze(nodeText(*c.src, nameNode)),
                 c.scope + squeeze(nodeText(*c.src, nameNode)));
        return;
    }

    // 其余（translation_unit / template_declaration / linkage_specification /
    // preproc_ifdef ...）一律继续下钻。模板函数就藏在 template_declaration 里，
    // 不下钻就永远抽不到 —— 这是词法兜底做不到、而必须下钻的典型。
    c.depth++;
    uint32_t cnt2 = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < cnt2; ++i) walk(c, ts_node_named_child(n, i));
    c.depth--;
}

std::vector<Symbol> extractWithTs(const fs::path& file, const std::string& src,
                                  const TSLanguage* lang, const ExtractOptions& opt) {
    TSParser* p = parsers().get(lang, opt.timeoutMicros);
    if (!p) return {};

    TSTree* tree = ts_parser_parse_string(p, nullptr, src.data(),
                                          static_cast<uint32_t>(src.size()));
    if (!tree) return {};

    std::vector<Symbol> out;
    Ctx c{&src, &file, &out, std::string(), false, 0};
    walk(c, ts_tree_root_node(tree));
    ts_tree_delete(tree);
    return out;
}

// 语言分派表：登记了文法的扩展名 → 走 tree-sitter
const TSLanguage* grammarFor(std::string_view ext) {
    static const std::pair<const char*, const TSLanguage* (*)()> table[] = {
        {".c",   &tree_sitter_cpp}, {".cc",  &tree_sitter_cpp},
        {".cpp", &tree_sitter_cpp}, {".cxx", &tree_sitter_cpp},
        {".h",   &tree_sitter_cpp}, {".hh",  &tree_sitter_cpp},
        {".hpp", &tree_sitter_cpp}, {".hxx", &tree_sitter_cpp},
        {".inl", &tree_sitter_cpp}, {".ipp", &tree_sitter_cpp},
    };
    for (auto& kv : table)
        if (ext == kv.first) return kv.second();
    return nullptr;
}

#endif  // AF_HAVE_TREESITTER

}  // namespace

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------

std::vector<Symbol> extractSymbols(const fs::path& file, std::string_view src,
                                   const ExtractOptions& opt) {
    if (src.empty()) return {};
    // 超长文件直接跳过：宁可漏，也不能让索引卡在一个生成文件上。
    if (src.size() > opt.maxBytes) return {};

#ifdef AF_HAVE_TREESITTER
    if (opt.backend != SymbolBackend::Lexical) {
        const TSLanguage* lang = grammarFor(file.extension().string());
        // ★ 有文法就信它，哪怕抽到 0 个符号。
        //   反面教材：写成 "if (!s.empty()) return s;" 会让"抽到 0 个"静默退回
        //   词法 —— A/B 对照就再也量不出真解析器的提升了。
        if (lang) return extractWithTs(file, std::string(src), lang, opt);
        if (opt.backend == SymbolBackend::TreeSitter) return {};  // 明确要求但没这门语言
    }
#endif
    return extractLexical(file, src);
}

std::string renderSignatures(std::string_view src, const fs::path& file,
                             const ExtractOptions& opt) {
    auto syms = extractSymbols(file, src, opt);
    std::ostringstream os;
    for (const auto& s : syms) {
        os << s.kind << " " << s.signature;
        if (!file.empty()) os << "   @ " << file.string() << ":" << s.line;
        os << "\n";
    }
    std::string out = os.str();
    // 一个符号都抽不出来时，退回"前 2000 字符"—— 降级链最后一档不能是空。
    if (out.empty())
        return std::string(src.substr(0, std::min<size_t>(src.size(), 2000)));
    return out;
}

bool treeSitterAvailable() noexcept { return kTreeSitterCompiled; }

const char* backendName(SymbolBackend b) noexcept {
    switch (b) {
        case SymbolBackend::Auto:      return "auto";
        case SymbolBackend::Lexical:   return "lexical";
        case SymbolBackend::TreeSitter: return "treesitter";
    }
    return "auto";
}

SymbolBackend backendFromName(std::string_view s) noexcept {
    if (s == "lex" || s == "lexical") return SymbolBackend::Lexical;
    if (s == "ts" || s == "treesitter" || s == "tree-sitter")
        return kTreeSitterCompiled ? SymbolBackend::TreeSitter : SymbolBackend::Lexical;
    return SymbolBackend::Auto;
}

bool grammarRegistered(std::string_view ext) noexcept {
#ifdef AF_HAVE_TREESITTER
    return grammarFor(ext) != nullptr;
#else
    (void)ext;
    return false;
#endif
}

}  // namespace af
