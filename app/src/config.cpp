// ---------------------------------------------------------------------------
//  L9 · config.cpp —— 配置加载与打印
// ---------------------------------------------------------------------------
#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "secret.hpp"

namespace af {

namespace {

std::string envStr(const char* key, const std::string& def) {
#ifdef _WIN32
    // ★ 环境变量同样是"外部字节"：中文机 getenv 返回 GBK 字节，
    //   中文路径（如 AF_PLUGIN）会变成乱码 → LoadLibrary 126。
    //   必须用 _wgetenv 读宽字符再转 UTF-8。
    std::wstring wkey;
    for (const char* p = key; *p; ++p) wkey.push_back(wchar_t(unsigned char(*p)));
    const wchar_t* w = _wgetenv(wkey.c_str());
    if (!w || !*w) return def;
    const int sz = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return def;
    std::string s(std::size_t(sz - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), sz, nullptr, nullptr);
    return s;
#else
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
#endif
}

int envInt(const char* key, int def) {
    const char* v = std::getenv(key);
    if (!v || !*v) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

std::ifstream openConfig(const std::string& workspace) {
    return std::ifstream(workspace + "/af_config.json", std::ios::binary);
}

// 从 JSON 读配置（容错：缺字段保持默认）
void mergeJson(AppConfig& cfg, const json& j) {
    if (j.contains("provider") && j["provider"].is_object()) {
        const auto& p = j["provider"];
        if (p.contains("kind") && p["kind"].is_string()) cfg.provider.kind = p["kind"].get<std::string>();
        if (p.contains("baseUrl") && p["baseUrl"].is_string()) cfg.provider.baseUrl = p["baseUrl"].get<std::string>();
        if (p.contains("model") && p["model"].is_string()) cfg.provider.model = p["model"].get<std::string>();
    }
    if (j.contains("budget") && j["budget"].is_object()) {
        const auto& b = j["budget"];
        if (b.contains("contextWindow")) cfg.budget.contextWindow = b["contextWindow"].get<int>();
        if (b.contains("reserveForReply")) cfg.budget.reserveForReply = b["reserveForReply"].get<int>();
        if (b.contains("maxOutputTokens")) cfg.budget.maxOutputTokens = b["maxOutputTokens"].get<int>();
    }
    if (j.contains("sandbox") && j["sandbox"].is_object()) {
        const auto& s = j["sandbox"];
        if (s.contains("enabled")) cfg.sandbox.enabled = s["enabled"].get<bool>();
        if (s.contains("allowNetwork")) cfg.sandbox.allowNetwork = s["allowNetwork"].get<bool>();
    }
    if (j.contains("logPath") && j["logPath"].is_string()) cfg.logPath = j["logPath"].get<std::string>();
    if (j.contains("sessionId") && j["sessionId"].is_string()) cfg.sessionId = j["sessionId"].get<std::string>();
}

}  // namespace

AppConfig loadConfig(const std::string& workspace) {
    AppConfig cfg;
    cfg.workspace = workspace;

    // ---- ② 配置文件 ----
    if (auto ifs = openConfig(workspace); ifs) {
        std::ostringstream os;
        os << ifs.rdbuf();
        const json j = json::parse(os.str(), nullptr, false);
        if (!j.is_discarded() && j.is_object()) mergeJson(cfg, j);
    }

    // ---- ③ 环境变量覆盖 ----
    cfg.provider.kind    = envStr("AF_PROVIDER", cfg.provider.kind);
    cfg.provider.baseUrl = envStr("OPENAI_BASE_URL", cfg.provider.baseUrl);
    cfg.provider.model   = envStr("AF_MODEL", cfg.provider.model);
    cfg.provider.apiKey  = envStr("OPENAI_API_KEY", "");

    cfg.logPath   = envStr("AF_LOG", cfg.logPath);
    cfg.pluginPath = envStr("AF_PLUGIN", cfg.pluginPath);
    cfg.sessionId = envStr("AF_SESSION", cfg.sessionId);

    cfg.sandbox.workspace = workspace;
    cfg.budget.contextWindow = envInt("AF_CONTEXT_WINDOW", cfg.budget.contextWindow);

    // ---- 密钥：环境变量优先，其次 DPAPI 密钥文件 ----
    if (cfg.provider.apiKey.empty() && cfg.useSecretFile) {
        cfg.provider.apiKey = loadSecretFile(workspace + "/" + cfg.secretFile);
    }
    return cfg;
}

std::string configToJson(const AppConfig& cfg) {
    json j = json::object();
    j["provider"] = json{{"kind", cfg.provider.kind},
                         {"baseUrl", cfg.provider.baseUrl},
                         {"model", cfg.provider.model},
                         {"apiKey", cfg.provider.apiKey.empty() ? "(未设置)" : "(已设置，已打码)"}};
    j["budget"] = json{{"contextWindow", cfg.budget.contextWindow},
                       {"reserveForReply", cfg.budget.reserveForReply},
                       {"maxOutputTokens", cfg.budget.maxOutputTokens}};
    j["sandbox"] = json{{"enabled", cfg.sandbox.enabled},
                        {"workspace", cfg.sandbox.workspace}};
    j["logPath"]   = cfg.logPath;
    j["sessionId"] = cfg.sessionId;
    j["workspace"] = cfg.workspace;
    return j.dump(2);
}

}  // namespace af


