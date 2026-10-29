// ---------------------------------------------------------------------------
//  plugin.cpp —— 动态加载 C ABI 插件
//
//  加载时必须先校验 abiVersion，不匹配直接拒绝 ——
//  否则老插件加载进来会以未定义行为崩掉整个进程。
// ---------------------------------------------------------------------------
#include "plugin.hpp"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace af {

#ifdef _WIN32
// ---------------------------------------------------------------------------
//  UTF-8 → UTF-16
//
//  ★ 为什么 LoadLibrary 这一行不能用 A 版本：
//    路径是"外部字符串"，它**一定可能含中文**（本课程自己的目录就叫
//    `11-逐讲/L4-v1能读`）。LoadLibraryA 会把 UTF-8 字节按当前 ANSI 代码页
//    （中文机 = GBK）解释 → 文件找不到，而错误码只说 "找不到指定模块"，
//    完全指不到"编码"这个根因。
//
//    规则：Windows 上凡是"内核要用的字符串"（路径、环境变量、命令行），
//    一律转成 UTF-16 后走 W 版本 API。
// ---------------------------------------------------------------------------
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
#endif

PluginHandle::PluginHandle(PluginHandle&& o) noexcept
    : lib_(o.lib_), plugin_(o.plugin_) {
    o.lib_ = nullptr; o.plugin_ = nullptr;
}

PluginHandle& PluginHandle::operator=(PluginHandle&& o) noexcept {
    if (this != &o) {
        this->~PluginHandle();
        lib_ = o.lib_; plugin_ = o.plugin_;
        o.lib_ = nullptr; o.plugin_ = nullptr;
    }
    return *this;
}

PluginHandle::~PluginHandle() {
    if (!lib_) return;
#ifdef _WIN32
    FreeLibrary(lib_);
#else
    dlclose(lib_);
#endif
}

std::optional<PluginHandle> PluginHandle::load(const std::string& path, std::string& err) {
    PluginHandle h;
#ifdef _WIN32
    h.lib_ = LoadLibraryW(utf8ToWide(path).c_str());
    if (!h.lib_) { err = "LoadLibrary 失败: " + std::to_string(GetLastError()); return std::nullopt; }
    auto fn = reinterpret_cast<AFPluginEntryFn>(GetProcAddress(h.lib_, AF_PLUGIN_ENTRY_NAME));
#else
    h.lib_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h.lib_) { err = std::string("dlopen 失败: ") + (dlerror() ? dlerror() : ""); return std::nullopt; }
    auto fn = reinterpret_cast<AFPluginEntryFn>(dlsym(h.lib_, AF_PLUGIN_ENTRY_NAME));
#endif

    if (!fn) { err = "未找到导出符号 " AF_PLUGIN_ENTRY_NAME; return std::nullopt; }

    const AFPlugin* p = fn();
    if (!p) { err = "af_plugin_entry() 返回空"; return std::nullopt; }
    if (p->abiVersion != AF_PLUGIN_ABI_VERSION) {
        err = "ABI 版本不匹配: 插件=" + std::to_string(p->abiVersion) +
              " 宿主=" + std::to_string(AF_PLUGIN_ABI_VERSION);
        return std::nullopt;
    }
    h.plugin_ = p;

    if (p->init) {
        if (p->init("{}") != 0) { err = "插件 init 失败"; return std::nullopt; }
    }
    return h;
}

std::vector<const AFToolPlugin*> PluginHandle::tools() const {
    std::vector<const AFToolPlugin*> out;
    if (!plugin_ || !plugin_->tools) return out;
    size_t n = 0;
    const AFToolPlugin* const* arr = plugin_->tools(&n);
    for (size_t i = 0; i < n; ++i) out.push_back(arr[i]);
    return out;
}

}  // namespace af
