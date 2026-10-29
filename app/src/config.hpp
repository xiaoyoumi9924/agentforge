// ---------------------------------------------------------------------------
//  L9 · config.hpp —— 配置管理（模块 ⑩）
//
//  三层来源，后者覆盖前者：
//    ① 默认值（代码内）        ② af_config.json（工作目录）        ③ 环境变量（AF_* / OPENAI_*）
//
//  为什么需要集中配置：散落在各处的 getenv 会让"为什么这次跑得慢/贵"无法回答；
//  配置文件给人看，环境变量给 CI 覆盖，代码默认值保证零配置可用。
// ---------------------------------------------------------------------------
#pragma once

#include <string>

#include "context_budget.hpp"
#include "model_provider.hpp"
#include "sandbox.hpp"
#include "types.hpp"

namespace af {

struct AppConfig {
    ProviderConfig provider;
    BudgetConfig   budget;
    LoopLimits     limits;
    SandboxConfig  sandbox;

    std::string workspace   = ".";              // 工作目录（沙箱/围栏基准）
    std::string logPath     = "agentforge.log";
    std::string pluginPath;                     // C ABI 插件（可空）
    std::string sessionId   = "default";        // 会话名（持久化用）
    std::string secretFile  = ".af_secret";     // DPAPI 密钥文件
    bool        useSecretFile = true;           // 无环境变量时尝试从密钥文件读取
};

// 读取配置：文件 → 环境变量覆盖；密钥优先级：环境变量 > DPAPI 密钥文件
AppConfig loadConfig(const std::string& workspace = ".");

// 打印用（敏感字段打码）
std::string configToJson(const AppConfig& cfg);

}  // namespace af
