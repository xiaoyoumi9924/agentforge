// ---------------------------------------------------------------------------
//  L9 · secret.hpp —— API Key 的加密存取（Windows DPAPI）
//
//  为什么不能明文：会话日志、备份、截图都可能把 .env 里的 key 带出去。
//  DPAPI 用当前 Windows 用户的凭据加密 —— 拿到文件也解不开（换用户/换机）。
//
//  接口保持平台中立；本课程只在 Windows 上实现（secret_win.cpp）。
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace af {

// 加密为可存盘的字符串（DPAPI blob 的 Base64）；失败返回空串
std::string protectSecret(const std::string& plain);
// 解密；失败返回空串
std::string unprotectSecret(const std::string& b64);

bool    saveSecretFile(const std::string& path, const std::string& key);
std::string loadSecretFile(const std::string& path);   // 失败/不存在返回空串
void    clearSecretFile(const std::string& path);

}  // namespace af
