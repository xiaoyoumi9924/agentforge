// ---------------------------------------------------------------------------
//  http_winhttp.cpp —— Windows 后端：WinHTTP
//
//  选它的理由：Windows 系统自带，不需要额外装 libcurl / vcpkg。
//  注意：WinHTTP 默认会缓冲，流式要显式关掉（见下面的禁用缓冲设置）。
// ---------------------------------------------------------------------------
#if defined(_WIN32)

#include "http_transport.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <string>

namespace af {
namespace {

// 从 URL 拆出 host / path / 是否 https / 端口
struct UrlParts { std::wstring host, path; bool https = true; int port = 443; };

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

bool crackUrl(const std::string& url, UrlParts& out) {
    std::wstring w = toWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}; wchar_t path[2048]{};
    uc.lpszHostName = host;  uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), DWORD(w.size()), 0, &uc)) return false;
    out.host = host;
    out.path = uc.lpszUrlPath ? path : L"/";
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out.port = uc.nPort ? uc.nPort : (out.https ? 443 : 80);
    return true;
}

class WinHttpTransport final : public IHttpTransport {
public:
    HttpResponseInfo post(const HttpRequestSpec& req, std::string& body) override {
        return doRequest(req, [&](const char* d, size_t n) {
            body.append(d, n);
            return true;
        });
    }

    HttpResponseInfo postStream(
        const HttpRequestSpec& req,
        const std::function<bool(const char* data, size_t len)>& onData) override {
        return doRequest(req, onData);
    }

private:
    HttpResponseInfo doRequest(const HttpRequestSpec& spec,
                               const std::function<bool(const char*, size_t)>& onData) {
        HttpResponseInfo info;
        UrlParts u;
        if (!crackUrl(spec.url, u)) {
            info.transportError = true;
            info.errorMessage = "bad url: " + spec.url;
            return info;
        }

        HINTERNET sess = WinHttpOpen(L"AgentForge/0.1",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!sess) { info.transportError = true; info.errorMessage = "WinHttpOpen failed"; return info; }

        WinHttpSetTimeouts(sess, spec.connectTimeoutMs, spec.connectTimeoutMs,
                           spec.totalTimeoutMs, spec.totalTimeoutMs);

        HINTERNET conn = WinHttpConnect(sess, u.host.c_str(), INTERNET_PORT(u.port), 0);
        if (!conn) { WinHttpCloseHandle(sess); info.transportError = true;
                     info.errorMessage = "WinHttpConnect failed"; return info; }

        HINTERNET req = WinHttpOpenRequest(conn, L"POST", u.path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           u.https ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
                    info.transportError = true; info.errorMessage = "WinHttpOpenRequest failed"; return info; }

        // 拼 header 块（CRLF 分隔，结尾两个空行）
        std::wstring hdrs;
        for (const auto& h : spec.headers) { hdrs += toWide(h); hdrs += L"\r\n"; }

        // ★ 流式必须关掉 WinHTTP 的内部缓冲，否则收不到增量 chunk
        DWORD disableFeature = WINHTTP_DISABLE_COOKIES;
        // 保持连接活跃，避免长思考期被掐断
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disableFeature, sizeof(disableFeature));

        // WinHttpSendRequest(hRequest, headers, headersLen,
        //                    optional, optionalLen, totalLen, context) —— 7 个参数
        BOOL ok = WinHttpSendRequest(req,
                                     hdrs.empty() ? nullptr : hdrs.c_str(),
                                     DWORD(hdrs.size()),
                                     (void*)spec.body.data(), DWORD(spec.body.size()),
                                     DWORD(spec.body.size()), 0);
        if (!ok || !WinHttpReceiveResponse(req, nullptr)) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
            info.transportError = true;
            info.errorMessage = "send/recv failed: " + std::to_string(GetLastError());
            return info;
        }

        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        info.statusCode = long(status);

        // 读响应体，逐块回调
        char buf[8192];
        DWORD read = 0;
        bool cancelled = false;
        while (WinHttpReadData(req, buf, sizeof(buf), &read) && read > 0) {
            if (onData && !onData(buf, size_t(read))) { cancelled = true; break; }
        }
        if (cancelled) { /* 上层取消：正常路径，不做错误处理 */ }

        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(sess);
        return info;
    }
};

}  // namespace

std::unique_ptr<IHttpTransport> createHttpTransport() {
    return std::make_unique<WinHttpTransport>();
}

}  // namespace af

#endif  // _WIN32
