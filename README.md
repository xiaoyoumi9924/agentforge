# AgentForge

**一个能读代码、改代码、执行代码的 AI Agent。** Windows 原生 C++ 实现，终端、桌面端、浏览器端三套界面，同一套内核。

![浏览器端界面](03-text/08-web-ui/images/web-main.png)

---

## 一、这是什么

这不是一个「调 API 聊天」的壳子。给它一个任务，它会自己决定读哪个文件、改哪一行、跑什么命令，然后把结果交回来。

- **能动手** —— 内置 Read / Write / Edit / Run 四类工具，改完可以立刻编译运行验证
- **看得见** —— 每一步都渲染成卡片：工具名、参数、耗时、输出，全过程可回放
- **刹得住** —— 六道闸门拦截危险操作，Job Object 限制进程树与资源，权限弹窗逐个确认
- **记得住** —— 会话落盘 SQLite，上下文按预算分层压缩，长任务不丢线索

整个项目从零手写：WinHTTP 收发、SSE 流式解析、工具注册表、仓库索引、Qt 界面，没有用任何 Agent 框架。

---

## 二、三分钟跑起来

Windows 10 / 11，**不需要管理员权限**，不改注册表，不污染 PATH。

### 第 1 步 · 安装

三选一，效果一样。

**A. PowerShell**

```powershell
irm https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.ps1 | iex
```

**B. Git Bash / MSYS**

```bash
curl -fsSL https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.sh | bash
```

**C. 手动** —— 到 [Releases](https://github.com/xiaoyoumi9924/agentforge/releases/latest) 下载 `AgentForge-Web-v1.0.0.zip`（1.2 MB），解压后双击 `run-web.bat`。

装到 `%LOCALAPPDATA%\AgentForge`，卸载就是删掉这个文件夹。

> 下载卡住多半是网络问题：先开代理再执行（脚本会读 `HTTPS_PROXY`），或者直接走 C 方案。

### 第 2 步 · 填 Key

第一次启动，终端会让你粘贴 API Key。默认接 DeepSeek（`https://api.deepseek.com/v1`）。

Key 用 Windows DPAPI 加密后存成 `key.dat`，只有当前 Windows 用户能解开，别的账户拷走也用不了。

### 第 3 步 · 打开

浏览器自动跳到 **http://127.0.0.1:8080**。没自动打开就手动访问一次，服务是常驻的，关掉浏览器不会停。

---

## 三、用起来是什么样

### ① 把任务说清楚

在输入框敲自然语言，越具体越好：

- 「读一下 `data.csv`，按产品汇总销售额，写到 `summary.md`」
- 「在这个目录新建 `hello.cpp`，打印 1 到 5，然后编译运行它」

### ② 看它自己动手

模型会先想调哪个工具，再逐个执行。每个动作一张卡片，右侧实时展开输出；跑偏了随时点「停止」。

危险动作（删文件、装依赖、访问网络）会弹权限框，可以单次放行，也可以本次会话全部放行。

### ③ 结果落在固定目录

```
%LOCALAPPDATA%\AgentForge\
├── agent.exe
├── run-web.bat            启动入口
├── key.dat                DPAPI 加密的 Key
└── workspace\
    └── web\               ★ 所有产出都在这里
```

不管从桌面快捷方式、开始菜单还是命令行启动，工作目录都锁死在 `workspace\web`，不会写到满硬盘都是。

---

## 四、改设置

| 你想改 | 怎么改 |
|---|---|
| 换模型 | 环境变量 `AF_MODEL`（如 `deepseek-reasoner`） |
| 换端点 | 环境变量 `OPENAI_BASE_URL`（任何 OpenAI 兼容服务） |
| 换 Key | 删掉 `key.dat`，重启后重新粘贴 |
| 换工作目录 | 编辑 `run-web.ps1` 里的 `workspace\web` |
| 换端口 | 编辑 `run-web.ps1` 里的 `--web` 参数 |

---

## 五、它是怎么搭起来的

自下而上五层，每层职责单一，接口冻结后上层不再关心下层实现。

```
模型层    provider（OpenAI 兼容）── SSE 流式解析 ── MockLLM
循环层    AgentLoop ── 上下文分层预算 / 压缩 / 降级 ── 六道闸门
工具层    ToolRegistry ── Read / Write / Edit / Run ── C ABI 插件
索引层    仓库索引（符号 + include 边）── 检索 ── L3 上下文
界面层    CLI ── 终端 UI ── Qt 6 桌面端 ── 浏览器端（WebSocket）
```

界面层四种形态共用同一个 `AgentLoop`，所以行为完全一致，只是交互方式不同。

---

## 六、仓库怎么逛

```
agentforge/
├── 01-course/      课程材料（老师原版，按讲次编号）
├── 02-agentforge/  完整实现（核心 + GUI + 插件 + 测试）
├── 03-text/        大作业题目与第八题实现
└── deploy/         安装与发布脚本
```

| 目录 | 内容 |
|---|---|
| `01-course/` | `courseware/` 12 讲课件（7 前导 + 10 主线 + 4 PPTX），`code/` 每讲源码快照 |
| `02-agentforge/` | 可直接编译的工程：`src/ gui/ plugins/ tests/ docs/ scripts/` |
| `03-text/` | `大作业-题目.pdf`（十道题）+ `08-web-ui/` 第八题实现 |
| `deploy/` | `run-web.ps1`、打包脚本、Release 模板 |

<details>
<summary><b>12 讲，每一讲解决什么问题（点开）</b></summary>

| # | 课件 | 源码 | 这一步做什么 |
|---|---|---|---|
| 01 | [pptx](01-course/courseware/main/L01-需求与总体设计.pptx) | [L01](01-course/code/L01-需求与总体设计) | 定边界：13 个模块、不变量、验收线 |
| 02 | [pptx](01-course/courseware/main/L02-模块设计与核心契约.pptx) | [L02](01-course/code/L02-模块设计与核心契约) | 冻结接口，写契约自检 |
| 03 | [pdf](01-course/courseware/main/L03-v0.pdf) | [L03](01-course/code/L03-v0能对话) | 第一次流式回答（SSE） |
| 04 | [pdf](01-course/courseware/main/L04-v1.pdf) | [L04](01-course/code/L04-v1能读) | 工具调用、C ABI 插件、权限门 |
| 05 | [pdf](01-course/courseware/main/L05-v2.pdf) | [L05](01-course/code/L05-v2v3能改能自主循环) | 自主循环与上下文预算 |
| 06 | [pdf](01-course/courseware/main/L06-v4.pdf) | [L06](01-course/code/L06-v4能刹车) | 六道闸门、Job Object 沙箱 |
| 07 | [pdf](01-course/courseware/main/L07-v5.pdf) | [L07](01-course/code/L07-v5有记忆) | 会话持久化与压缩 |
| 08 | [pdf](01-course/courseware/main/L08-v6.pdf) | [L08](01-course/code/L08-v6有地图) | 仓库索引 |
| 09 | [pdf](01-course/courseware/main/L09-v7.pdf) | [L09](01-course/code/L09-v7有面目) | 终端界面 |
| 10 | [pdf](01-course/courseware/main/L10-v8.pdf) | [L10](01-course/code/L10-v8能存档) | 配置与密钥 |
| 11 | [pdf](01-course/courseware/main/L11-v9.pdf) | [L11](01-course/code/L11-v9会分身) | 子代理 |
| 12 | [pdf](01-course/courseware/main/L12-v10.pdf) | [L12](01-course/code/L12-v10有仪表盘) | 评测与交付 |

</details>

---

## 七、自己编译

### 前置

- **Visual Studio 2022**，勾选「使用 C++ 的桌面开发」（含 Windows SDK）
- **Qt 6.8**，只有桌面端 GUI 需要
- 浏览器端与 CLI **不需要 Qt**

### 编译

```powershell
cd 02-agentforge
./scripts/build.ps1          # → build\agent.exe
./scripts/build_gui.ps1      # → build_gui\AgentForgeGUI.exe（需 Qt）
```

### 验证（不用 API Key，零费用）

`tests/mock_server.py` 是一个 OpenAI 兼容的假服务端，可重复跑。

```powershell
python tests/mock_server.py &
./build/agent.exe --selftest     # 115 项进程内自检
./tests/evaluate.ps1             # 4/4 端到端用例
```

---

## 八、已知局限

- **沙箱是进程级的** —— Job Object 能强杀进程树、限制资源，但不提供网络与文件系统的内核边界
- **索引是词法级的** —— 符号抽取会有误报，不追求语法完备
- **索引全量重建** —— 大仓库首次打开偏慢，没做增量
- **Diff 没有逐行高亮** —— GUI 面板展示的是「参数 + 结果」对照
- **评测跑的是 MockLLM** —— 真实模型走环境变量接入，未在评测中计费运行

---

## 九、许可

代码 [MIT](LICENSE)。`01-course/courseware/` 与 `01-course/code/` 的课程材料版权归课程与授课教师所有，仅用于教学。

详见 [NOTICE](NOTICE)。
