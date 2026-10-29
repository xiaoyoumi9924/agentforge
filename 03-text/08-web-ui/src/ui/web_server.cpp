// ---------------------------------------------------------------------------
//  ui/web_server.cpp —— 最小 HTTP + WebSocket 服务器
//
//  只用 Winsock2 + 手写 SHA1/Base64，不引入任何第三方库（见头文件注释）。
//
//  ★ 三条硬性要求，代码里对应三处：
//    ① 端口被占 → start() 返回 false，调用方退回 plain（不抛、不崩）；
//    ② 浏览器关了 → 读线程 recv 返回 0 → drop() 收尸，Agent 完全不知道；
//    ③ 发送绝不能把渲染线程永久堵住 → SO_SNDTIMEO=2s，超时就标记断开。
//
//  ★ 帧长上限 1MB：浏览器的消息都很小，超过这个数只可能是有人拿 telnet
//    乱发字节 —— 与其分配一块 4GB 的内存，不如直接挂断。
// ---------------------------------------------------------------------------
#include "web_server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace af::web {
namespace {

using Socket = SOCKET;
constexpr Socket kBad = INVALID_SOCKET;

// ---- 原始收发 --------------------------------------------------------------

bool recvExact(Socket s, void* buf, std::size_t n) {
    auto* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < n) {
        const int r = ::recv(s, p + got, static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool sendAll(Socket s, const char* p, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        const int r = ::send(s, p + sent, static_cast<int>(n - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

std::once_flag g_wsaOnce;
std::atomic<bool> g_wsaOk{false};

bool wsaInit() {
    std::call_once(g_wsaOnce, [] {
        WSADATA d{};
        g_wsaOk.store(WSAStartup(MAKEWORD(2, 2), &d) == 0);
    });
    return g_wsaOk.load();
}

// ---- SHA1 / Base64 ---------------------------------------------------------
//
//  WebSocket 握手只要求把 key 和一个固定 GUID 拼起来做 SHA1 再 Base64。
//  SHA1 已经"不安全"很多年了，但这里**不是**用来保证安全的 ——
//  它只是协议规定的握手摘要，所以用它是正确的，不是"用了过时算法"。

void sha1(const std::string& in, unsigned char out[20]) {
    std::uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu;
    std::uint32_t h3 = 0x10325476u, h4 = 0xC3D2E1F0u;

    const std::uint64_t totalBits = static_cast<std::uint64_t>(in.size()) * 8ull;
    std::vector<unsigned char> msg(in.begin(), in.end());
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<unsigned char>((totalBits >> (8 * i)) & 0xFFull));

    std::uint32_t w[80];
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(msg[off + i * 4 + 0]) << 24) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            const std::uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20)      { f = (b & c) | ((~b) & d);            k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                       k = 0xCA62C1D6u; }
            const std::uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    const std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<unsigned char>((hs[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<unsigned char>((hs[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<unsigned char>((hs[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<unsigned char>((hs[i]) & 0xFF);
    }
}

std::string base64(const unsigned char* d, std::size_t n) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve(((n + 2) / 3) * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(d[i]) << 16) |
                                ((i + 1 < n ? static_cast<std::uint32_t>(d[i + 1]) : 0u) << 8) |
                                (i + 2 < n ? static_cast<std::uint32_t>(d[i + 2]) : 0u);
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        o += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        o += (i + 2 < n) ? T[v & 63] : '=';
    }
    return o;
}

std::string wsAcceptKey(const std::string& clientKey) {
    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20];
    sha1(clientKey + kGuid, digest);
    return base64(digest, 20);
}

// ---- HTTP ------------------------------------------------------------------

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool readRequest(Socket s, std::string& req) {
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos) {
        if (req.size() > 16384) return false;      // 请求头不该有这么大
        const int r = ::recv(s, buf, sizeof buf, 0);
        if (r <= 0) return false;
        req.append(buf, static_cast<std::size_t>(r));
    }
    return true;
}

// ---- 来源校验 --------------------------------------------------------------
//
// ★ 为什么只绑 127.0.0.1 还不够：
//   绑回环挡住的是"别的机器"，挡不住"你本机浏览器里另一个网站"。恶意页面
//   可以用 new WebSocket("ws://127.0.0.1:8080/ws") 直接连上来 —— 端口固定、
//   协议公开，它连上就能收到权限弹窗、还能替用户点"允许"。这不是理论问题，
//   是任何本地 WS 服务的标准攻击面（DNS rebinding / CSWSH）。
//
// ★ 判据：浏览器的跨站请求**一定**带 Origin，而且它伪造不了；所以只要
//   Origin 存在就必须是本机同端口的页面。没有 Origin 的放行 —— 那只能是
//   web_probe.ps1 / 自检里的假浏览器这类非浏览器客户端（不是浏览器，就没
//   有"别的网站"这回事）。
//
// ★ 剩下的边界（诚实写在这里）：本机任意进程仍可直接连、并伪造/省略 Origin。
//   要挡住它得加每次启动的随机 token，那是"本机进程互信"层面的问题，
//   不在本作业范围（README「已知限制」里写明）。
bool originAllowed(const std::string& origin, int port) {
    if (origin.empty()) return true;                    // 非浏览器客户端

    std::string s = lower(origin);
    while (!s.empty() && s.back() == '/') s.pop_back();

    const std::string scheme = "http://";
    if (s.rfind(scheme, 0) != 0) return false;          // https / file / null 一律拒
    std::string rest = s.substr(scheme.size());
    if (rest.empty()) return false;

    std::string host, portStr;
    if (rest[0] == '[') {                               // IPv6 字面量
        const std::size_t close = rest.find(']');
        if (close == std::string::npos) return false;
        host = rest.substr(0, close + 1);               // 含方括号，形如 [::1]
        if (close + 1 < rest.size()) {
            if (rest[close + 1] != ':') return false;
            portStr = rest.substr(close + 2);
        }
    } else {
        const std::size_t colon = rest.rfind(':');
        if (colon != std::string::npos) {
            host    = rest.substr(0, colon);
            portStr = rest.substr(colon + 1);
        } else {
            host = rest;
        }
    }

    if (host != "127.0.0.1" && host != "localhost" && host != "[::1]") return false;
    if (portStr.empty()) return true;                   // 默认端口：同源即可
    char* end = nullptr;
    const long p = std::strtol(portStr.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    return p == port;                                   // 必须是我们自己的端口
}

std::unordered_map<std::string, std::string> parseHeaders(const std::string& req) {    std::unordered_map<std::string, std::string> h;
    std::size_t pos = req.find("\r\n");
    if (pos == std::string::npos) return h;
    pos += 2;
    while (pos < req.size()) {
        const std::size_t end = req.find("\r\n", pos);
        if (end == std::string::npos || end == pos) break;
        const std::string line = req.substr(pos, end - pos);
        const std::size_t c = line.find(':');
        if (c != std::string::npos) h[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
        pos = end + 2;
    }
    return h;
}

// ---- WebSocket 帧 ----------------------------------------------------------

std::string encodeTextFrame(const std::string& payload) {
    std::string f;
    f.reserve(payload.size() + 10);
    f.push_back(static_cast<char>(0x81));                 // FIN=1, opcode=text
    const std::size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFFu) {
        f.push_back(static_cast<char>(126));
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i)
            f.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
    }
    f += payload;
    return f;
}

std::string encodeCtrlFrame(unsigned char opcode, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    f.push_back(static_cast<char>(payload.size() & 0x7F));
    f += payload;
    return f;
}

bool readFrame(Socket s, int& opcode, std::string& payload) {
    unsigned char h[2];
    if (!recvExact(s, h, 2)) return false;
    opcode = h[0] & 0x0F;
    const bool masked = (h[1] & 0x80) != 0;
    std::uint64_t len = h[1] & 0x7F;
    if (len == 126) {
        unsigned char e[2];
        if (!recvExact(s, e, 2)) return false;
        len = (static_cast<std::uint64_t>(e[0]) << 8) | e[1];
    } else if (len == 127) {
        unsigned char e[8];
        if (!recvExact(s, e, 8)) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
    }
    if (len > (1u << 20)) return false;                   // 1MB 上限

    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked && !recvExact(s, mask, 4)) return false;

    payload.assign(static_cast<std::size_t>(len), '\0');
    if (len && !recvExact(s, payload.data(), static_cast<std::size_t>(len))) return false;
    if (masked) {
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(payload[i] ^ static_cast<char>(mask[i % 4]));
    }
    return true;
}

}  // namespace

// ===========================================================================
//  Impl
// ===========================================================================
struct WebServer::Impl {
    struct Client {
        Socket            s = kBad;
        int               id = 0;                // 连接编号（日志/审计用）
        std::mutex        wmu;                 // 一条连接上的写必须串行
        std::atomic<bool> alive{true};
        std::atomic<bool> ws{false};           // 握手完成了吗（决定计数）
    };

    explicit Impl(Options o) : opt(std::move(o)) {}

    Options  opt;
    mutable std::mutex mu;
    std::vector<std::shared_ptr<Client>> clients;
    std::atomic<int> nextId{1};

    std::atomic<Socket> listenSock{kBad};
    std::thread         acceptTh;
    std::atomic<bool>   running{false};
    std::atomic<int>    connected{0};

    void drop(const std::shared_ptr<Client>& cl) {
        if (!cl->alive.exchange(false)) return;            // 只收尸一次
        const bool wasWs = cl->ws.load();
        {
            std::lock_guard<std::mutex> lk(mu);
            clients.erase(std::remove(clients.begin(), clients.end(), cl), clients.end());
        }
        if (wasWs) connected.fetch_sub(1);
        ::shutdown(cl->s, SD_BOTH);
        ::closesocket(cl->s);
        // ★ 回调在锁外调：渲染器的回调可能反过来调 send()（要拿 mu）
        if (wasWs && opt.onDisconnect) opt.onDisconnect(cl->id);
    }

    void serve(const std::shared_ptr<Client>& cl) {
        // 5 秒收超时：一条"只连不说话"的 TCP 不能把线程永久占住
        DWORD rcvTo = 5000;
        ::setsockopt(cl->s, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&rcvTo), sizeof rcvTo);
        // 2 秒发超时：浏览器卡住时最多拖慢一帧，不能拖死渲染线程
        DWORD sndTo = 2000;
        ::setsockopt(cl->s, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&sndTo), sizeof sndTo);
        // ★ 关闭即 RST（linger=0）：不让端口留在 TIME_WAIT 里，
        //   否则"关掉浏览器 → 马上重启 agent"会绑不上端口。
        linger lg{};
        lg.l_onoff  = 1;
        lg.l_linger = 0;
        ::setsockopt(cl->s, SOL_SOCKET, SO_LINGER,
                     reinterpret_cast<const char*>(&lg), sizeof lg);

        std::string req;
        if (!readRequest(cl->s, req)) { drop(cl); return; }
        const auto h = parseHeaders(req);

        const auto keyIt = h.find("sec-websocket-key");
        if (keyIt == h.end()) {
            // 普通 HTTP：只服务一页；其它路径 404（不做目录穿越，也就不需要防）
            const bool root = req.rfind("GET / ", 0) == 0 || req.rfind("GET /?", 0) == 0 ||
                              req.rfind("GET /index.html", 0) == 0;
            std::string resp;
            if (root) {
                resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                       "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(opt.html.size()) + "\r\n\r\n" + opt.html;
            } else {
                resp = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                       "Connection: close\r\nContent-Length: 9\r\n\r\nnot found";
            }
            sendAll(cl->s, resp.data(), resp.size());
            drop(cl);
            return;
        }

        // ---- 101 握手 ------------------------------------------------------
        // ★ 先验来源，再谈升级：跨站页面（Origin 不是本机同端口）直接 403。
        //   顺序很重要 —— 先握手再拒，那个页面已经收到 hello/replay 了。
        if (const auto oi = h.find("origin"); oi != h.end() && !originAllowed(oi->second, opt.port)) {
            std::cerr << "[web] 拒绝连接：来源 " << oi->second
                      << " 不是本机页面（只接受 http://127.0.0.1:" << opt.port << "）\n"
                      << std::flush;
            const std::string body = "origin not allowed\n";
            const std::string denied =
                "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain; charset=utf-8\r\n"
                "Connection: close\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\n\r\n" + body;
            sendAll(cl->s, denied.data(), denied.size());
            drop(cl);
            return;
        }

        const std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + wsAcceptKey(keyIt->second) + "\r\n\r\n";
        {
            std::lock_guard<std::mutex> lk(cl->wmu);
            if (!sendAll(cl->s, resp.data(), resp.size())) { drop(cl); return; }
        }
        cl->ws.store(true);
        connected.fetch_add(1);

        // ★★ 握手完成后必须**取消**收超时。
        //    浏览器是长连接：用户读着答案、没敲键盘的 5 分钟里，一个字节都不会发。
        //    留着 SO_RCVTIMEO=5s 的后果是每 5 秒"正常连接被当成断线"，
        //    然后权限弹窗跟着一起消失。这个坑在第一次联调时就踩了 ——
        //    症状很像"前端没收到消息"，根因却在服务端把空闲当成了死亡。
        //    对端真的死了怎么办：stop() 会 shutdown 所有连接，recv 立刻返回。
        DWORD noTimeout = 0;
        ::setsockopt(cl->s, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&noTimeout), sizeof noTimeout);

        if (opt.onConnect) opt.onConnect();

        // ---- 帧循环 --------------------------------------------------------
        while (running.load() && cl->alive.load()) {
            int op = 0;
            std::string payload;
            if (!readFrame(cl->s, op, payload)) break;

            if (op == 0x8) {                                   // close：回一个 close 再走
                const std::string f = encodeCtrlFrame(0x8, {});
                std::lock_guard<std::mutex> lk(cl->wmu);
                sendAll(cl->s, f.data(), f.size());
                break;
            }
            if (op == 0x9) {                                   // ping：原样 pong
                const std::string f = encodeCtrlFrame(0xA, payload);
                std::lock_guard<std::mutex> lk(cl->wmu);
                sendAll(cl->s, f.data(), f.size());
                continue;
            }
            if (op == 0xA) continue;                           // pong：忽略
            if (op == 0x1 && opt.onMessage) opt.onMessage(cl->id, payload);
        }
        drop(cl);
    }

    void acceptLoop() {
        std::vector<std::thread> readers;
        while (running.load()) {
            const Socket l = listenSock.load();
            if (l == kBad) break;
            sockaddr_in sa{};
            int sl = sizeof sa;
            const Socket c = ::accept(l, reinterpret_cast<sockaddr*>(&sa), &sl);
            if (c == kBad) {
                if (!running.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            BOOL one = TRUE;                                   // 别让 Nagle 把小帧攒起来
            ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&one), sizeof one);
            auto cl = std::make_shared<Client>();
            cl->s  = c;
            cl->id = nextId.fetch_add(1);                  // 连接编号从 1 起
            { std::lock_guard<std::mutex> lk(mu); clients.push_back(cl); }
            readers.emplace_back([this, cl] { serve(cl); });
        }
        // 唤醒所有读线程，再等它们退出 —— 顺序不能反（join 完才能析构）
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto& c : clients)
                if (c->alive.load()) ::shutdown(c->s, SD_BOTH);
        }
        for (auto& t : readers)
            if (t.joinable()) t.join();
        {
            std::lock_guard<std::mutex> lk(mu);
            clients.clear();
        }
    }
};

// ===========================================================================
//  公开接口
// ===========================================================================
WebServer::WebServer(Options opt) : impl_(std::make_unique<Impl>(std::move(opt))) {}
WebServer::~WebServer() { stop(); }

bool WebServer::start() {
    if (impl_->running.load()) return true;
    if (!wsaInit()) return false;

    const Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) return false;

    // ★ 用 EXCLUSIVEADDRUSE 而不是 REUSEADDR：Windows 上 REUSEADDR 允许
    //   两个进程绑同一个端口（后绑的会"劫持"进来的连接）。UI 端口被劫持
    //   意味着别人能替你点"允许" —— 这不是性能取舍，是安全问题。
    //   代价是 TIME_WAIT 期间不能立刻重启；所以连接关闭时用 linger=0 发 RST，
    //   让端口不留在 TIME_WAIT 里（见 serve()）。
    BOOL excl = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&excl), sizeof excl);

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(static_cast<u_short>(impl_->opt.port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);                // 只监听本机回环

    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == SOCKET_ERROR ||
        ::listen(s, 16) == SOCKET_ERROR) {
        ::closesocket(s);
        return false;
    }

    impl_->listenSock.store(s);
    impl_->running.store(true);
    impl_->acceptTh = std::thread([this] { impl_->acceptLoop(); });
    return true;
}

void WebServer::stop() {
    if (!impl_) return;
    impl_->running.store(false);
    const Socket s = impl_->listenSock.exchange(kBad);
    if (s != kBad) ::closesocket(s);
    if (impl_->acceptTh.joinable()) impl_->acceptTh.join();
}

bool WebServer::send(const std::string& utf8) {
    std::vector<std::shared_ptr<Impl::Client>> snap;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        snap = impl_->clients;
    }
    std::vector<std::shared_ptr<Impl::Client>> live;
    for (auto& c : snap)
        if (c->alive.load() && c->ws.load()) live.push_back(c);
    if (live.empty()) return false;

    const std::string frame = encodeTextFrame(utf8);
    bool any = false;
    for (auto& c : live) {
        std::lock_guard<std::mutex> lk(c->wmu);
        if (!c->alive.load()) continue;
        if (sendAll(c->s, frame.data(), frame.size())) {
            any = true;
        } else {
            // ★ 只 shutdown、**不** closesocket：让读线程走正常的收尸路径，
            //   否则 socket 会被 double-close（那类 bug 在 Windows 上是随机崩溃）。
            ::shutdown(c->s, SD_BOTH);
        }
    }
    return any;
}

int WebServer::clientCount() const { return impl_->connected.load(); }
int WebServer::port() const { return impl_->opt.port; }

// ===========================================================================
//  仅测试用：冒充一个浏览器（--selftest I-100）
//
//  ★ 这里刻意不写 WSACleanup()：WSA 的引用计数是**进程级**的，服务端那边
//    还有一次 WSAStartup 在生效；这里再减一次会把 Winsock 从服务端脚下抽走
//    —— 表现是"关掉浏览器后下一次 accept 报 10093"。少减一次没有代价，
//    减错一次却是难查的偶发崩溃。
// ===========================================================================
namespace {

// 连上 + 发 101 握手。返回 true = 真的收到了 101（被 403 拒掉时必须是 false）。
bool wsHandshake(Socket s, int port, const std::string& origin) {
    std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1:";
    req += std::to_string(port);
    req += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    if (!origin.empty()) {                        // I-103：按浏览器的方式带来源
        req += "Origin: ";
        req += origin;
        req += "\r\n";
    }
    req += "\r\n";

    if (::send(s, req.data(), static_cast<int>(req.size()), 0) <= 0) return false;
    char buf[256] = {0};
    const int n = ::recv(s, buf, sizeof(buf) - 1, 0);
    return n > 0 &&
           std::string(buf, static_cast<std::size_t>(n)).find("101") != std::string::npos;
}

// 连一条到 127.0.0.1:port 的 TCP（超时 2s）。
Socket wsDial(int port) {
    const Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) return kBad;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<unsigned short>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);

    DWORD to = 2000;   // 连不上 / 收不到握手就别把自检拖成 5 秒
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&to), sizeof(to));

    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::closesocket(s);
        return kBad;
    }
    return s;
}

}  // namespace

bool probeClientConnect(int port, int holdMs, const std::string& origin) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    bool ok = false;
    const Socket s = wsDial(port);
    if (s != kBad) {
        ok = wsHandshake(s, port, origin);
        if (ok && holdMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        // ★ 直接关掉 = 用户关掉标签页：读线程 recv 拿到 0，走正常收尸路径。
        ::closesocket(s);
    }
    return ok;
}

bool probeClientSend(int port, const std::string& origin, const std::string& text,
                     int holdMs, int preDelayMs) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    bool ok = false;
    const Socket s = wsDial(port);
    if (s != kBad) {
        if (wsHandshake(s, port, origin)) {
            if (preDelayMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(preDelayMs));
            // ★ 客户端帧必须掩码（RFC 6455 §5.3）：不掩码的话服务端按协议应当
            //   直接挂断，那这条用例测的就不是"回答被采纳"而是"帧被拒"。
            std::string f;
            f.push_back(static_cast<char>(0x81));               // FIN + text
            const std::size_t n = text.size();
            if (n < 126) {
                f.push_back(static_cast<char>(0x80 | n));       // MASK + 长度
            } else {
                f.push_back(static_cast<char>(0x80 | 126));
                f.push_back(static_cast<char>((n >> 8) & 0xFF));
                f.push_back(static_cast<char>(n & 0xFF));
            }
            const unsigned char mask[4] = {0x37, 0xFA, 0x21, 0x3D};
            f.append(reinterpret_cast<const char*>(mask), 4);
            for (std::size_t i = 0; i < n; ++i)
                f.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
            ok = ::send(s, f.data(), static_cast<int>(f.size()), 0) > 0;
            if (ok && holdMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        }
        ::closesocket(s);
    }
    return ok;
}

bool probeClientRead(int port, const std::string& origin, std::string& out, int ms) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    bool ok = false;
    const Socket s = wsDial(port);
    if (s != kBad) {
        if (wsHandshake(s, port, origin)) {
            DWORD to = 150;                       // 帧之间不会隔太久；gap 超过就收工
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&to), sizeof(to));
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(ms);
            char buf[4096];
            while (std::chrono::steady_clock::now() < deadline) {
                const int n = ::recv(s, buf, sizeof buf, 0);
                if (n <= 0) break;                // 超时 / 对端关了
                out.append(buf, static_cast<std::size_t>(n));
                ok = true;
            }
        }
        ::closesocket(s);
    }
    return ok;
}

}  // namespace af::web
