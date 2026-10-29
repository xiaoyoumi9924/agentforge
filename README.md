# AgentForge：Windows 原生的 AI 编程 Agent

![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078d4)
![Language](https://img.shields.io/badge/C%2B%2B-20-00599c)
![License](https://img.shields.io/badge/license-MIT-3fb950)
![Release](https://img.shields.io/badge/release-v1.0.0-8957e5)

从 WinHTTP 发包一路搭到浏览器界面：一个纯 C++ 实现的 Agent 内核，覆盖 SSE 流式解析、工具注册表、
上下文分层预算与压缩、六道安全闸门、Job Object 沙箱、仓库索引、会话持久化，
以及 CLI、终端 UI、Qt 桌面端、WebSocket 浏览器端四套界面。不依赖 LangChain，不依赖 Python。

![浏览器端界面](03-text/08-web-ui/images/web-main.png)

<sub>浏览器端（`agent.exe --web`）：左侧会话列表，中间任务流，每次工具调用渲染为一张卡片，展开可见参数、耗时与输出。</sub>

## 快速开始

### 第一步：安装

Windows 10 / 11，无需管理员权限，不写注册表、不改 PATH。

**方式一：PowerShell**

```powershell
irm https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.ps1 | iex
```

**方式二：Git Bash / MSYS**

```bash
curl -fsSL https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.sh | bash
```

**方式三：手动下载** —— 到 [Releases 页面](../../releases/latest) 取 `AgentForge-Web-v1.0.0.zip`
（1.2 MB），解压后双击 `run-web.bat`。装到 `%LOCALAPPDATA%\AgentForge`，卸载时删除该目录即可。

### 第二步：连接模型

启动后自动打开浏览器并访问 `http://127.0.0.1:8080`，终端提示粘贴 API Key，
默认对接 DeepSeek（`https://api.deepseek.com/v1`）。Key 经 Windows DPAPI 加密后存为 `key.dat`，
仅当前 Windows 用户可解密，复制到其他账户无法使用。

## 使用

在输入框用自然语言描述任务，Agent 自行规划调用哪些工具：

- 「读一下 `data.csv`，按产品汇总销售额，写到 `summary.md`」
- 「新建 `hello.cpp`，打印 1 到 5，然后编译运行」

写文件、执行命令前会弹窗确认，也可切换为「自动允许」；终端窗口是服务本体，关掉即停止。
所有产出固定在安装目录的 `workspace/web/` 下，与启动位置无关。

## 架构

自下而上五层，接口冻结后互不穿透：

```
模型层    provider（OpenAI 兼容）──── SSE 流式解析 ──── MockLLM
循环层    AgentLoop ── 上下文分层预算 / 压缩 / 降级 ── 六道闸门
工具层    ToolRegistry ── Read / Write / Edit / Run ── C ABI 插件
索引层    仓库索引（符号 + include 边）── 检索 ── L3 上下文
界面层    CLI ── 终端 UI ── Qt 6 桌面端 ── 浏览器端（WebSocket）
```

四套界面共用同一个 `AgentLoop`，行为一致，只有交互形式不同。

## 项目结构

```
.
├── 01-course/                课程材料：12 讲课件与逐讲源码
├── 02-agentforge/            完整工程（src / gui / plugins / tests / docs / scripts）
├── 03-text/                  大作业：题目原文与第八题（Web 后端）实现
├── deploy/                   安装与发布脚本
├── docs/                     使用手册
├── install.ps1 / install.sh  一键安装
└── LICENSE                   MIT
```

## 从源码构建

需要 Visual Studio 2022（「使用 C++ 的桌面开发」工作负载，含 Windows SDK）；Qt 6.8 仅桌面端需要。

```powershell
cd 02-agentforge
./scripts/build.ps1          # → build\agent.exe
./scripts/build_gui.ps1      # → build_gui\AgentForgeGUI.exe（需 Qt）
```

`tests/mock_server.py` 是与 OpenAI 兼容的本地假服务端，不用 API Key 也能复现全部行为：

```powershell
python tests/mock_server.py &
./build/agent.exe --selftest     # 115 项进程内自检
./tests/evaluate.ps1             # 4/4 端到端用例
```

## 已知限制

- 沙箱基于 Job Object，提供进程树强杀与资源上限，不提供网络与文件系统的内核级边界
- 仓库索引为词法级，符号抽取存在误报；全量重建，未做增量
- GUI 的 diff 面板展示「参数 + 结果」对照，没有逐行高亮
- 评测默认使用 MockLLM，真实模型经环境变量接入，未在评测中计费运行

## 文档

- [**使用手册**](docs/使用手册.md) —— 安装细节、界面操作、任务写法、全部配置项、命令行参数、故障排查
- [课程材料索引](01-course/README.md) —— 前导课程与主线 12 讲课件、逐讲源码
- [Web 后端实现说明](03-text/08-web-ui/README.md) —— 第八题的设计与验证记录

## 许可

代码以 [MIT License](LICENSE) 开源。`01-course/courseware/` 与 `01-course/code/` 的课程材料
版权归课程与授课教师所有，仅用于教学，详见 [NOTICE](NOTICE)。
