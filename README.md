# AgentForge

一个能读代码、改代码、执行代码的 AI Agent —— Windows 原生 C++ 实现，带桌面端和浏览器端。

![浏览器端](03-text/08-web-ui/images/web-main.png)

## 一键运行

**PowerShell**（Windows 10/11，无需管理员）

```powershell
irm https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.ps1 | iex
```

**Git Bash / MSYS**

```bash
curl -fsSL https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.sh | bash
```

**或者手动下载**：[`AgentForge-Web-v1.0.0.zip`](https://github.com/xiaoyoumi9924/agentforge/releases/latest)（1.2 MB，解压后双击 `run-web.bat`）

装完会自动打开浏览器。第一次启动会问你要 API Key，默认接 DeepSeek `https://api.deepseek.com/v1`，
Key 用 Windows DPAPI 加密后存在本地，只有当前用户能解开。

> 国内网络如果卡在下载，先开代理再执行命令；或直接去 Releases 页面下载 zip。

## 用起来是什么样

浏览器打开 `http://127.0.0.1:8080`，在输入框写任务：

- 「读一下 `data.csv`，按产品汇总销售额，结果写到 `summary.md`」
- 「在这个目录里新建 hello.cpp，打印 1 到 5，然后编译运行它」

它会自己决定调哪个工具、改哪个文件、跑什么命令。每一步都会显示成卡片：工具名、参数、耗时、输出。
权限弹窗可以逐个放行，也可以设成「全部放行」。

**产出都在 `workspace/web/`** —— 不管从哪启动，文件都落在这个固定目录，不用满硬盘找。

| 你想… | 改这里 |
|---|---|
| 换模型 | 环境变量 `AF_MODEL` |
| 换端点 | 环境变量 `OPENAI_BASE_URL` |
| 换 Key | 删掉安装目录的 `key.dat` 再启动 |
| 换工作目录 | 改 `run-web.ps1` 里的 `workspace\web` |

## 它由哪些部分组成

```
模型层    provider（OpenAI 兼容）── SSE 流式解析 ── MockLLM
循环层    AgentLoop ── 上下文分层预算 / 压缩 / 降级 ── 六道闸门
工具层    ToolRegistry ── Read / Write / Edit / Run ── C ABI 插件
索引层    仓库索引（符号 + include 边）── 检索 ── L3 上下文
界面层    CLI ── 终端 UI ── Qt 6 桌面端 ── 浏览器端（WebSocket）
```

## 仓库里有什么

| 目录 | 内容 |
|---|---|
| `01-course/` | 12 讲的课件（7 个前导 PDF + 10 个主线 PDF + 4 个 PPTX）与每一讲的源码 |
| `02-agentforge/` | 完整实现：核心、Qt GUI、插件、测试、构建脚本 |
| `03-text/` | 大作业原文（十道题）+ 我们选的第八题（Web 后端）的实现 |
| `deploy/` | 一次性安装包与发布脚本 |

<details>
<summary><b>12 讲讲什么（点开）</b></summary>

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

## 从源码构建

前置：Visual Studio 2022（「使用 C++ 的桌面开发」工作负载，含 Windows SDK）；Qt 6.8 只有桌面端需要。

```powershell
cd 02-agentforge
./scripts/build.ps1              # → build\agent.exe
./scripts/build_gui.ps1          # → build_gui\AgentForgeGUI.exe
```

没有 API Key 也能验证：`tests/mock_server.py` 是 OpenAI 兼容的假服务端，零费用可重复。

```powershell
python tests/mock_server.py &
./build/agent.exe --selftest     # 115 项进程内自检
./tests/evaluate.ps1             # 4/4 端到端用例
```

## 已知局限

- 沙箱是 Job Object，提供进程树强杀与资源上限，**不提供**网络 / 文件系统的内核级边界
- 仓库索引是词法级，符号抽取有误报，不追求语法完备
- 索引全量重建，未做增量
- GUI 的 diff 面板是「参数 + 结果对照」，没有逐行高亮
- 评测默认 MockLLM；真实模型走环境变量接入，未在评测中计费运行

## 许可

代码 MIT。`01-course/courseware/` 与 `01-course/code/` 的课程材料版权归课程与授课教师，仅用于教学。
详见 [NOTICE](NOTICE)。
