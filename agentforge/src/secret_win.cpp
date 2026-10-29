// ---------------------------------------------------------------------------
//  L9 · secret_win.cpp —— DPAPI 密钥存取（Windows）
//
//  CryptProtectData / CryptUnprotectData：以当前用户身份加密，
//  文件内容（Base64 后的密文）落盘；换用户或换机后无法解密 —— 这正是我们要的。
// ---------------------------------------------------------------------------
#include "secret.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dpapi.h>
#  pragma comment(lib, "crypt32.lib")
#endif

namespace af {

namespace {

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = (unsigned char(in[i]) << 16) | (unsigned char(in[i + 1]) << 8) |
                           (unsigned char(in[i + 2]));
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        const unsigned v = unsigned(char(in[i])) << 16;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (unsigned(char(in[i])) << 16) | (unsigned(char(in[i + 1])) << 8);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::string base64Decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((buf >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace

#ifdef _WIN32
std::string protectSecret(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = DWORD(plain.size());
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"AgentForge secret", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    std::string blob(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return base64Encode(blob);
}

std::string unprotectSecret(const std::string& b64) {
    if (b64.empty()) return {};
    const std::string blob = base64Decode(b64);
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()));
    in.cbData = DWORD(blob.size());
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}
#else
std::string protectSecret(const std::string&) { return {}; }
std::string unprotectSecret(const std::string&) { return {}; }
#endif

bool saveSecretFile(const std::string& path, const std::string& key) {
    const std::string b64 = protectSecret(key);
    if (b64.empty()) return false;
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs << b64;
    return bool(ofs);
}

std::string loadSecretFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::string b64;
    std::getline(ifs, b64);
    return unprotectSecret(b64);
}

void clearSecretFile(const std::string& path) {
    std::remove(path.c_str());
}

}  // namespace af
