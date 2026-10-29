// ---------------------------------------------------------------------------
//  L6 · logging.cpp —— 结构化日志实现（JSON Lines）
// ---------------------------------------------------------------------------
#include "logging.hpp"

#include <chrono>
#include <ctime>

namespace af {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

std::string nowStamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, int(ms.count()));
    return buf;
}

void Logger::setFile(std::string path) {
    std::lock_guard<std::mutex> lock(mu_);
    if (ofs_.is_open()) ofs_.close();
    open_ = false;
    if (path.empty()) return;
    ofs_.open(path, std::ios::out | std::ios::app);
    open_ = ofs_.is_open();
}

bool Logger::enabled() const noexcept { return open_; }

void Logger::log(std::string_view level, std::string_view module,
                 std::string_view message, const json& extra) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!open_) return;

    json line = json::object();
    line["ts"]    = nowStamp();
    line["level"] = std::string(level);
    line["mod"]   = std::string(module);
    line["msg"]   = std::string(message);
    if (!extra.empty()) line["extra"] = extra;

    ofs_ << line.dump() << '\n';
    ofs_.flush();
}

void Logger::info(std::string_view module, std::string_view message, const json& extra) {
    log("info", module, message, extra);
}
void Logger::warn(std::string_view module, std::string_view message, const json& extra) {
    log("warn", module, message, extra);
}
void Logger::error(std::string_view module, std::string_view message, const json& extra) {
    log("error", module, message, extra);
}

}  // namespace af
