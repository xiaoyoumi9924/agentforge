# AgentForge · 成品

第 12 讲的完整工程。想直接跑起来，从这里开始。

```powershell
./build.ps1          # → build\agent.exe
./build_gui.ps1      # → build_gui\AgentForgeGUI.exe（需要 Qt 6.8）
./build/agent.exe --selftest      # 115 项自检
```

> 仓库里只有这一份完整代码。`course/` 下是课件与每一讲的源码，`text/` 下是题目的改动。

## 能做什么

- 流式对话（SSE），兼容任何 OpenAI 兼容端点
- 工具调用：读、写、改、跑命令，外加 C ABI 插件
- 六道闸门：步数 / token / 墙钟 / 连续错误 / 单步超时 / 权限
- 仓库索引与检索：动手前先看相关代码
- 会话持久化：SQLite 存储，压缩后继续
- 两个前端：CLI 与 Qt 6 桌面端

## 目录

```
src/        核心：循环、工具、沙箱、上下文、索引、会话
gui/        Qt 6 桌面端
plugins/    C ABI 插件示例
tests/      契约自检、MockLLM、各讲验证脚本、端到端评测
configs/    模型与循环上限配置
docs/       设计文档与评估报告
```

## 没有 Key 也能跑

```powershell
python tests/mock_server.py &
./build/agent.exe "用三句话解释 KMP"
```

真实模型：

```powershell
$env:OPENAI_BASE_URL="https://api.deepseek.com/v1"
$env:OPENAI_API_KEY="sk-..."
```
