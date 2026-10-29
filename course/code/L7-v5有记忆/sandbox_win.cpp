// ---------------------------------------------------------------------------
//  L6 · sandbox_win.cpp —— 端口 ③ 的 Windows 实现（ADR-4）
//
//  ★ 这一节注释比代码重要 —— 它讲的是**能力边界**，而边界必须诚实。
//
//  Linux 的 Landlock + seccomp 能做到：
//      · 文件系统：只允许写 workspace，其余路径 EACCES（内核强制，绕不过）
//      · 网络：整体关闭，或按端口/AF 过滤
//      · 系统调用：危险 syscall 直接 EPERM
//
//  Windows 上**没有等价的一条命令**。本讲真正做到的，只有四件事：
//      ① 生命周期：Job Object —— 进程**树**一起生、一起死
//         （JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE）。这挡住了
//         "命令起了一堆后台子进程，杀主进程杀不干净"。
//      ② 资源：ACTIVE_PROCESS 上限（防 fork 炸弹）+ 进程内存上限
//      ③ 时间：WaitForSingleObject 超时 → TerminateJobObject，
//         **连子进程一起杀**（单个 TerminateProcess 杀不掉孙进程）
//      ④ 策略级拦截：禁网 / denyRead 的**命令文本**匹配（不是内核隔离）
//
//  ★ 没有做到的（而且**明说没做到**）：
//      文件系统写隔离、真网络隔离。这在 Windows 上需要
//      **AppContainer + 显式 ACL 改写**，几百行且要管理员权限。
//      "不假装跑通"是本课的硬规矩 —— 所以 `selfCheck()` 会把
//      "已强制 / 仅策略 / 未实现"三类逐条列出来，让人一眼看到边界。
//
//  ★ 为什么策略级拦截也值得做（虽然它绕得过）：
//      它不是安全边界，是**体验与默认值**。
//      把"默认不许联网"写进默认值，比写一份文档告诉用户"你应该禁网"可靠得多。
//      真正的安全边界另行下沉到内核 —— 这正是 ADR-4 说的事。
// ---------------------------------------------------------------------------
#include "sandbox.hpp"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace af {

namespace {

#ifdef _WIN32

// UTF-8 → UTF-16。
// ★ 又是 K-7：Windows 上凡是"内核要用的字符串"一律走宽字符 API。
//   命令、工作目录、临时文件路径全部要过这里。
std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

// 读文件 → UTF-8 字符串（非法字节过一遍 sanitizeUtf8）。
// 命令输出是**外部字节**：cmd 的输出是 GBK，不过滤的话下一轮
// json::dump() 会抛 type_error.316（L3/L4 反复踩过的那个坑）。
std::string readTextFile(const std::wstring& path) {
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs) return {};
    std::ostringstream os;
    os << ifs.rdbuf();
    return sanitizeUtf8(os.str());
}

// ---------------------------------------------------------------------------
//  策略级检查（**在启动进程之前**，命中就直接拒绝，不浪费一次进程创建）
//
//  ★ 为什么把检查放在最前面：能拦在最外面就拦在最外面。
//    启动之后再限流/再杀，代价永远更高，可观测性也更差。
// ---------------------------------------------------------------------------
struct PolicyVerdict {
    bool        blocked = false;
    std::string reason;
};

PolicyVerdict policyCheck(const std::string& cmd, const SandboxConfig& cfg) {
    // ---- 禁网 ----
    if (!cfg.allowNetwork) {
        static const char* kNetMarkers[] = {
            "curl", "wget", "Invoke-WebRequest", "iwr ", "start-process",
            " ncat", "nc ", "ssh ", "scp ", "ftp ", "telnet",
            "http://", "https://",
        };
        for (const char* m : kNetMarkers) {
            if (cmd.find(m) != std::string::npos)
                return {true, std::string("网络已禁用（命令命中 `") + m + "`）"};
        }
    }

    // ---- 禁止读取的路径模式（~/.ssh、*.env、凭据……）----
    for (const auto& p : cfg.denyRead) {
        if (!p.empty() && cmd.find(p) != std::string::npos)
            return {true, "命令命中了禁止访问的路径模式: " + p};
    }

    return {false, {}};
}

// ---------------------------------------------------------------------------
//  Windows 实现：Job Object + 临时文件接管 stdout/stderr
//
//  ★ 为什么用**临时文件**接管输出，而不是管道：
//    管道要在"等子进程"和"读管道"之间做并发，否则 64KB 管道缓冲一满就死锁。
//    那需要读线程或 overlapped I/O —— 都是额外的并发复杂度。
//    临时文件方案把所有并发问题变成顺序问题，**离线可验证**，
//    而且"先等进程结束、再读文件"天然拿到完整输出。
// ---------------------------------------------------------------------------
class WinSandbox final : public ISandbox {
public:
    explicit WinSandbox(SandboxConfig cfg) : cfg_(std::move(cfg)) {}

    ExecResult exec(const std::string& command, const std::string& cwd,
                    std::chrono::milliseconds timeout) override {
        ExecResult r;

        // ★ 沙箱被显式关掉时**拒绝执行**，而不是"悄悄降级成不隔离"。
        //   静默降级是最坏的一种行为：用户以为自己被保护着。
        if (!cfg_.enabled) {
            r.blockedBySandbox = true;
            r.blockReason      = "沙箱被禁用（enabled=false）：拒绝执行，不静默降级";
            return r;
        }

        const PolicyVerdict pv = policyCheck(command, cfg_);
        if (pv.blocked) {
            r.blockedBySandbox = true;
            r.blockReason      = pv.reason;
            return r;
        }

        return launch(command, cwd, timeout);
    }

    // 能力自检：把"已强制 / 仅策略 / 未实现"三层逐条列出来。
    // 对应 Codex CLI 的 `codex debug seatbelt` —— 用户需要能一条命令问出
    // "我这台机器上沙箱到底生效了没、边界在哪"。
    bool selfCheck(std::string& report) const override {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        const bool jobOk = (job != nullptr);
        if (job) CloseHandle(job);

        std::ostringstream os;
        os << "sandbox=JobObject(win) enabled=" << (cfg_.enabled ? 1 : 0)
           << " jobObject=" << (jobOk ? "ok" : "FAILED")
           << " workspace=" << (cfg_.workspace.empty() ? "(unset)" : cfg_.workspace)
           << " allowNetwork=" << (cfg_.allowNetwork ? 1 : 0)
           << "\n  [已强制·内核]  进程树生命周期（KILL_ON_JOB_CLOSE）"
           << "\n  [已强制·内核]  进程数上限 " << kMaxProcs
           << " / 进程内存上限 " << (kMemLimitBytes >> 20) << " MiB"
           << "\n  [已强制·内核]  超时强杀（TerminateJobObject，含子进程）"
           << "\n  [仅策略·可绕过] 禁网 / denyRead 的命令文本匹配"
           << "\n  [未实现]       文件系统写隔离、真网络隔离"
           << "（需 AppContainer + ACL，本讲刻意不做）";
        report = os.str();
        return cfg_.enabled && jobOk;
    }

    const SandboxConfig& config() const override { return cfg_; }

private:
    static constexpr DWORD kMaxProcs      = 16;
    static constexpr unsigned long long kMemLimitBytes = 1ull << 30;   // 1 GiB

    ExecResult launch(const std::string& command, const std::string& cwd,
                      std::chrono::milliseconds timeout) {
        ExecResult r;

        // ---- 0. 准备临时文件（接管 stdout / stderr）----------------------
        wchar_t tmpDir[MAX_PATH] = {0};
        if (GetTempPathW(MAX_PATH, tmpDir) == 0) {
            r.blockedBySandbox = true;
            r.blockReason      = "GetTempPath 失败";
            return r;
        }
        const std::wstring stem =
            std::wstring(tmpDir) + L"af_sbx_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(GetTickCount64());
        const std::wstring outPath = stem + L"_out.txt";
        const std::wstring errPath = stem + L"_err.txt";

        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;              // 子进程要能继承这两个句柄

        HANDLE hOut = CreateFileW(outPath.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE hErr = CreateFileW(errPath.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
            if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
            if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
            if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
            r.blockedBySandbox = true;
            r.blockReason      = "无法创建沙箱输出文件";
            return r;
        }

        // ---- 1. Job Object：进程树 + 资源上限 ----------------------------
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |      // 句柄一关，全树陪葬
                JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
            li.BasicLimitInformation.ActiveProcessLimit = kMaxProcs;
            li.ProcessMemoryLimit                       = kMemLimitBytes;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                    &li, sizeof(li));
        }

        // ---- 2. 启动（CREATE_SUSPENDED：先入 Job，再放行）----------------
        //  ★ 顺序很重要：必须先挂进 Job 再 Resume。
        //    反过来写会有一个窗口期——进程已经跑起来、但还没进 Job，
        //    它在这个窗口里 fork 出来的子进程**不在 Job 里**，
        //    于是超时强杀时会漏掉它们。
        std::wstring cmdline = L"cmd.exe /c " + toWide(command);

        STARTUPINFOW si{};
        si.cb          = sizeof(si);
        si.dwFlags     = STARTF_USESTDHANDLES;
        si.hStdInput   = hNul;
        si.hStdOutput  = hOut;
        si.hStdError   = hErr;

        PROCESS_INFORMATION pi{};
        const std::wstring wcwd = toWide(cwd);

        const BOOL ok = CreateProcessW(
            nullptr, cmdline.data(), nullptr, nullptr,
            TRUE,                                          // 继承句柄
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            nullptr, wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);

        if (!ok) {
            const DWORD err = GetLastError();
            if (job) CloseHandle(job);
            CloseHandle(hOut); CloseHandle(hErr); CloseHandle(hNul);
            DeleteFileW(outPath.c_str()); DeleteFileW(errPath.c_str());
            r.blockedBySandbox = true;
            r.blockReason = "CreateProcess 失败（错误码 " + std::to_string(err) + "）";
            return r;
        }

        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);

        // ---- 3. 等：带超时 ------------------------------------------------
        long long ms = timeout.count();
        if (ms < 0) ms = 0;
        if (ms > 0x7FFFFFFFll) ms = 0x7FFFFFFFll;

        const DWORD w = WaitForSingleObject(pi.hProcess, DWORD(ms));
        if (w == WAIT_TIMEOUT) {
            r.timedOut = true;
            r.exitCode = 124;                    // 与 coreutils `timeout` 的约定一致
            if (job) TerminateJobObject(job, 124);          // ★ 连子进程一起杀
            else     TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 5000);         // 给它 5s 死透
            r.blockReason = "命令超时（被沙箱强杀，含其子进程）";
        } else {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            r.exitCode = int(code);
        }

        // ---- 4. 收尾 ------------------------------------------------------
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        // ★ 先关句柄再读文件：句柄不关，cmd 的缓冲可能还没 flush 到磁盘。
        CloseHandle(hOut);
        CloseHandle(hErr);
        CloseHandle(hNul);

        r.stdoutOut = readTextFile(outPath);
        r.stderrOut = readTextFile(errPath);
        DeleteFileW(outPath.c_str());
        DeleteFileW(errPath.c_str());
        return r;
    }

    SandboxConfig cfg_;
};

#else   // 非 Windows：本讲不实现，**拒绝执行**（fail-closed）

class UnimplementedSandbox final : public ISandbox {
public:
    explicit UnimplementedSandbox(SandboxConfig cfg) : cfg_(std::move(cfg)) {}
    ExecResult exec(const std::string&, const std::string&,
                    std::chrono::milliseconds) override {
        ExecResult r;
        r.blockedBySandbox = true;
        r.blockReason = "本平台未实现沙箱（本课只在 Windows 上验证）";
        return r;
    }
    bool selfCheck(std::string& report) const override {
        report = "sandbox=unimplemented（本课只在 Windows 上验证）";
        return false;
    }
    const SandboxConfig& config() const override { return cfg_; }
private:
    SandboxConfig cfg_;
};

#endif

}  // namespace

// ---------------------------------------------------------------------------
//  工厂（P-16）
// ---------------------------------------------------------------------------
std::shared_ptr<ISandbox> createSandbox(SandboxConfig cfg) {
#ifdef _WIN32
    return std::make_shared<WinSandbox>(std::move(cfg));
#else
    return std::make_shared<UnimplementedSandbox>(std::move(cfg));
#endif
}

// ---------------------------------------------------------------------------
//  被拦截时必须把话说清楚
//
//  ★ 两条信息缺一不可：
//    ① 为什么被拦（分不清"命令写错"和"被沙箱拦"→ 模型反复重试同一条命令）
//    ② "重试不会改变结果"（不然它会换一种写法重试同一件事，白烧 token）
//    Codex CLI 加了这条提示后离线评测有明显提升。
// ---------------------------------------------------------------------------
std::string sandboxBlockedMessage(const ExecResult& r) {
    if (!r.blockedBySandbox && !r.timedOut) return {};
    std::string s = "[被沙箱拦截] " + r.blockReason;
    s += "\n★ 这**不是命令写错了**，重试不会改变结果。";
    s += "请换一种做法，或说明你需要什么权限。";
    return s;
}

}  // namespace af
