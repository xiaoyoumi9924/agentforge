// ---------------------------------------------------------------------------
//  L11 · tools.cpp —— 内置工具实现（Read/Write/Edit/Bash）
//
//  设计要点（与 L5/L6 一致）：
//    · Read 豁免落盘（persistThresholdChars = SIZE_MAX）—— 防"读→落盘→再读"死循环
//    · Write/Edit 描述强调"先 Read 再写"，Edit 的 old_str 必须唯一
//    · Bash 走沙箱（Job Object），退出码必须进结果
//    · 写路径围栏：应用层纵深防御（真正的边界在沙箱/权限门）
// ---------------------------------------------------------------------------
#include "tools.hpp"

#include <cstdio>
#include <cstring>
#include <cwctype>
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

#include "logging.hpp"
#include "types.hpp"

namespace af {

namespace {

std::string readFileWithLineNumbers(const std::string& path, int maxLines = 2000) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream os;
    std::string line;
    int n = 0;
    while (std::getline(ifs, line) && n < maxLines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        os << ++n << "\t" << line << "\n";
    }
    return os.str();
}

bool writeFileRaw(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), std::streamsize(content.size()));
    return bool(ofs);
}

std::string readFileRaw(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream os;
    os << ifs.rdbuf();
    return os.str();
}

#ifdef _WIN32
std::wstring u8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring canonicalPath(const std::string& path) {
    const std::wstring wpath = u8ToWide(path);
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(wpath.c_str(), MAX_PATH * 4, buf, nullptr);
    if (!n) return {};
    std::wstring full(buf, n);
    for (auto& c : full) c = wchar_t(std::towlower(c));
    return full;
}

bool pathUnderDir(const std::string& path, const std::string& dir) {
    const std::wstring p = canonicalPath(path);
    std::wstring d = canonicalPath(dir);
    if (p.empty() || d.empty()) return false;
    if (d.back() != L'\\') d.push_back(L'\\');
    return p.compare(0, d.size(), d) == 0 || p == d;
}
#else
bool pathUnderDir(const std::string&, const std::string&) { return true; }
#endif

}  // namespace

// ============================================================================
//  Read
// ============================================================================
void registerReadTool(ToolRegistry& reg) {
    ToolDef def;
    def.name = "Read";
    def.description =
        "读取文件内容，返回带行号的文本。用于查看代码、配置或日志。"
        "不要用它读超过 2000 行的大文件 —— 先用搜索类工具定位范围。";
    def.inputSchema = json::object({
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"},
                                  {"description", "文件的绝对路径或相对路径"}}}}},
        {"required", json::array({"path"})},
    });
    def.risk = RiskLevel::Read;
    def.persistThresholdChars = SIZE_MAX;    // ★ 豁免落盘（正确性要求）

    reg.registerBuiltin(def, [](const json& args) -> ToolResult {
        const std::string path = args.value("path", std::string());
        if (path.empty()) return ToolResult::error("", "path 不能为空");
        std::string content = readFileWithLineNumbers(path);
        if (content.empty())
            return ToolResult::error("", "无法读取文件（不存在或为空）: " + path);
        return ToolResult::ok("", std::move(content));
    });
}

// ============================================================================
//  Write
// ============================================================================
void registerWriteTool(ToolRegistry& reg, std::string workspace) {
    ToolDef def;
    def.name = "Write";
    def.description =
        "写入文件（覆盖写）。★ 必须先 Read 目标文件再写 —— 否则是盲写，"
        "会悄悄丢掉你没读到的行。新建文件可以直接写。";
    def.inputSchema = json::object({
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"}, {"description", "文件路径"}}},
                        {"content", {{"type", "string"}, {"description", "要写入的完整内容"}}}}},
        {"required", json::array({"path", "content"})},
    });
    def.risk = RiskLevel::Write;

    reg.registerBuiltin(def, [workspace](const json& args) -> ToolResult {
        const std::string path = args.value("path", std::string());
        const std::string content = args.value("content", std::string());
        if (path.empty()) return ToolResult::error("", "path 不能为空");

        if (!workspace.empty() && !pathUnderDir(path, workspace)) {
            Logger::instance().warn("audit", "write-rejected", json{{"path", path}});
            return ToolResult::error("", "路径越界：只能在工作目录内写入: " + path);
        }

        const bool ok = writeFileRaw(path, content);
        Logger::instance().log(ok ? "info" : "error", "audit", "write",
                               json{{"path", path}, {"bytes", content.size()}, {"ok", ok}});
        if (!ok) return ToolResult::error("", "写入失败（路径非法或没有权限）: " + path);
        return ToolResult::ok("", "已写入 " + std::to_string(content.size()) + " 字节 → " + path);
    });
}

// ============================================================================
//  Edit
// ============================================================================
void registerEditTool(ToolRegistry& reg, std::string workspace) {
    ToolDef def;
    def.name = "Edit";
    def.description =
        "把文件中唯一出现的 old_str 替换为 new_str。"
        "old_str 必须在文件中恰好出现一次，否则失败 —— 用失败换准确性，"
        "避免误改；失败时请给出更多上下文再试。";
    def.inputSchema = json::object({
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"}, {"description", "文件路径"}}},
                        {"old_str", {{"type", "string"}, {"description", "要被替换的原文（必须唯一）"}}},
                        {"new_str", {{"type", "string"}, {"description", "替换后的内容"}}}}},
        {"required", json::array({"path", "old_str", "new_str"})},
    });
    def.risk = RiskLevel::Write;

    reg.registerBuiltin(def, [workspace](const json& args) -> ToolResult {
        const std::string path = args.value("path", std::string());
        const std::string oldStr = args.value("old_str", std::string());
        const std::string newStr = args.value("new_str", std::string());
        if (path.empty() || oldStr.empty())
            return ToolResult::error("", "path / old_str 不能为空");

        if (!workspace.empty() && !pathUnderDir(path, workspace)) {
            Logger::instance().warn("audit", "edit-rejected", json{{"path", path}});
            return ToolResult::error("", "路径越界：只能在工作目录内编辑: " + path);
        }

        std::string content = readFileRaw(path);
        if (content.empty())
            return ToolResult::error("", "文件不存在或为空: " + path);

        const std::size_t first = content.find(oldStr);
        if (first == std::string::npos)
            return ToolResult::error("", "文件里找不到 old_str，请先 Read 确认原文");
        if (content.find(oldStr, first + 1) != std::string::npos)
            return ToolResult::error("", "old_str 在文件里出现多次，请提供更多上下文让替换唯一");

        content.replace(first, oldStr.size(), newStr);
        const bool ok = writeFileRaw(path, content);
        Logger::instance().log(ok ? "info" : "error", "audit", "edit",
                               json{{"path", path}, {"ok", ok}});
        if (!ok) return ToolResult::error("", "写入失败: " + path);
        return ToolResult::ok("", "已替换 1 处 → " + path);
    });
}

// ============================================================================
//  Bash
// ============================================================================
void registerBashTool(ToolRegistry& reg,
                      std::shared_ptr<ISandbox> sandbox,
                      std::string workspace) {
    ToolDef def;
    def.name = "Bash";
    def.description =
        "在项目目录执行 shell 命令并返回 stdout+stderr。"
        "结果里会附带一行 [exit code N]，请以它判断成败。"
        "命令在受限沙箱中执行（超时会被强杀）。";
    def.inputSchema = json::object({
        {"type", "object"},
        {"properties", {{"command", {{"type", "string"}, {"description", "要执行的命令"}}}}},
        {"required", json::array({"command"})},
    });
    def.risk = RiskLevel::Exec;

    reg.registerBuiltin(def, [sandbox, workspace](const json& args) -> ToolResult {
        const std::string command = args.value("command", std::string());
        if (command.empty()) return ToolResult::error("", "command 不能为空");

        Logger::instance().info("audit", "bash", json{{"command", command}});

        if (sandbox && sandbox->config().enabled) {
            const ExecResult er = sandbox->exec(command, workspace, std::chrono::seconds(60));
            std::string out = er.stdoutOut;
            if (!er.stderrOut.empty()) out += er.stderrOut;

            if (er.blockedBySandbox) {
                Logger::instance().error("audit", "bash-blocked",
                                         json{{"command", command}, {"reason", er.blockReason}});
                out += "\n" + sandboxBlockedMessage(er);
                return ToolResult::error("", std::move(out));
            }

            out += "\n[exit code " + std::to_string(er.exitCode) + "]";
            Logger::instance().log(er.exitCode == 0 ? "info" : "warn", "audit", "bash-done",
                                   json{{"command", command}, {"exit", er.exitCode}});
            if (er.exitCode != 0) return ToolResult::error("", std::move(out));
            return ToolResult::ok("", std::move(out));
        }

        // 无沙箱兜底
        FILE* p = _popen(command.c_str(), "r");
        if (!p) return ToolResult::error("", "无法启动命令: " + command);

        std::string out;
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        const int code = _pclose(p);

        out += "\n[exit code " + std::to_string(code) + "]";
        if (code != 0) return ToolResult::error("", std::move(out));
        return ToolResult::ok("", std::move(out));
    });
}

}  // namespace af
