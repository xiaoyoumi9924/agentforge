// ---------------------------------------------------------------------------
//  ui/web_json.hpp —— UiEvent → JSON 的编码规则
//
//  ★ 单独抽出来，是为了**可测**：`--selftest` 里可以直接喂一个 UiEvent
//    断言编码结果，不需要起服务器、也不需要浏览器。
//    （L7 定下的规矩：能纯函数验证的，就不要留给"跑起来看看"。）
//
//  ★ 只发"能消费的东西"：ToolCallDelta 那种半个 JSON 的中间态不发 ——
//    浏览器拿到它也没法画。这一点与 tui.hpp 里 UiEvent 的设计同源。
// ---------------------------------------------------------------------------
#pragma once

#include "tui.hpp"

#include <string>

namespace af::ui {

// UTF-8 安全截断：不会把一个多字节字符切成两半（切了浏览器就是乱码方块）。
std::string clipUtf8(const std::string& s, std::size_t maxBytes);

// 转义成 JSON 字符串**内容**（不含两侧引号）。UTF-8 原样透传。
std::string jsonEscape(const std::string& s);

// 编码成一行 JSON。空串表示"这条事件不发"。
std::string eventToJson(const UiEvent& e);

}  // namespace af::ui
