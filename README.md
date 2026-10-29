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

Windows 10 / 11，无需管理员权限，不写注册表、不改 PATH。以下三种方式任选其一。

**方式一：PowerShell**

```powershell
irm https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.ps1 | iex
```

**方式二：Git Bash / MSYS**

```bash
curl -fsSL https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.sh | bash
```

**方式三：手动下载** —— 到 [Releases 页面](../../releases/latest) 下载 `AgentForge-Web-v1.0.0.zip`（1.2 MB），
解压后双击 `run-web.bat`。

默认安装到 `%LOCALAPPDATA%\AgentForge`，卸载时删除该目录即可。若下载卡住，先开启代理再执行
（脚本会读取 `HTTPS_PROXY`），或直接走方式三。

### 第二步：连接模型

安装完成后自动打开浏览器并访问 `http://127.0.0.1:8080`，终端提示粘贴 API Key，
默认对接 DeepSeek（`https://api.deepseek.com/v1`）。Key 经 Windows DPAPI 加密后存为 `key.dat`，
仅当前 Windows 用户可解密，复制到其他账户无法使用。

| 配置项 | 修改方式 |
| --- | --- |
| 模型 | 环境变量 `AF_MODEL`，如 `deepseek-reasoner` |
| 端点 | 环境变量 `OPENAI_BASE_URL`，支持任意 OpenAI 兼容服务 |
| API Key | 删除 `key.dat`，重启后重新输入 |
| 工作目录 | `run-web.ps1` 中的 `workspace\web` |

## 使用

在输入框用自然语言描述任务，Agent 自行规划调用哪些工具：

- 「读一下 `data.csv`，按产品汇总销售额，写到 `summary.md`」
- 「新建 `hello.cpp`，打印 1 到 5，然后编译运行」

写文件、执行命令、访问网络等操作会先弹出权限确认，可单次放行，也可本次会话全部放行；
运行过程中可随时中断。所有产出固定在安装目录的 `workspace/web/` 下，与启动位置无关：

```
AgentForge/
├── agent.exe
├── run-web.bat         启动入口
├── key.dat             DPAPI 加密的 API Key
└── workspace/
    └── web/            工作目录，Agent 的全部产出
```

## 架构

自下而上五层，接口冻结后互不穿透：

```
模型层    provider（OpenAI 兼容）──── SSE 流式解析 ──── MockLLM
循环层    AgentLoop ── 上下文分层预算 / 压缩 / 降级 ── 六道闸门
工具层    ToolRegistry ── Read / Write / Edit / Run ── C ABI 插件
索引层    仓库索引（符号 + include 边）── 检索 ── L3 上下文
界面层    CLI ── 终端 UI ── Qt 6 桌面端 ── 浏览器端（WebSocket）
```

四套界面共用同一个 `AgentLoop`，因此行为完全一致，只有交互形式不同。

## 项目结构

```
.
├── 01-course/                课程材料
│   ├── courseware/
│   │   ├── intro/            7 份前导课件（PDF）
│   │   └── main/             主线 12 讲课件（10 PDF + 4 PPTX）
│   └── code/                 每一讲的源码快照，按讲次编号
├── 02-agentforge/            完整工程
│   ├── src/                  核心：传输、模型、循环、工具、索引、会话
│   ├── gui/                  Qt 6 桌面端
│   ├── plugins/              C ABI 插件示例
│   ├── tests/                mock 服务端、进程内自检、端到端评测
│   ├── docs/                 设计文档
│   └── scripts/              build.ps1 / build_gui.ps1 / package.ps1
├── 03-text/                  大作业
│   ├── 大作业-题目.pdf        题目原文（十道题）
│   └── 08-web-ui/            第八题「Web 后端」的实现
├── deploy/                   安装与发布脚本
├── install.ps1 / install.sh  一键安装
└── LICENSE                   MIT
```

## 课程材料

主线 12 讲，每讲完成一个可运行的能力增量，`01-course/code/` 下是与之对应的源码快照。

| # | 课件 | 源码 | 本讲完成的能力 |
| :-: | --- | --- | --- |
| 01 | [pptx](01-course/courseware/main/L01-需求与总体设计.pptx) | [L01](01-course/code/L01-需求与总体设计) | 需求与总体设计：13 个模块、不变量、验收线 |
| 02 | [pptx](01-course/courseware/main/L02-模块设计与核心契约.pptx) | [L02](01-course/code/L02-模块设计与核心契约) | 冻结模块接口，写契约自检 |
| 03 | [pdf](01-course/courseware/main/L03-v0.pdf) | [L03](01-course/code/L03-v0能对话) | v0：SSE 流式对话 |
| 04 | [pdf](01-course/courseware/main/L04-v1.pdf) | [L04](01-course/code/L04-v1能读) | v1：工具调用、C ABI 插件、权限门 |
| 05 | [pdf](01-course/courseware/main/L05-v2.pdf) | [L05](01-course/code/L05-v2v3能改能自主循环) | v2/v3：文件编辑与自主循环、上下文预算 |
| 06 | [pdf](01-course/courseware/main/L06-v4.pdf) | [L06](01-course/code/L06-v4能刹车) | v4：六道闸门、Job Object 沙箱 |
| 07 | [pdf](01-course/courseware/main/L07-v5.pdf) | [L07](01-course/code/L07-v5有记忆) | v5：会话持久化与上下文压缩 |
| 08 | [pdf](01-course/courseware/main/L08-v6.pdf) | [L08](01-course/code/L08-v6有地图) | v6：仓库索引与符号检索 |
| 09 | [pdf](01-course/courseware/main/L09-v7.pdf) | [L09](01-course/code/L09-v7有面目) | v7：终端界面 |
| 10 | [pdf](01-course/courseware/main/L10-v8.pdf) | [L10](01-course/code/L10-v8能存档) | v8：配置与密钥管理 |
| 11 | [pdf](01-course/courseware/main/L11-v9.pdf) | [L11](01-course/code/L11-v9会分身) | v9：子代理 |
| 12 | [pdf](01-course/courseware/main/L12-v10.pdf) | [L12](01-course/code/L12-v10有仪表盘) | v10：评测与交付 |

## 从源码构建

需要 Visual Studio 2022（「使用 C++ 的桌面开发」工作负载，含 Windows SDK）；Qt 6.8 仅桌面端需要，
浏览器端与 CLI 不依赖 Qt。

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
- 仓库索引为词法级，符号抽取存在误报，不追求语法完备
- 索引全量重建，大仓库首次打开偏慢，未做增量
- GUI 的 diff 面板展示「参数 + 结果」对照，没有逐行高亮
- 评测默认使用 MockLLM，真实模型经环境变量接入，未在评测中计费运行

## 许可

代码以 [MIT License](LICENSE) 开源。`01-course/courseware/` 与 `01-course/code/` 的课程材料
版权归课程与授课教师所有，仅用于教学，详见 [NOTICE](NOTICE)。
