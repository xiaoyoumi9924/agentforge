// ---------------------------------------------------------------------------
//  L6 · logging.hpp —— 结构化日志（模块 ⑨）
//
//  为什么结构化：自由文本日志在 Agent 场景下几乎不可查询 ——
//  你要回答"上一轮为什么超预算"时，需要按 turn/step/tool 字段过滤。
//  输出采用 JSON Lines：一行一个事件，便于 grep / jq / 后续可视化。
//
//  设计取舍：不引第三方日志库（spdlog 等），保持零依赖；
//  代价是功能少（无轮转/无异步），课程规模足够，L12 需要时再补。
// ---------------------------------------------------------------------------
#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#include "types.hpp"

namespace af {

class Logger {
public:
    static Logger& instance();

    void setFile(std::string path);        // 空路径 = 关闭文件输出
    bool enabled() const noexcept;

    void log(std::string_view level, std::string_view module,
             std::string_view message, const json& extra = json::object());

    void info(std::string_view module, std::string_view message,
              const json& extra = json::object());
    void warn(std::string_view module, std::string_view message,
              const json& extra = json::object());
    void error(std::string_view module, std::string_view message,
               const json& extra = json::object());

private:
    Logger() = default;
    std::mutex mu_;
    std::ofstream ofs_;
    bool open_ = false;
};

// 时间戳：本地时间 ISO8601（毫秒）
std::string nowStamp();

}  // namespace af
