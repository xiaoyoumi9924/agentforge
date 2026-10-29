// ---------------------------------------------------------------------------
//  sample_plugin.cpp —— 一个真正可编译、可被宿主加载的 C ABI 插件
//
//  它提供一个 WordCount 工具：统计文件的 行数 / 词数 / 字符数。
//
//  ★ 这个文件存在的意义：把"插件接口必须是纯 C ABI"从一句口号变成
//    一个**能被 dlopen 起来的 .dll**。三条铁律在这里全部有具体落点：
//
//    铁律 1  extern "C"       —— 关掉 name mangling，符号名可预测
//    铁律 2  不跨边界抛异常   —— 所有错误走 *errOut；本文件里没有一处 throw
//    铁律 3  谁分配谁释放     —— 返回的内存由插件的 freeString 释放
//
//  ★ 另一个值得注意的点：**这个插件没有包含任何宿主代码**。
//    它只用 C 标准库。这正是 C ABI 桥的价值 ——
//    插件作者不需要和宿主用同一个 JSON 库、同一个编译器版本、同一套 STL。
//
//  构建：.\plugins\build_plugin.ps1
// ---------------------------------------------------------------------------
#include "../plugin.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
//  跨边界安全的字符串分配：一律 malloc，一律由 freeString 释放
// ---------------------------------------------------------------------------
static char* dupe(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return p;
}

// ---------------------------------------------------------------------------
//  极简 JSON 取值：只找第一个 "key" : "value"
//
//  为什么不用宿主那套 nlohmann/json：
//    那正是插件不该有的依赖。ABI 边界只传 char*，
//    插件内部想怎么实现都行 —— 这就是解耦的意义。
// ---------------------------------------------------------------------------
static std::string jsonStringField(const char* jsonText, const char* key) {
    if (!jsonText || !key) return {};
    const std::string s(jsonText);
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t p = s.find(needle);
    if (p == std::string::npos) return {};
    p = s.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = s.find('"', p);
    if (p == std::string::npos) return {};
    const std::size_t q = s.find('"', p + 1);
    if (q == std::string::npos) return {};
    return s.substr(p + 1, q - p - 1);
}

// ---------------------------------------------------------------------------
//  工具实现
// ---------------------------------------------------------------------------
static char* wordcount_execute(const char* argsJson, char** errOut) {
    if (errOut) *errOut = nullptr;

    const std::string path = jsonStringField(argsJson, "path");
    if (path.empty()) {
        if (errOut) *errOut = dupe("缺少必需字段 `path`");
        return nullptr;                       // ★ 失败返回 NULL + errOut，不是抛异常
    }

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (errOut) *errOut = dupe("无法打开文件: " + path);
        return nullptr;
    }

    std::size_t lines = 0, words = 0, chars = 0;
    bool inWord = false;
    int c;
    while ((c = std::fgetc(f)) != EOF) {
        ++chars;
        if (c == '\n') ++lines;
        const bool space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (space) {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            ++words;
        }
    }
    std::fclose(f);
    if (chars > 0 && lines == 0) ++lines;      // 最后一行没有换行符

    std::string out = "文件 " + path + "：\n"
                      "  行数   " + std::to_string(lines) + "\n"
                      "  词数   " + std::to_string(words) + "\n"
                      "  字符数 " + std::to_string(chars) + "\n";
    return dupe(out);                          // ★ 交给宿主的 freeString 释放
}

// ---------------------------------------------------------------------------
//  工具描述（静态存储期 —— 宿主只持有指针，不复制）
// ---------------------------------------------------------------------------
static const char* kName        = "WordCount";
static const char* kDescription = "统计文本文件的行数、词数与字符数。只读，安全。";
static const char* kSchema =
    R"({"type":"object","properties":{"path":{"type":"string",)"
    R"("description":"要统计的文件路径"}},"required":["path"]})";

static const AFToolPlugin kWordCountTool = {
    AF_PLUGIN_ABI_VERSION,
    []() -> const char* { return kName; },
    []() -> const char* { return kDescription; },
    []() -> const char* { return kSchema; },
    []() -> AFRisk { return AF_RISK_READ; },
    []() -> uint32_t { return 10000u; },
    []() -> uint64_t { return ~uint64_t(0); },   // 豁免落盘
    wordcount_execute,
    [](char* p) { std::free(p); },               // ★ freeString
};

// ---------------------------------------------------------------------------
//  IPlugin：一个插件包可以贡献多个工具
// ---------------------------------------------------------------------------
static const AFToolPlugin* kToolTable[] = {&kWordCountTool};

static const char* plugin_id(void)      { return "sample-plugin"; }
static const char* plugin_version(void) { return "1.0.0"; }
static int  plugin_init(const char*)    { return 0; }   // 0 = 成功
static void plugin_shutdown(void)       {}

static const AFToolPlugin* const* plugin_tools(std::size_t* count) {
    *count = sizeof(kToolTable) / sizeof(kToolTable[0]);
    return kToolTable;
}

static const AFPlugin kPlugin = {
    AF_PLUGIN_ABI_VERSION,
    plugin_id,
    plugin_version,
    plugin_init,
    plugin_shutdown,
    plugin_tools,
};

// ---------------------------------------------------------------------------
//  唯一的导出符号
// ---------------------------------------------------------------------------
extern "C" {

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const AFPlugin* af_plugin_entry(void) {
    return &kPlugin;
}

}  // extern "C"
