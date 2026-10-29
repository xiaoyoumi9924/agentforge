// ---------------------------------------------------------------------------
//  ui/web_json.cpp —— 事件编码（纯函数，无 IO、无线程）
// ---------------------------------------------------------------------------
#include "web_json.hpp"

#include <cstdio>

namespace af::ui {
namespace {

// 单条文本上限：工具输出可能几十万字符，全塞给浏览器会让它卡住 ——
// 这与 L1 降级链是同一个道理：**先截断，再传输**，不要指望对面扛得住。
constexpr std::size_t kMaxToolText = 16 * 1024;
constexpr std::size_t kMaxArgs     = 4 * 1024;

std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

}  // namespace

std::string clipUtf8(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t n = maxBytes;
    // 回退到字符边界：UTF-8 的后续字节形如 10xxxxxx
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
    char tail[64];
    std::snprintf(tail, sizeof tail, "\n…[已截断，原文 %zu 字节]", s.size());
    return s.substr(0, n) + tail;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        case '\b': o += "\\b";  break;
        case '\f': o += "\\f";  break;
        default:
            if (u < 0x20) {                       // 其余控制字符：\u00XX
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(u));
                o += buf;
            } else {
                o += c;                           // ★ UTF-8 字节原样透传
            }
        }
    }
    return o;
}

std::string eventToJson(const UiEvent& e) {
    switch (e.kind) {
    case UiEvent::Kind::Text:
        return e.text.empty() ? std::string() : "{\"t\":\"text\",\"s\":" + q(e.text) + "}";

    case UiEvent::Kind::Thinking:
        return e.text.empty() ? std::string() : "{\"t\":\"think\",\"s\":" + q(e.text) + "}";

    case UiEvent::Kind::ToolCall:
        return "{\"t\":\"toolCall\",\"name\":" + q(e.toolName) + ",\"id\":" + q(e.toolId) + "}";

    case UiEvent::Kind::ToolArgs:
        return "{\"t\":\"toolArgs\",\"id\":" + q(e.toolId) + ",\"name\":" + q(e.toolName) +
               ",\"s\":" + q(clipUtf8(e.text, kMaxArgs)) + "}";

    case UiEvent::Kind::ToolStart:
        return "{\"t\":\"toolStart\",\"name\":" + q(e.toolName) + ",\"id\":" + q(e.toolId) + "}";

    case UiEvent::Kind::ToolDone:
        return "{\"t\":\"toolDone\",\"name\":" + q(e.toolName) + ",\"id\":" + q(e.toolId) +
               ",\"ok\":" + (e.isError ? "false" : "true") +
               ",\"ms\":" + std::to_string(static_cast<long long>(e.elapsedMs)) +
               ",\"s\":" + q(clipUtf8(e.text, kMaxToolText)) + "}";

    case UiEvent::Kind::StreamError:
        return "{\"t\":\"error\",\"stream\":true,\"s\":" + q(e.text) + "}";

    case UiEvent::Kind::Usage:
        return "{\"t\":\"usage\",\"n\":" + std::to_string(e.tokens) + "}";

    case UiEvent::Kind::StreamDone:
        return "{\"t\":\"done\"}";

    case UiEvent::Kind::Notice:
        return "{\"t\":\"notice\",\"s\":" + q(e.text) + "}";

    case UiEvent::Kind::Error:
        return "{\"t\":\"error\",\"s\":" + q(e.text) + "}";

    case UiEvent::Kind::PermAsk:
        return "{\"t\":\"perm\",\"id\":" + std::to_string(e.promptId) + ",\"prompt\":" + q(e.text) + "}";

    case UiEvent::Kind::PermAnswer:
        return "{\"t\":\"permEnd\",\"id\":" + std::to_string(e.promptId) +
               ",\"allow\":" + (e.answer ? "true" : "false") + "}";
    }
    return {};
}

}  // namespace af::ui
