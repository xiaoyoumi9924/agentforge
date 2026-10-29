// ---------------------------------------------------------------------------
//  L2 · http_transport.hpp —— 端口 ②：HTTP 传输
//
//  为什么必须有这个端口：
//    providers.cpp 里的协议转换（OpenAI ↔ Anthropic）是**平台无关的核心逻辑**，
//    不该被 libcurl / WinHTTP 这些平台细节绑死。抽掉这层，代价是：
//      · Windows 上用户得先装 vcpkg + libcurl
//      · 换 HTTP 库 = 重写协议转换代码
//
//  抽出来后（L3 兑现）：
//      Windows  → WinHTTP（系统自带，零额外依赖）
//      Linux/macOS → libcurl
//    而 providers.cpp 一行都不用改。
//
//  ★ 本讲只有声明，没有实现 —— 实现是 L3 的事。
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace af {

struct HttpRequestSpec {
    std::string              url;                 // 完整 URL
    std::vector<std::string> headers;             // 每行形如 "X-Api-Key: xxx"
    std::string              body;                // 已序列化好的 JSON
    int connectTimeoutMs = 10'000;
    int totalTimeoutMs   = 600'000;
};

struct HttpResponseInfo {
    long        statusCode = 0;
    bool        transportError = false;           // 连不上 / DNS 失败 / 超时
    std::string errorMessage;
};

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;

    // 非流式：一次拿回整包。
    // 注意 body 是**出参**（引用），因为响应体可能很大，不该再拷贝一次。
    virtual HttpResponseInfo post(const HttpRequestSpec& req, std::string& body) = 0;

    // 流式：每收到一段字节就回调。
    // ★ 回调返回 false 表示调用方要求取消 —— 必须**立刻中止传输**，
    //   否则用户按了中断还要等模型把整段吐完。
    virtual HttpResponseInfo postStream(
        const HttpRequestSpec& req,
        const std::function<bool(const char* data, std::size_t len)>& onData) = 0;
};

// 工厂（P-16 工厂方法）：按平台选实现。
// Windows → WinHTTP ；其它 → libcurl
std::unique_ptr<IHttpTransport> createHttpTransport();

}  // namespace af
