// ---------------------------------------------------------------------------
//  L11 · tools.hpp —— 内置工具（Read/Write/Edit/Bash）的共享注册入口
//
//  从 main.cpp 抽出来，供两个前端复用：
//    · 命令行 agent.exe（main.cpp）
//    · Qt 图形界面 AgentForgeGUI（gui/）
//  避免"同一套工具在 CLI 和 GUI 各写一遍"的漂移。
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <string>

#include "sandbox.hpp"
#include "tool_registry.hpp"

namespace af {

// 只读工具（无路径围栏）
void registerReadTool(ToolRegistry& reg);

// 写工具：workspace 非空时只允许写工作目录内（纵深防御，非内核边界）
void registerWriteTool(ToolRegistry& reg, std::string workspace);
void registerEditTool(ToolRegistry& reg, std::string workspace);

// Bash：优先在沙箱（Job Object）内执行；sandbox 可为空
void registerBashTool(ToolRegistry& reg, std::shared_ptr<ISandbox> sandbox,
                      std::string workspace);

}  // namespace af
