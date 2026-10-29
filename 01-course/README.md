# 课程材料

本目录保存课程原始材料，未作任何改写。

```
01-course/
├── courseware/
│   ├── intro/    前导课程 7 份（PDF）
│   └── main/     主线 12 讲课件（10 PDF + 4 PPTX）
└── code/         每一讲的源码快照，按讲次编号
```

## 前导课程

| 文件 | 内容 |
| --- | --- |
| `000课程简介.pdf` | 课程目标、考核方式、环境要求 |
| `001 AI全流程软件工程.pdf` | AI 参与软件工程全流程的现状与边界 |
| `002 会话流程与缓存.pdf` | 对话式交互的上下文结构与缓存机制 |
| `003 AgentForge.pdf` | 本项目的能力边界与整体架构 |
| `004 MockLLM.pdf` | 零成本验证：用假模型驱动完整链路 |
| `010 初识AI Coding能力.pdf` | AI 编码工具的能力与失效场景 |
| `020 最小闭环.pdf` | 从一句话到可运行程序的最小路径 |

## 主线 12 讲

每讲完成一个可运行的能力增量，`code/` 下是对应的源码快照。

| # | 课件 | 源码 | 本讲完成的能力 |
| :-: | --- | --- | --- |
| 01 | [pptx](courseware/main/L01-需求与总体设计.pptx) | [L01](code/L01-需求与总体设计) | 需求与总体设计：13 个模块、不变量、验收线 |
| 02 | [pptx](courseware/main/L02-模块设计与核心契约.pptx) | [L02](code/L02-模块设计与核心契约) | 冻结模块接口，写契约自检 |
| 03 | [pdf](courseware/main/L03-v0.pdf) · [pptx](courseware/main/L03-v0能对话.pptx) | [L03](code/L03-v0能对话) | v0：SSE 流式对话 |
| 04 | [pdf](courseware/main/L04-v1.pdf) · [pptx](courseware/main/L04-v1能读.pptx) | [L04](code/L04-v1能读) | v1：工具调用、C ABI 插件、权限门 |
| 05 | [pdf](courseware/main/L05-v2.pdf) | [L05](code/L05-v2v3能改能自主循环) | v2/v3：文件编辑、自主循环、上下文预算 |
| 06 | [pdf](courseware/main/L06-v4.pdf) | [L06](code/L06-v4能刹车) | v4：六道闸门、Job Object 沙箱 |
| 07 | [pdf](courseware/main/L07-v5.pdf) | [L07](code/L07-v5有记忆) | v5：会话持久化与上下文压缩 |
| 08 | [pdf](courseware/main/L08-v6.pdf) | [L08](code/L08-v6有地图) | v6：仓库索引与符号检索 |
| 09 | [pdf](courseware/main/L09-v7.pdf) | [L09](code/L09-v7有面目) | v7：终端界面 |
| 10 | [pdf](courseware/main/L10-v8.pdf) | [L10](code/L10-v8能存档) | v8：配置与密钥管理 |
| 11 | [pdf](courseware/main/L11-v9.pdf) | [L11](code/L11-v9会分身) | v9：子代理 |
| 12 | [pdf](courseware/main/L12-v10.pdf) | [L12](code/L12-v10有仪表盘) | v10：评测与交付 |

## 版权

课件与逐讲源码版权归课程与授课教师所有，仅用于教学，不适用仓库根目录的 MIT 许可。
详见 [NOTICE](../NOTICE)。
