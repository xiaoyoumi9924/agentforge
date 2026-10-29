// ---------------------------------------------------------------------------
//  L2 · sandbox.hpp —— 端口 ③：执行环境隔离
//
//  为什么安全边界必须在内核层（ADR-4 的完整论证）：
//
//    ① 命令白名单是安全剧场 —— 靠空白符/变量展开有无数种绕法：
//         rm -r -f   /   rm${IFS}-rf   /   find . -delete   /   自写脚本再跑
//    ② 容器也不够 —— 共享宿主内核，逃逸面太大，且要求用户装 Docker
//    ③ 应用层的路径检查也不行 —— 软链、`..` 逃逸、经由别的程序写文件……
//       你要在应用层把这些全想到，才等价于内核层的一条规则
//
//    真正的边界要下沉到：Landlock + seccomp / Seatbelt / Job Object + AppContainer。
//
//  ★ 本讲只有声明 —— 这是刻意的。**接口在设计阶段定下来，实现在需要时补**，
//    这正是端口与适配器的价值：L6 补实现时，循环内核一行都不用改。
// ---------------------------------------------------------------------------
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "types.hpp"

namespace af {

struct SandboxConfig {
    bool        enabled   = true;
    std::string workspace;                    // 唯一可写目录
    bool        allowNetwork = false;         // 默认禁网：防注入、防外传
    std::vector<std::string> writableExtra;   // 例如 /tmp
    std::vector<std::string> denyRead;        // 例如 ~/.ssh、*.env
    bool        protectGit = true;            // .git 只读（防模型改写历史）
};

struct ExecResult {
    int         exitCode = 0;
    std::string stdoutOut;
    std::string stderrOut;
    bool        timedOut = false;

    // ★ 必须能区分"命令自己错了"和"被沙箱拦了"。
    //   分不清的后果：模型以为命令写错了，无限重试同一条命令。
    bool        blockedBySandbox = false;
    std::string blockReason;
};

class ISandbox {
public:
    virtual ~ISandbox() = default;

    // 在沙箱内执行一条命令
    virtual ExecResult exec(const std::string& command,
                            const std::string& cwd,
                            std::chrono::milliseconds timeout) = 0;

    // 能力自检：沙箱必须可诊断，否则没人敢用。
    // 对应 Codex CLI 的 `codex debug seatbelt` / `codex debug landlock` ——
    // 用户需要能一条命令问出"我这台机器上沙箱到底生效了没有、能力边界在哪"。
    virtual bool selfCheck(std::string& report) const = 0;

    virtual const SandboxConfig& config() const = 0;
};

// 工厂：按平台选实现（Linux / macOS / Windows 三套，能力不等价）
std::shared_ptr<ISandbox> createSandbox(SandboxConfig cfg);

// ---------------------------------------------------------------------------
//  被拦截时必须把原因说清楚，否则模型会以为命令写错了而反复重试。
//  ★ 而且必须明确告诉它"重试不会改变结果" ——
//    否则它会用不同写法重试同一件事，白白烧 token。
// ---------------------------------------------------------------------------
std::string sandboxBlockedMessage(const ExecResult& r);

}  // namespace af
