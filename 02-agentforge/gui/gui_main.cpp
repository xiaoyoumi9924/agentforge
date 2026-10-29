// ---------------------------------------------------------------------------
//  L11 · gui_main.cpp —— AgentForgeGUI 入口
//
//  AF_GUI_SMOKE=1 时（配合 QT_QPA_PLATFORM=offscreen）：
//    启动 1.5 秒后打印 "GUI SMOKE OK" 并退出 —— 供 verify_L11.ps1 做无头验证。
//
//  AF_GUI_CRASHLOG=1 时：捕获未处理异常，打印崩溃地址与所在模块（排查用）。
// ---------------------------------------------------------------------------
#include <QApplication>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

#include "main_window.hpp"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#  pragma comment(lib, "psapi.lib")

namespace {

void printCrash(void* addr) {
    const auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "CRASH addr=%p (self+0x%llx)\n", addr,
                 static_cast<unsigned long long>(reinterpret_cast<unsigned char*>(addr) - base));

    HMODULE mods[512];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const DWORD n = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < n; ++i) {
            IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mods[i]);
            IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                reinterpret_cast<unsigned char*>(mods[i]) + dos->e_lfanew);
            const auto b = reinterpret_cast<unsigned char*>(mods[i]);
            if (addr >= b + nt->OptionalHeader.BaseOfCode &&
                addr < b + nt->OptionalHeader.BaseOfCode + nt->OptionalHeader.SizeOfCode) {
                wchar_t name[MAX_PATH] = {0};
                GetModuleFileNameW(mods[i], name, MAX_PATH);
                std::fwprintf(stderr, L"  module: %ls  offset=0x%llx\n", name,
                              static_cast<unsigned long long>(reinterpret_cast<unsigned char*>(addr) - b));
            }
        }
    }
    std::fflush(stderr);
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "EXCEPTION code=0x%lx\n",
                 static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode));
    printCrash(info->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    if (std::getenv("AF_GUI_CRASHLOG"))
        SetUnhandledExceptionFilter(&crashFilter);
#endif
    QApplication app(argc, argv);
    app.setApplicationName("AgentForgeGUI");

    af::gui::MainWindow w;
    w.show();

    if (std::getenv("AF_GUI_SMOKE")) {
        QTimer::singleShot(1500, &app, [&w]() {
            w.smokeDump();
            std::printf("GUI SMOKE OK\n");
            std::fflush(stdout);
            QApplication::quit();
        });
    }
    return app.exec();
}
