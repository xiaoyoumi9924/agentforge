// ---------------------------------------------------------------------------
//  ui/web_server.hpp —— 最小 HTTP + WebSocket 服务器（只监听 127.0.0.1）
//
//  ★ 为什么自己写，不用第三方库：
//    本机的第三方依赖只有 FTXUI（77 个 .cpp、85s 编译）和 tree-sitter。
//    引入一个 WebSocket 库意味着再一次"下载 + 编译 + 版本对不上"，
//    而我们要的只有三件事：GET / 返回一页 HTML、101 握手、收发文本帧。
//    Winsock + 80 行 SHA1 就是全部 —— 依赖数量是**交付风险**，不是本事。
//
//  ★ 为什么只绑 127.0.0.1：
//    ① 安全：UI 端口不该暴露在局域网上（谁能连上谁就能替你点"允许"）；
//    ② 工程：绑 0.0.0.0 会触发 Windows 防火墙弹窗，演示现场最怕这个。
//
//  ★ 线程模型（刻意简单）：
//    1 个 accept 线程 + 每连接 1 个读线程。浏览器只开 1 条 WS 连接，
//    所以这是"够用"而不是"偷懒"：连接数上去了再谈 IOCP，现在谈是过度设计。
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace af::web {

class WebServer {
public:
    struct Options {
        int         port = 8080;
        std::string html;                                  // 单页前端（内嵌）
        // ★ 带连接 id：权限回答要能说清"是哪条连接发的"。
        //   id 由服务端在 accept 时分配、单调递增，只在日志/审计里用。
        std::function<void(int clientId, const std::string&)> onMessage;
        std::function<void()>                   onConnect; // 握手完成
        std::function<void(int clientId)>       onDisconnect;
    };

    explicit WebServer(Options opt);
    ~WebServer();

    WebServer(const WebServer&)            = delete;
    WebServer& operator=(const WebServer&) = delete;

    // 绑定 + 监听 + 起 accept 线程。false = 端口被占用 / 初始化失败。
    // ★ 失败必须能"干净地失败"：调用方据此退回 plain（C32 降级不是瘫痪）。
    bool start();
    void stop();

    // 广播一条文本帧。没人连着时返回 false（**不阻塞、不抛**）。
    bool send(const std::string& utf8);

    int  clientCount() const;
    int  port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
//  ★ 仅测试用：冒充一个浏览器（--selftest I-100 用）
//
//  连上 127.0.0.1:port → 发真实的 101 握手 → 保持 holdMs → 主动关闭。
//  返回 true 表示"真的握手成功了"（不是"我假装连上了"）。
//
//  ★ 为什么自检要开一个真 socket：
//    "浏览器关掉，Agent 不崩"的关键不在计数，而在**读线程的收尸路径**
//    （recv 拿到 0 → drop() → onDisconnect → 退回终端输出）。把 clientCount
//    直接改成 0，这条路一行都没跑到，测试就成了摆设。
//
//  origin 非空时按浏览器的方式带上 Origin 头（I-103 用：跨站来源必须被 403 拒掉）。
// ---------------------------------------------------------------------------
bool probeClientConnect(int port, int holdMs, const std::string& origin = std::string());

// ---------------------------------------------------------------------------
//  ★ 仅测试用：冒充浏览器**回答权限**（--selftest I-104 用）
//
//  握手 → 发一个带掩码的文本帧（RFC 6455 要求客户端帧必须掩码）→ 断开。
//  用来证明"回答真的被采纳"，而不只是"消息到了服务端"。
// ---------------------------------------------------------------------------
//  preDelayMs：连上后等一会儿再发（给"Agent 那边先进入等待"留出窗口）。
bool probeClientSend(int port, const std::string& origin, const std::string& text,
                     int holdMs, int preDelayMs = 0);

// 连上后把服务端推来的帧（hello / replay / ...）在 ms 毫秒内读进 out。
// 用来断言"事件里到底带了什么字段"（I-105b 验 cid 打标）。
bool probeClientRead(int port, const std::string& origin, std::string& out, int ms);

}  // namespace af::web
