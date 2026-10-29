// ---------------------------------------------------------------------------
//  ui/tui_web.hpp —— Web 后端（题目八）的对外接口
//
//  ★ 为什么另开一个头文件，而不是往 tui.hpp 里加声明：
//    tui.hpp 的 Renderer 接口是"核心与 UI 之间的契约"，CR-3 要求它零改动。
//    新增的工厂/查询函数是**这一讲的扩展**，放这里，tui.hpp 一个字节都不动。
// ---------------------------------------------------------------------------
#pragma once

#include "tui.hpp"

#include <memory>
#include <string>

namespace af::ui {

// AF_WEB_PORT，默认 8080；非法值回落 8080。
int webPortFromEnv();

// 绑定失败（端口被占 / WSA 初始化失败）返回 nullptr —— 调用方据此退回 plain。
std::unique_ptr<Renderer> makeWebRenderer(int port);

bool webIsBackend(const Renderer& r);
int  webClientCount(const Renderer& r);

// 等浏览器连上；期间一直被问"到了吗"。超时返回 false。
bool webWaitClient(Renderer& r, int timeoutMs);

// --web 模式：等浏览器提交任务。timeoutMs < 0 表示一直等。
bool webWaitForTask(Renderer& r, std::string& out, int timeoutMs);

// ★ 多对话版：顺带带回"这条任务属于哪个对话"（cid）。
//   前端每条 task 都带 cid；老客户端不带 → cid 为空，调用方当成默认对话。
bool webWaitForTask(Renderer& r, std::string& out, std::string& cid, int timeoutMs);

// ★ 设置面板：浏览器里改了 API Key / 端点 / 模型，走这条通路送到 C++ 侧。
//   dirty=false 表示"这次没有新设置"，调用方什么都不做。
struct WebSettings {
    bool        dirty = false;
    std::string apiKey, baseUrl, model;
};

// 取走一次待应用的设置（取走就清掉 dirty）。
bool webTakeSettings(Renderer& r, WebSettings& out);

// 当前浏览器是否连着一个 AI 对话（给日志用）。
bool webHasClient(const Renderer& r);

// ★ 手动压缩：浏览器点"压缩上下文" → 这里取走一次请求（取走就清标志）。
//   cid = 要压缩哪个对话的上下文（空 = 默认对话）。
bool webTakeCompact(Renderer& r, std::string& cid);

// ★ 往页面推一条系统提示（压缩进度/结果用）。非 web 后端什么都不做。
void webNotice(Renderer& r, const std::string& text);

}  // namespace af::ui
