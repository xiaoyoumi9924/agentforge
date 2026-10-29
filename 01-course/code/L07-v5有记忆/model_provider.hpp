// ---------------------------------------------------------------------------
//  L2 · model_provider.hpp —— 端口 ①：模型适配
//
//  上层（循环 / 上下文 / UI）只依赖这个接口，不知道底层是 OpenAI、
//  Anthropic 还是本地 Ollama。新模型靠**配置**接入，而不是改代码。
//
//  这个端口是本课程"第二个实现出现时才抽象"原则的样板：
//    我们在 L3 真的要把同一份协议转换代码接到两家 API 上，
//    这时才引入它 —— 而不是第一天就引入一个只有一处实现的"未来可能有用"的接口。
//
//  ★ 本讲只有声明，没有实现 —— 实现在 L3。
// ---------------------------------------------------------------------------
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "types.hpp"

namespace af {

// ---------------------------------------------------------------------------
//  重试策略
//
//  ★ 关键约束：**只在流开始之前重试**。
//    一旦已经开始吐 token 再重试，等于①重新计费 ②用户会看到重复内容。
//    所以 firstTokenTimeoutMs 与 totalTimeoutMs 是两个不同的超时。
// ---------------------------------------------------------------------------
struct RetryPolicy {
    int    maxAttempts        = 3;
    int    initialBackoffMs   = 1'000;
    int    maxBackoffMs       = 30'000;
    double backoffMultiplier  = 2.0;
    bool   jitter             = true;      // 抖动：防惊群（100 个客户端同时重试）
    std::vector<int> retryOn  = {408, 409, 429, 500, 502, 503, 504};
    int    firstTokenTimeoutMs = 30'000;   // TTFT —— 最重要的体验指标
    int    totalTimeoutMs      = 600'000;
};

class IModelProvider {
public:
    virtual ~IModelProvider() = default;

    // 厂商标识（"openai" / "anthropic"）。
    // ★ 注意：这不是模型名。曾经因为它，所有请求都发出了 model="openai"。
    virtual std::string_view id() const = 0;

    virtual int  contextWindow() const = 0;          // 压缩触发条件依赖它
    virtual bool supportsTools() const = 0;
    virtual bool supportsStreaming() const = 0;
    virtual bool supportsPromptCaching() const = 0;

    virtual ChatResponse chat(const ChatRequest& req) = 0;

    // 流式。onChunk 返回 false 表示调用方要求取消 —— 必须一路传播到传输层。
    virtual ChatResponse stream(
        const ChatRequest& req,
        const std::function<bool(const ChatChunk&)>& onChunk) = 0;

    // 预算计算要用。真实实现应接 tokenizer；
    // 用"字节数 / 4"估算时，压缩触发点必须留足安全边际。
    virtual int countTokens(std::string_view text) = 0;
};

// ---------------------------------------------------------------------------
//  Provider 配置（数据类，不是接口）
// ---------------------------------------------------------------------------
struct ProviderConfig {
    std::string kind;         // "openai" | "anthropic"（本地 ollama/vllm 走 openai 协议）
    std::string baseUrl;
    std::string apiKey;
    std::string model;        // 默认模型名。请求里 req.model 为空时用它
    int         contextWindow = 200'000;
    RetryPolicy retry;
};

// 工厂（P-16）
std::unique_ptr<IModelProvider> createProvider(const ProviderConfig& cfg);

// ---------------------------------------------------------------------------
//  请求追踪：每个请求都要有一条（可观测性的基础，L12 会用到）
// ---------------------------------------------------------------------------
struct RequestTrace {
    std::string traceId, sessionId, turnId, provider, model;
    int  stepIndex = 0;
    Ms   ttftMs{0};          // Time To First Token
    Ms   totalMs{0};
    Usage usage;
    double cacheHitRate = 0.0;
    FinishReason finishReason = FinishReason::Other;
    int retried = 0;
    std::string error;
};

}  // namespace af
