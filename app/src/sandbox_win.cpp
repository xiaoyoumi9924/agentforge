// ---------------------------------------------------------------------------
//  L6 · sandbox_win.cpp —— 端口 ③ 的 Windows 实现（Job Object）
//
//  真正的安全边界要下沉到内核层。Windows 上最容易落地的是 **Job Object**：
//    · 强制杀进程树（KILL_ON_JOB_CLOSE，进程越权开子进程也没用）
//    · 限制内存、限制活动进程数（防 fork 炸弹）
//    · 超时强杀
//
//  ★ 诚实说明能力边界（selfCheck 必须如实报告）：
//    · Job Object 不能拒绝网络访问（要 AppContainer + Capability 才能做到）；
//    · Job Object 不能把文件系统限制到单目录（要 AppContainer/过滤驱动）。
//    所以本实现提供"进程生命周期 + 资源上限"的硬边界，
//    网络/文件系统边界由应用层权限门兜底 —— 并**如实标注为未强制**。
//
//  设计要点（与 Codex CLI 的 codex debug seatbelt/landlock 同思路）：
//    用户必须能一条命令问出"我这台机器上沙箱到底生效了没有、边界在哪"。
// ---------------------------------------------------------------------------
#include "sandbox.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace af {

namespace {

constexpr DWORD kBufSize = 4096;

std::string win32Error(DWORD code) {
    char buf[512] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, buf, sizeof(buf), nullptr);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

// 只读 .git（防模型改写历史）：这里是应用层辅助，真正的边界在 AppContainer。
class JobObjectSandbox final : public ISandbox {
public:
    explicit JobObjectSandbox(SandboxConfig cfg) : cfg_(std::move(cfg)) {}

    ExecResult exec(const std::string& command,
                    const std::string& cwd,
                    std::chrono::milliseconds timeout) override {
        ExecResult r;

        if (!cfg_.enabled) {
            r.blockedBySandbox = true;
            r.blockReason = "沙箱未启用（配置 enabled=false）";
            return r;
        }

        // ---- 创建用于接管子进程的 Job ----
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job) {
            r.blockedBySandbox = true;
            r.blockReason = "CreateJobObject 失败: " + win32Error(GetLastError());
            return r;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        info.BasicLimitInformation.ActiveProcessLimit = 16;
        info.ProcessMemoryLimit = 256ull * 1024 * 1024;   // 256 MB
        info.JobMemoryLimit     = 256ull * 1024 * 1024;
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                                 JOB_OBJECT_LIMIT_JOB_MEMORY;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));

        // ---- 管道：捕获 stdout/stderr ----
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            CloseHandle(job);
            r.blockedBySandbox = true;
            r.blockReason = "CreatePipe 失败";
            return r;
        }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = wr;
        si.hStdError  = wr;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        std::wstring wcmd = L"cmd.exe /c " + std::wstring(command.begin(), command.end());
        std::wstring wdir = cwd.empty() ? L"." : std::wstring(cwd.begin(), cwd.end());

        const BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                                       CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                       nullptr, wdir.c_str(), &si, &pi);
        CloseHandle(wr);
        if (!ok) {
            CloseHandle(rd);
            CloseHandle(job);
            r.blockedBySandbox = true;
            r.blockReason = "CreateProcess 失败: " + win32Error(GetLastError());
            return r;
        }

        // 先放进 Job 再恢复执行 —— 否则窗口期内可能逃逸
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            r.blockedBySandbox = true;
            r.blockReason = "AssignProcessToJobObject 失败: " + win32Error(GetLastError());
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(rd);
            CloseHandle(job);
            return r;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        // ---- 边读输出边等，超时整棵进程树杀掉 ----
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        bool exited = false;
        for (;;) {
            DWORD avail = 0;
            if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                char buf[kBufSize];
                DWORD got = 0;
                if (ReadFile(rd, buf, std::min<DWORD>(avail, kBufSize), &got, nullptr) && got > 0) {
                    r.stdoutOut.append(buf, got);
                    continue;
                }
            }
            if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) { exited = true; break; }
            if (std::chrono::steady_clock::now() >= deadline) {
                TerminateJobObject(job, 0xE1);   // 连子进程一起杀
                r.timedOut = true;
                break;
            }
        }

        // 排干管道里剩余的输出
        for (;;) {
            char buf[kBufSize];
            DWORD got = 0;
            if (!ReadFile(rd, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            r.stdoutOut.append(buf, got);
        }

        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        r.exitCode = int(code);
        r.blockedBySandbox = r.timedOut;

        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return r;
    }

    bool selfCheck(std::string& report) const override {
        std::string s;

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job) {
            report = "Job Object 不可用：" + win32Error(GetLastError());
            return false;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        info.BasicLimitInformation.ActiveProcessLimit = 16;
        info.ProcessMemoryLimit = 256ull * 1024 * 1024;
        info.JobMemoryLimit     = 256ull * 1024 * 1024;
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                                 JOB_OBJECT_LIMIT_JOB_MEMORY;

        const BOOL set = SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                                 &info, sizeof(info));
        if (!set) {
            report = "SetInformationJobObject 失败：" + win32Error(GetLastError());
            CloseHandle(job);
            return false;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION got{};
        DWORD gotLen = 0;
        QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &got,
                                  sizeof(got), &gotLen);
        CloseHandle(job);

        s += "沙箱实现：Windows Job Object\n";
        s += "  [OK]  进程树强杀（KILL_ON_JOB_CLOSE）：子进程被赋予的 Job 一致，越权开子进程同权\n";
        s += "  [OK]  活动进程数上限 = " + std::to_string(got.BasicLimitInformation.ActiveProcessLimit) + "\n";
        s += "  [OK]  内存上限 = " + std::to_string(got.ProcessMemoryLimit / 1024 / 1024) + " MB\n";
        s += "  [--]  网络隔离：未强制（Job Object 不提供网络拒绝能力，需 AppContainer）\n";
        s += "  [--]  文件系统单目录写限：未强制（应用层权限门兜底，非内核边界）\n";
        s += "  结论：进程生命周期与资源边界已生效；网络/文件系统边界需权限门兜底。\n";
        report = std::move(s);
        return true;
    }

    const SandboxConfig& config() const override { return cfg_; }

private:
    SandboxConfig cfg_;
};

}  // namespace

std::shared_ptr<ISandbox> createSandbox(SandboxConfig cfg) {
    return std::make_shared<JobObjectSandbox>(std::move(cfg));
}

std::string sandboxBlockedMessage(const ExecResult& r) {
    if (!r.blockedBySandbox) return {};
    if (r.timedOut)
        return "沙箱拦截：命令超时，整个进程树已被强杀。重试不会改变结果，请换方案。";
    return "沙箱拦截：" + r.blockReason + "。重试不会改变结果，请换方案。";
}

}  // namespace af

#else  // 非 Windows：课程只在 Windows 上跑，其它平台给一个诚实的中性实现

namespace af {
namespace {
class NoopSandbox final : public ISandbox {
public:
    explicit NoopSandbox(SandboxConfig cfg) : cfg_(std::move(cfg)) {}
    ExecResult exec(const std::string&, const std::string&, std::chrono::milliseconds) override {
        ExecResult r;
        r.blockedBySandbox = true;
        r.blockReason = "本平台未实现沙箱";
        return r;
    }
    bool selfCheck(std::string& report) const override {
        report = "本平台未实现沙箱";
        return false;
    }
    const SandboxConfig& config() const override { return cfg_; }
private:
    SandboxConfig cfg_;
};
}  // namespace

std::shared_ptr<ISandbox> createSandbox(SandboxConfig cfg) {
    return std::make_shared<NoopSandbox>(std::move(cfg));
}
std::string sandboxBlockedMessage(const ExecResult& r) {
    return r.blockedBySandbox ? "沙箱不可用：" + r.blockReason : std::string{};
}
}  // namespace af
#endif

