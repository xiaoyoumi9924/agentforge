# Agent 配置与 Skills（提交附件）

## 1. Agent 配置（模块 ⑩）

### 1.1 `af_config.json`（工作目录，可选）

```json
{
  "provider": {
    "kind": "openai",
    "baseUrl": "https://api.deepseek.com/v1",
    "model": "deepseek-chat"
  },
  "budget": {
    "contextWindow": 200000,
    "reserveForReply": 16384,
    "maxOutputTokens": 8192
  },
  "sandbox": {
    "enabled": true,
    "allowNetwork": false
  },
  "logPath": "agentforge.log",
  "sessionId": "default"
}
```

### 1.2 环境变量（覆盖配置文件）

| 变量 | 说明 | 默认 |
|---|---|---|
| `OPENAI_API_KEY` | 模型 API Key（优先级最高） | 空 |
| `OPENAI_BASE_URL` | OpenAI 兼容端点 | 官方 |
| `AF_MODEL` | 模型名 | gpt-4o-mini |
| `AF_LOG` | 日志路径 | agentforge.log |
| `AF_PLUGIN` | 插件 DLL 路径 | 空 |
| `AF_SESSION` | 会话名（SQLite 持久化） | default |
| `AF_CONTEXT_WINDOW` | 上下文窗口 | 200000 |

### 1.3 密钥管理（DPAPI）

```powershell
agent.exe --set-key sk-...    # 加密落盘 .af_secret（DPAPI，当前用户）
agent.exe --config            # 查看有效配置（密钥打码）
agent.exe --clear-key         # 清除
```

### 1.4 权限规则（deny 永远赢）

| 规则 | 效果 |
|---|---|
| `Deny  Bash(rm -rf *)` | 删除类命令一律拒绝 |
| `Allow Read` | 只读工具直接放行 |
| fallback `Ask` | Write/Edit/Bash 需人工批准（fail-closed） |

## 2. Skills（项目级约定，供 AI 与组员共用）

Skills = 把"每次都要做的正确做法"固化成文档/脚本，让 AI 不再重复踩坑。

### S1 编译 skill（CLI）

- 入口：`build.ps1`（vswhere 定位 MSVC → 注册表 KitsRoot10 → 本地 NuGet `thirdparty/winsdk` 兜底）；
- 必须 `/utf-8`（无 BOM UTF-8 源码 + 中文注释）、`/std:c++20`；
- 产出：`build\agent.exe`、`build\contract_check.exe`。

### S2 编译 skill（GUI）

- 入口：`build_gui.ps1`（Qt 6.8.3 + CMake + Ninja + windeployqt，含 offscreen 插件）；
- 依赖：`C:\Qt\6.8.3\msvc2022_64`、`C:\Qt\Tools\CMake_64`、`python -m ninja`；
- 产出：`build_gui\AgentForgeGUI.exe`。

### S3 验证 skill（提交门槛）

- 每次改动后必须全绿：`agent.exe --selftest`（42 项）+ 对应 `verify_L*.ps1`；
- 回归评测：`tests/evaluate.ps1`（4/4）；
- 全部通过才允许 git commit。

### S4 目录约定

```
src/      核心（契约层 types.hpp 不依赖任何项目文件）
gui/      Qt 界面（复用 src 核心）
tests/    验证脚本 + MockLLM 假服务端
plugins/  C ABI 示例插件
docs/     设计文档（SRS/HLD/SDD/各阶段说明）
thirdparty/  nlohmann/json.hpp、sqlite3.c（本地提供，不进 git）
```

### S5 提交流程

`git add -A` → `git commit -m "<讲次>: <内容> (<验证结果>)"` → 保持工作区干净。

## 3. 这套配置在展示时的对应关系

- "Skills" → 本文档 S1-S5 + 对应脚本；
- "Agent 配置" → 1.1-1.4 的配置文件/环境变量/权限规则；
- 演示时打开本文件与 `af_config.json` 即可覆盖考核点。
