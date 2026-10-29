# 题目八 · Web UI 后端

把 AgentForge 的界面层扩出**第三个后端**：浏览器。流式输出、工具卡片、权限弹窗全在浏览器里，
而 `AgentLoop`、`ToolRegistry`、上下文预算、会话存储——一行都没改。

![界面](images/web-main.png)

## 题目要求的五个功能点

| # | 要求 | 实现 |
|---|---|---|
| 1 | 实现 WebRenderer：把 UiEvent 序列化成 JSON，经 WebSocket 推给浏览器 | `ui/tui_web.cpp` + `ui/web_json.cpp` |
| 2 | 写一个最小前端：HTML + JS，显示对话流、工具卡片、权限弹窗 | `ui/web_page.hpp`，单页，编译进 exe |
| 3 | 权限弹窗双向通信：点「允许」→ 回传 → 唤醒 Agent | `perm` 下行 / `permAnswer` 上行 |
| 4 | 和工厂接线：`AF_UI=web` 时返回 WebRenderer | `ui/tui_plain.cpp` 加一个分支，`main` 一行没动 |
| 5 | 非 TTY 时降级：没有浏览器也能跑 | 三层降级，见下 |

## 这个目录是什么

这里**不是**一份能独立编译的工程，而是相对 `app/` 的**改动集合**——新增的文件、以及被改过的文件。
看点在于「接口没动，只加了一个 Renderer 实现」。

想跑起来，把 `src/` 下的文件覆盖进 `app/` 对应位置（`main.cpp` 放 `src/`，`ui/` 整个替换），然后用 `app/build.ps1` 编译：

```powershell
cd app
./build.ps1
$env:OPENAI_API_KEY="sk-..."        # 或先起 tests/mock_server.py
.\build\agent.exe --ui web          # 自动打开 http://127.0.0.1:8080
```

没有浏览器也能验协议：

```powershell
./scripts/web_probe.ps1     # 不开浏览器，直接跑一遍 WebSocket 握手与事件流
```

## 三层降级

题目里最容易被忽略的一条是「非 TTY 时退回」。真实情况有三种，都要处理：

| 场景 | 行为 |
|---|---|
| 端口被占用 | 顺延端口，日志里说明 |
| 等不到浏览器 | 3 秒后退回 PlainRenderer，终端继续输出 |
| 运行中浏览器掉线 | Agent 不崩，权限按拒绝处理，剩余步骤跑完 |

`makeRenderer` 永远返回非空——这是设计上的硬约束。

## 验证

全量 115 项自检通过，其中 20 条是本题目新增（I-95 ~ I-105b），全部不需要浏览器、不需要网络。
题目八的自检在交付版工程里，本仓库只保留与 Web 后端相关的改动。

## 文件

```
src/
  main.cpp            改过的入口：加 --ui web 分支
  ui/tui_web.cpp      WebRenderer：事件 → JSON → 广播
  ui/web_server.cpp   WebSocket 服务端（握手、帧、广播、心跳）
  ui/web_json.cpp     事件编码（纯函数，无 IO）
  ui/web_page.hpp     单页前端（HTML + CSS + JS，编译进 exe）
  ui/tui_plain.cpp    工厂里加一个分支
scripts/
  build.ps1           交付版构建脚本（保留作参考）
  web_probe.ps1       不开浏览器的协议验收
images/
  web-main.png        界面截图
```
