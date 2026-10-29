// ---------------------------------------------------------------------------
//  plugin.hpp —— IPlugin / IToolPlugin 的 C ABI 定义
//
//  为什么必须是纯 C 接口：
//    C++ 的 name mangling、异常传播、标准库内存布局在不同编译器/版本间
//    都不兼容。插件用 .so/.dylib/.dll 动态加载，跨模块传 C++ 类型是未定义行为。
//
//  三条铁律：
//    1. extern "C"       —— 关掉 mangling，符号名可预测
//    2. 不跨边界抛异常   —— 用 errOut 参数传递错误
//    3. 谁分配谁释放     —— 插件提供 free_string，宿主调用它
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AF_PLUGIN_ABI_VERSION 1u

// 非拥有的字符串视图，避免为了传字符串分配内存
typedef struct AFString {
    const char* data;
    size_t size;
} AFString;

// 风险等级：0=read 1=write 2=exec 3=network
typedef enum AFRisk {
    AF_RISK_READ = 0, AF_RISK_WRITE = 1, AF_RISK_EXEC = 2, AF_RISK_NETWORK = 3
} AFRisk;

// ---------------------------------------------------------------------------
//  IToolPlugin —— 一个可被模型调用的工具
// ---------------------------------------------------------------------------
typedef struct AFToolPlugin {
    uint32_t abiVersion;

    const char* (*name)(void);
    const char* (*description)(void);          // 给模型看的说明
    const char* (*inputSchemaJson)(void);      // JSON Schema 字符串

    AFRisk (*riskLevel)(void);
    uint32_t (*timeoutMs)(void);
    // 超过此字符数落盘，只留预览。Read 工具应返回 SIZE_MAX 豁免，
    // 否则会形成"读文件→落盘→再读那个文件"的循环依赖。
    uint64_t (*persistThresholdChars)(void);

    // 执行。成功返回 JSON 字符串（结果由 free_string 释放）；
    // 失败返回 NULL 并设置 *errOut（同样由 free_string 释放）。
    // 绝不跨边界抛异常。
    char* (*execute)(const char* argsJson, char** errOut);

    void (*freeString)(char*);
} AFToolPlugin;

// ---------------------------------------------------------------------------
//  IPlugin —— 插件包，可贡献多个工具
// ---------------------------------------------------------------------------
typedef struct AFPlugin {
    uint32_t abiVersion;
    const char* (*id)(void);
    const char* (*version)(void);

    int (*init)(const char* configJson);   // 返回 0 成功
    void (*shutdown)(void);

    // 返回工具指针数组（静态存储期），count 输出数量
    const AFToolPlugin* const* (*tools)(size_t* count);
} AFPlugin;

// 唯一的导出符号。宿主 dlopen 后 dlsym 找它。
// 加载时必须先校验 abiVersion，不匹配直接拒绝加载。
typedef const AFPlugin* (*AFPluginEntryFn)(void);

#define AF_PLUGIN_ENTRY_NAME "af_plugin_entry"

#ifdef __cplusplus
}  // extern "C"
#endif

// ---------------------------------------------------------------------------
//  宿主侧加载辅助（仅 C++ 可见）
// ---------------------------------------------------------------------------
#ifdef __cplusplus

#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
   // ★ NOMINMAX 必须在 windows.h 之前定义：
   //   windows.h 会把 min/max 定义成宏，污染所有下游包含链，
   //   导致 std::min / std::max 展开成非法表达式（MSVC C2589）。
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   using AfLibHandle = HMODULE;
#else
#  include <dlfcn.h>
   using AfLibHandle = void*;
#endif

namespace af {

class PluginHandle {
public:
    static std::optional<PluginHandle> load(const std::string& path, std::string& err);
    ~PluginHandle();

    PluginHandle(const PluginHandle&) = delete;
    PluginHandle& operator=(const PluginHandle&) = delete;
    PluginHandle(PluginHandle&&) noexcept;
    PluginHandle& operator=(PluginHandle&&) noexcept;

    const AFPlugin* plugin() const noexcept { return plugin_; }
    std::vector<const AFToolPlugin*> tools() const;

private:
    PluginHandle() = default;
    AfLibHandle lib_ = nullptr;
    const AFPlugin* plugin_ = nullptr;
};

// 插件实现一个工具的最小样板（放在插件自己的 .cpp 里）：
//
//   static const char* kSchema = R"({"type":"object",
//       "properties":{"path":{"type":"string"}},"required":["path"]})";
//
//   static const AFToolPlugin kTool = {
//       AF_PLUGIN_ABI_VERSION,
//       [](){ return "Read"; },
//       [](){ return "读取文件内容，带行号。"; },
//       [](){ return kSchema; },
//       [](){ return AF_RISK_READ; },
//       [](){ return 10000u; },
//       [](){ return uint64_t(SIZE_MAX); },        // 豁免落盘
//       [](const char* args, char** err){ ... },
//       [](char* p){ free(p); },
//   };
//
//   导出的唯一入口必须是这个符号名（AF_PLUGIN_ENTRY_NAME）：
//       extern "C" __declspec(dllexport) const AFPlugin* af_plugin_entry(void);
//   Linux/macOS 上去掉 __declspec(dllexport)，改用
//       __attribute__((visibility("default")))
//
//   完整可编译的样例见本目录 plugins/sample_plugin.cpp。

}  // namespace af
#endif  // __cplusplus
