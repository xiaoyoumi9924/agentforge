// ---------------------------------------------------------------------------
//  sse_parser.hpp —— SSE 分帧器
//
//  为什么必须自己写：TCP 不保证消息边界。一次 onData 可能收到
//  半个事件、一个半事件，甚至切在多字节 UTF-8 字符中间。
//  在没有遇到完整分隔符前绝不能尝试解析 —— 这是"工具参数 JSON 被静默损坏"
//  的根源。
// ---------------------------------------------------------------------------
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace af {

struct SSEEvent {
    std::string event;   // "message"（默认）/"message_stop"/"error" ...
    std::string data;    // 多行 data: 已用 '\n' 拼接
    std::string id;

    bool isDone() const noexcept;   // [DONE] 或 message_stop
};

class SSEParser {
public:
    // 喂入任意长度的字节片段；返回本次能完整切出的所有事件
    std::vector<SSEEvent> push(std::string_view chunk);

    // 流结束时调用：处理不以 "\n\n" 结尾的残留
    std::vector<SSEEvent> flush();

    void reset() noexcept { buffer_.clear(); }
    size_t bufferedBytes() const noexcept { return buffer_.size(); }

private:
    std::string buffer_;   // 跨块缓冲 —— 核心状态

    static std::optional<SSEEvent> parseBlock(std::string_view block);
};

// ---------------------------------------------------------------------------
//  UTF-8 安全：判断 buffer 末尾是否停在一个完整字符中间。
//  用于日志与调试（解析本身不需要，因为按 \n\n 切分天然安全）。
// ---------------------------------------------------------------------------
bool endsWithIncompleteUtf8(std::string_view s) noexcept;

}  // namespace af
