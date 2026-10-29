#include "sse_parser.hpp"

#include <algorithm>

namespace af {

namespace {
constexpr std::string_view kSep  = "\n\n";
constexpr std::string_view kSep2 = "\r\n\r\n";
}  // namespace

bool SSEEvent::isDone() const noexcept {
    if (data == "[DONE]") return true;
    if (event == "message_stop") return true;
    return false;
}

std::vector<SSEEvent> SSEParser::push(std::string_view chunk) {
    buffer_.append(chunk);
    std::vector<SSEEvent> out;

    // 同时支持 "\n\n" 与 "\r\n\r\n"（Windows 系 provider 会发后者）
    for (;;) {
        size_t pos = std::string::npos;
        int sepLen = 2;

        auto p1 = buffer_.find(kSep);
        auto p2 = buffer_.find(kSep2);
        if (p2 != std::string::npos && (p1 == std::string::npos || p2 < p1)) {
            pos = p2; sepLen = 4;
        } else if (p1 != std::string::npos) {
            pos = p1; sepLen = 2;
        } else {
            break;   // 没有完整分隔符 —— 留在 buffer 里等下一块
        }

        std::string raw = buffer_.substr(0, pos);
        buffer_.erase(0, pos + sepLen);

        if (auto e = parseBlock(raw)) out.push_back(std::move(*e));
    }
    return out;
}

std::vector<SSEEvent> SSEParser::flush() {
    std::vector<SSEEvent> out;
    if (buffer_.empty()) return out;
    std::string raw = std::move(buffer_);
    buffer_.clear();
    if (auto e = parseBlock(raw)) out.push_back(std::move(*e));
    return out;
}

std::optional<SSEEvent> SSEParser::parseBlock(std::string_view block) {
    SSEEvent ev;
    std::vector<std::string> dataLines;
    bool hasData = false;

    // 逐行处理；块内行分隔符可能是 \n 或 \r\n
    size_t i = 0;
    while (i <= block.size()) {
        size_t nl = block.find('\n', i);
        std::string_view line =
            (nl == std::string::npos) ? block.substr(i) : block.substr(i, nl - i);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (line.empty() || line[0] == ':') {
            // 空行或注释帧（keepalive ": keepalive"）—— 忽略
        } else if (line.starts_with("data:")) {
            // 规范：字段名后有且仅有一个可选前导空格
            std::string_view v = line.substr(5);
            if (!v.empty() && v[0] == ' ') v.remove_prefix(1);
            dataLines.emplace_back(v);
            hasData = true;
        } else if (line.starts_with("event:")) {
            std::string_view v = line.substr(6);
            if (!v.empty() && v[0] == ' ') v.remove_prefix(1);
            ev.event = std::string(v);
        } else if (line.starts_with("id:")) {
            std::string_view v = line.substr(3);
            if (!v.empty() && v[0] == ' ') v.remove_prefix(1);
            ev.id = std::string(v);
        }
        // 其它字段（retry: 等）忽略

        if (nl == std::string::npos) break;
        i = nl + 1;
    }

    if (!hasData) return std::nullopt;

    // 多行 data 用 '\n' 拼接后再交给上层 parse —— 关键
    for (size_t k = 0; k < dataLines.size(); ++k) {
        if (k) ev.data += '\n';
        ev.data += dataLines[k];
    }
    return ev;
}

bool endsWithIncompleteUtf8(std::string_view s) noexcept {
    // 从末尾最多回看 4 字节，判断最后一个字符是否还没收全
    size_t n = std::min<size_t>(4, s.size());
    for (size_t k = 1; k <= n; ++k) {
        unsigned char c = static_cast<unsigned char>(s[s.size() - k]);
        size_t expected;
        if ((c & 0x80) == 0x00) expected = 1;
        else if ((c & 0xE0) == 0xC0) expected = 2;
        else if ((c & 0xF0) == 0xE0) expected = 3;
        else if ((c & 0xF8) == 0xF0) expected = 4;
        else continue;                      //  continuation byte
        return k < expected;                // 收到了 lead byte 但字节数不够
    }
    return false;
}

}  // namespace af
