// ---------------------------------------------------------------------------
//  symbol_extract.hpp —— 从源码里抽符号（模块 ③ 的最底层）
//
//  ★ 为什么这个组件在 L8 才出现，而不是 L6 就写？
//
//    L6 之前只有一个调用方（RepoIndex），所以词法占位写在 repo_index.cpp 的
//    匿名 namespace 里是对的 —— 没有第二个调用方，就不需要名字。
//
//    L8 本来该出现第二个调用方：ContextAssembler 的降级链
//    （extractSignatures 目前是"保留看起来像声明的行"的占位，注释里写着
//     "生产实现应接 tree-sitter"，与 repo_index.cpp 里那句一模一样）。
//
//    ★ 但这条线**没接上**，原因是接口缺字段：`Message` 没有 path，
//      路径沿 `Turn::retrieved → fitTo → extractSignatures` 传不下来，
//      而 tree-sitter 要按扩展名选文法。见 `_推进状态.md` 的 I-20。
//
//    ★ 那这个文件为什么还要独立出来？两个理由，都不是"为了抽象而抽象"：
//      ① 抽取有**两个真实现**（tree-sitter / 词法），并且要能被单独对比与替换；
//      ② 它是本讲唯一需要 `#ifdef` 的地方 —— 把可选依赖关进一个编译单元，
//         其余 20 个文件一行都不用知道 tree-sitter 在不在。
//
//  ★ 依赖方向（无环）：
//      symbol_extract  ←  repo_index      （要 vector<Symbol>）
//      symbol_extract  ←  context_budget  （计划中要 renderSignatures 的 string，见 I-20）
//    context_budget 届时拿到的是**字符串**不是 Symbol，所以模块 ④ 不依赖模块 ③。
//
//  ★ 为什么不用端口（ISymbolExtractor）？
//    端口是给"模块边界之外的第二个实现者"用的（IModelProvider / ISandbox ...）。
//    符号抽取没有外部实现者，它只是"有/没有 tree-sitter 两种编译形态"，
//    所以这里用【运行时选择 + 编译期兜底】，不用多态。
// ---------------------------------------------------------------------------
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace af {

struct Symbol {
    fs::path file;
    int line = 0;                 // 1-based
    std::string kind;             // class / struct / enum / function / method
    std::string name;             // 末段名：建图时用它匹配文件名
    std::string qualName;         // 全限定名：展示用（ns::Foo::bar）
    std::string signature;        // 紧凑签名，不含 kind
};

// Auto = 有 tree-sitter 就用，没有就用词法
enum class SymbolBackend { Auto, Lexical, TreeSitter };

struct ExtractOptions {
    SymbolBackend backend = SymbolBackend::Auto;
    // 单文件上限。超过就跳过解析 —— 否则一个 25MB 的生成文件能把整库索引拖垮
    // （本课程自己的 thirdparty/tree-sitter 里就有这么一个 parser.c）。
    size_t maxBytes = 1u << 20;   // 1 MiB
    // 单文件解析超时（微秒）。病态文件不至于卡死整个索引。
    uint64_t timeoutMicros = 2'000'000;
};

// 抽出符号。文件过大 / 解析失败都返回空 —— 失败是返回值，不是异常（CR-2）。
std::vector<Symbol> extractSymbols(const fs::path& file, std::string_view src,
                                   const ExtractOptions& opt = {});

// 降级链用：把一段源码压成"签名清单"文本（模块 ④ 消费的形态）
std::string renderSignatures(std::string_view src, const fs::path& file = {},
                             const ExtractOptions& opt = {});

// tree-sitter 有没有被编进来（编译期决定）
constexpr bool kTreeSitterCompiled =
#ifdef AF_HAVE_TREESITTER
    true;
#else
    false;
#endif

bool treeSitterAvailable() noexcept;
const char* backendName(SymbolBackend b) noexcept;
SymbolBackend backendFromName(std::string_view s) noexcept;

// 语言分派：登记了文法的扩展名走 tree-sitter，没登记的走词法。
// 接一门新语言 = 下载它的 parser.c + 在这里加几行 —— 其余逻辑一行不动。
bool grammarRegistered(std::string_view ext) noexcept;

}  // namespace af
