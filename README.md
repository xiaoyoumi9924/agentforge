# AgentForge

一门课，12 讲，从一副空骨架到一个能读代码、改代码、执行代码，还带桌面端和浏览器端的 AI Agent。

![浏览器端](text/08-web-ui/images/web-main.png)

## 这是什么

三样东西放在一起：

- **课程**（`course/`）—— 12 讲的原始课件，以及每一讲的源码，全是老师给的那份，未做改写
- **实现**（`app/`）—— 跟着这 12 讲做出来的完整工程，clone 下来就能编译
- **作业**（`text/`）—— 大作业原文（十个题目）+ 我们选的第八个：给 Agent 加一个浏览器前端

## 从这里开始

| 你有多少时间 | 从哪进 |
|---|---|
| **5 分钟** | `cd app && ./build.ps1`，先看成品 |
| **3 小时** | 从 [`course/code/L01-需求与总体设计`](course/code/L01-需求与总体设计) 顺序读源码 |
| **只想看题** | [`text/`](text) —— 十道题（[PDF](text/大作业-题目.pdf)），我们选了第八个 |

## 12 讲

| # | 课件 | 源码 | 这一步做什么 |
|---|---|---|---|
| 01 | [pptx](course/courseware/main/L01-需求与总体设计.pptx) | [L01-需求与总体设计](course/code/L01-需求与总体设计) | 定边界：13 个模块、不变量、验收线 |
| 02 | [pptx](course/courseware/main/L02-模块设计与核心契约.pptx) | [L02-模块设计与核心契约](course/code/L02-模块设计与核心契约) | 冻结接口，写契约自检 |
| 03 | [pdf](course/courseware/main/L03-v0.pdf) | [L03-v0能对话](course/code/L03-v0能对话) | 第一次流式回答（SSE） |
| 04 | [pdf](course/courseware/main/L04-v1.pdf) | [L04-v1能读](course/code/L04-v1能读) | 工具调用、C ABI 插件、权限门 |
| 05 | [pdf](course/courseware/main/L05-v2.pdf) | [L05-v2v3能改能自主循环](course/code/L05-v2v3能改能自主循环) | 自主循环与上下文预算 |
| 06 | [pdf](course/courseware/main/L06-v4.pdf) | [L06-v4能刹车](course/code/L06-v4能刹车) | 六道闸门、Job Object 沙箱 |
| 07 | [pdf](course/courseware/main/L07-v5.pdf) | [L07-v5有记忆](course/code/L07-v5有记忆) | 会话持久化与压缩 |
| 08 | [pdf](course/courseware/main/L08-v6.pdf) | [L08-v6有地图](course/code/L08-v6有地图) | 仓库索引 |
| 09 | [pdf](course/courseware/main/L09-v7.pdf) | [L09-v7有面目](course/code/L09-v7有面目) | 终端界面 |
| 10 | [pdf](course/courseware/main/L10-v8.pdf) | [L10-v8能存档](course/code/L10-v8能存档) | 配置与密钥 |
| 11 | [pdf](course/courseware/main/L11-v9.pdf) | [L11-v9会分身](course/code/L11-v9会分身) | 子代理 |
| 12 | [pdf](course/courseware/main/L12-v10.pdf) | [L12-v10有仪表盘](course/code/L12-v10有仪表盘) | 评测与交付 |

开课前还有 7 个前导 PDF（课程简介、AI 全流程、会话与缓存、AgentForge 总览、MockLLM、AI Coding、最小闭环），在 [`course/courseware/intro/`](course/courseware/intro)。

## 它由哪些部分组成

```
模型层    provider（OpenAI 兼容）── SSE 流式解析 ── MockLLM
循环层    AgentLoop ── 上下文分层预算 / 压缩 / 降级 ── 六道闸门
工具层    ToolRegistry ── Read / Write / Edit / Run ── C ABI 插件
索引层    仓库索引（符号 + include 边）── 检索 ── L3 上下文
界面层    CLI ── 终端 UI ── Qt 6 桌面端 ── 浏览器端（WebSocket）
```

## 构建

前置：Windows 10/11、Visual Studio 2022（「使用 C++ 的桌面开发」工作负载，含 Windows SDK）。

```powershell
cd app
./build.ps1              # → build\agent.exe
./build_gui.ps1          # → build_gui\AgentForgeGUI.exe（需要 Qt 6.8）
```

## 没有 API Key 也能跑

所有验证都走 `app/tests/mock_server.py`（OpenAI 兼容假服务端），零费用、可重复：

```powershell
cd app
python tests/mock_server.py &
./build/agent.exe --selftest     # 115 项进程内自检
./tests/evaluate.ps1             # 4/4 端到端用例
```

接真实模型时设环境变量即可，代码不改：

```powershell
$env:OPENAI_BASE_URL="https://api.deepseek.com/v1"
$env:OPENAI_API_KEY="sk-..."
```

## 目录

```
course/courseware/intro/   前导课（7 个 PDF）
course/courseware/main/    主线课件（10 个 PDF + 4 个 PPTX）
course/code/               L1–L12 每一讲的源码
app/                       完整实现：核心、GUI、插件、测试、构建脚本
text/                      大作业原文 + 第八题的实现
```

## 已知局限

- 沙箱是 Job Object，提供进程树强杀与资源上限，**不提供**网络 / 文件系统的内核级边界
- 仓库索引是词法级，符号抽取有误报，不追求语法完备
- 索引全量重建，未做增量
- GUI 的 diff 面板是「参数 + 结果对照」，没有逐行高亮
- 评测默认 MockLLM；真实模型走环境变量接入，未在评测中计费运行

## 许可

代码 MIT。`course/slides/` 的课件与 `course/code/` 的课程源码版权归课程与授课教师，仅用于教学。详见 [NOTICE](NOTICE)。
