# L11 Qt GUI —— 设计说明

| 项 | 内容 |
|---|---|
| 阶段 | L11 · 用户界面与交互（模块 ⑪） |
| 环境 | Qt 6.8.3 (msvc2022_64) + CMake 3.30 + Ninja + MSVC 14.44 |
| 代码 | `gui/{gui_main,main_window}.{hpp,cpp}`、`gui/CMakeLists.txt`、`build_gui.ps1`；核心复用 `src/tools.*` 与全部 L5-L10 模块 |
| 验收 | `tests/verify_L11.ps1`：依赖检查 + 无头冒烟 + CLI 回归 → 8/8 PASS |

## 1. 三个面板（对应课程要求）

| 面板 | 实现 |
|---|---|
| 对话窗口 | `QTextBrowser` 流式渲染：`LoopHooks.onChunk` 的 `TextDelta` → `insertText` 追加，自动滚动 |
| 工具反馈 / diff | 右侧 `QPlainTextEdit`：工具调用参数 + 结果全文；Edit 场景展示 old/new |
| 命令面板 | 输入框以 `/` 开头执行本地命令：`/help /sandbox /index /sessions /context /clear`；工具栏一键版 |

## 2. 线程模型

```
UI 线程: 输入 → startRun() → 起 worker 线程
worker:  构造 Provider/Registry/Assembler/Loop（与 CLI 同一组合根）→ Loop.run
          事件 → QMetaObject::invokeMethod(this, ..., QueuedConnection) → UI 更新
权限询问: worker 阻塞在 std::future；UI 线程弹 QMessageBox，答复后唤醒
```

- 停止按钮：`std::stop_source` → `LoopLimits.stop`；流式回调检查 `stop_requested()`，立即中止传输；
- 析构：`request_stop()` + `join()`，保证窗口关闭时 worker 不会悬垂。

## 3. 与 CLI 的关系

- 工具注册抽到 `src/tools.{hpp,cpp}`，CLI 与 GUI 共用（避免两套实现漂移）；
- 会话持久化（SQLite）、检索、沙箱、DPAPI 密钥、配置加载全部复用；
- GUI 额外接收 `LoopHooks.confirm` 并映射为模态对话框 —— 权限门 Ask 分支在 GUI 里也有人工确认。

## 4. 构建与验证

```powershell
./build_gui.ps1             # CMake+Ninja 构建 + windeployqt（含 offscreen 插件）
./tests/verify_L11.ps1      # 无头冒烟 + CLI 回归
./build_gui/AgentForgeGUI.exe   # 双击运行（真实界面）
```

环境变量 `AF_GUI_SMOKE=1` + `QT_QPA_PLATFORM=offscreen`：1.5 秒后打印 `GUI SMOKE OK` 并退出（无头验证）。

## 5. 已知边界

- 本轮 diff 面板是"参数+结果对照"，未做逐行 diff 高亮（可作为期末展示的优化点）；
- 流式期间 UI 不做 token 计数展示（最终 Done 事件里打印计费）；
- Qt offscreen 下缺少字体目录（警告无害）；真实桌面运行时使用系统字体。

## 6. 视觉设计（v2 · Apple HIG 风格）

- **设计语言**：浅色"精密工作台"——底色 #F5F5F7、卡片纯白、发丝线 #E8E8ED，单一系统蓝 #0071E3；
- **字体**：系统字体栈（Segoe UI Variable / 微软雅黑），等宽只用于数据与代码（Cascadia Mono）；
- **结构**：左侧边栏（品牌 / 快捷命令 / 会话 / 状态卡）｜中央对话流（用户蓝色气泡、助手白色卡片、工具卡片、提示/完成胶囊）｜右侧工具详情（参数 + 完整结果，等宽）；
- **交互**：输入条为圆角 22px 的白色胶囊（发送/停止），底部状态条含模型 / 沙箱 / 会话 / tokens 芯片；Esc 停止；工具卡片可点击看详情；
- **状态**：运行中禁用输入并显示"停止"，Done 事件更新状态与 chips；空态有引导文案；
- **诊断**：`AF_GUI_CRASHLOG=1` 打印崩溃地址与模块（配合 `/MAP` 生成的 map 文件定位）。
