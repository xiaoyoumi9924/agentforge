#  从零开始手写一个 AI Agent 

用 C++20 一步步搭出一个能读代码、改代码、跑命令的 Windows Agent：包含详细教程和源代码；
SSE 流式解析、工具注册表、上下文预算与压缩、六道安全闸门、Job Object 沙箱、仓库索引、
会话持久化，最后接上 CLI、终端 UI、Qt 桌面端和浏览器界面。

![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078d4)
![Language](https://img.shields.io/badge/C%2B%2B-20-00599c)
![License](https://img.shields.io/badge/license-MIT-3fb950)


![浏览器端界面](03-text/08-web-ui/images/web-main.png)


## 快速开始

以下是一个完整的初级agent，现已实现web功能，更多功能有待开发~~~

### 第一步：安装

目前仅支持windows系统。

**方式一：PowerShell**

```powershell
irm https://raw.githubusercontent.com/xiaoyoumi9924/agentforge/main/install.ps1 | iex
```

**方式二：手动下载** —— 到 [Releases 页面](../../releases/latest) 取 `AgentForge-Web-v1.0.0.zip`
，解压后双击 `run-web.bat`。

### 第二步：连接模型

启动后自动打开浏览器并访问 `http://127.0.0.1:8080`，终端提示粘贴 API Key，
默认对接 DeepSeek。




## 项目结构

```
. ├── 01-course/ 课程原始材料：12 讲课件与逐讲源码 │ ├── courseware/ 前导 7 份 PDF + 主线 10 份 PDF + 4 份 PPTX │ ├── code/ 逐讲源码快照，L01 … L12 │ └── README.md 12 讲索引与版权说明 ├── 02-agentforge/ 完整工程（第 12 讲成品） │ ├── src/ 核心：传输、模型、循环、工具、沙箱、索引、会话 │ ├── gui/ Qt 6 桌面端 │ ├── plugins/ C ABI 插件示例 │ ├── tests/ mock 服务端、契约自检、端到端评测 │ ├── docs/ 13 份设计说明与评估报告 │ ├── configs/ 配置样例 │ ├── scripts/ build.ps1 / build_gui.ps1 / package.ps1 │ └── thirdparty/ 单头依赖（nlohmann/json） ├── 03-text/ 大作业 │ ├── 大作业-题目.pdf 题目原文（十道题） │ └── 08-web-ui/ 第八题：浏览器端实现 ├── deploy/ 安装与发布脚本 ├── docs/ 使用手册 ├── install.ps1 一键安装（PowerShell） ├── install.sh 一键安装（Git Bash） ├── LICENSE MIT └── README.md
```



## 文档

- [**使用手册**](docs/使用手册.md) —— 安装细节、界面操作、任务写法、全部配置项、命令行参数、故障排查
- [课程材料索引](01-course/README.md) —— 前导课程与主线 12 讲课件、逐讲源码
- [Web 后端实现说明](03-text/08-web-ui/README.md) —— 第八题的设计与验证记录

## 许可

代码以 [MIT License](LICENSE) 开源。`01-course/courseware/` 与 `01-course/code/` 的课程材料
版权归课程与授课教师所有，仅用于教学，详见 [NOTICE](NOTICE)。
