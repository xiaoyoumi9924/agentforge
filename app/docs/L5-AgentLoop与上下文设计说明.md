# L5 AgentLoop 与上下文预算 —— 设计说明

| 项 | 内容 |
|---|---|
| 阶段 | L5 · AgentLoop + 上下文分层组装 + 四个工具 |
| 上游 | SDD（端口 ① ② ③ ④ ⑤），L4 工具调用 |
| 代码 | `src/agent_loop.{hpp,cpp}`、`src/context_budget.{hpp,cpp}`、`src/main.cpp` |
| 验收 | `tests/verify_L5.ps1`：selftest 18 PASS + Mock 冒烟 4 PASS |

## 1. 本讲核心变化

把 L4 的"临时 for 循环"正式抽象成 **AgentLoop**，并引入**上下文分层组装**：

| 维度 | L4 main 里的 for | L5 AgentLoop::run |
|---|---|---|
| 位置 | 藏在 main 里 | 独立类，可复用 |
| 依赖 | 直接抓 provider/registry/ui | 构造注入（引用），不持有 |
| 渲染 | 直接 std::cout | 通过 LoopHooks 抛事件 |
| 上下文 | 手写 req.system/req.messages | ContextAssembler::assemble 统一 |
| 状态 | 局部变量 history | Turn&（外部持有）+ 成员 spentTokens_ |
| 错误回喂 | 没有（直接 break） | 错误作为 observation 塞回历史，continue |

## 2. 上下文分层（ADR-5）

```
L0 系统提示     恒定 → 缓存前缀，永不裁剪
L1 项目知识     CLAUDE.md / rules
L2 仓库地图     符号骨架
L3 检索命中     检索结果块（相关优先）
L4 活动文件     正在编辑的文件全文
L5 对话历史     块级裁剪（assistant+tool 成组删除）
L6 用户输入     永远最后、永远完整
```

**顺序即缓存前缀**：稳定内容越靠前，模型注意力越靠后；L0/L1/L2 严格固定在前。

**超预算降级链**：全文 → 签名 → 符号名 + 路径 → 仅路径 → 丢弃。
直接一刀截断会产生"半个函数"这种有害内容——按签名/符号名降级至少保留语义线索。

## 3. 消息合法性守卫（G8）

协议硬要求：每条 tool 消息必须紧跟"请求了它那个 id 的 assistant 消息"，且不能被别的消息隔开。

| 类型 | 示例 | 后果 |
|---|---|---|
| ① 孤儿 tool | 裁剪时删了 assistant，留下 tool | 400 |
| ② 配对窗口被打断 | 中间插了一条 user | 400 |
| ③ 只回一半 | 请求 3 个工具，只回填 1 条 | 400 |

修复策略：以"块"为单位重建历史，非法块整段丢弃；丢块比造块安全。

## 4. 四个工具

| 工具 | 唯一要点 | 设计意图 |
|---|---|---|
| Read  | persistThresholdChars = SIZE_MAX 豁免落盘 | 避免"读→落盘→再读"循环依赖 |
| Write | 描述写明"先 Read 再写" | 否则模型盲写，悄悄丢掉没读到的行 |
| Edit  | old_str 必须在文件中唯一出现 | 不唯一就失败——失败换准确性 |
| Bash  | 退出码必须进结果 `[exit code N]` | 非 0 当成功会让模型得到错误结论 |

## 5. 三面板验证（verify_L5.ps1）

- **selftest**（不联网）：T1 预算分配 / T2 上下文测量 / T3 守卫修复孤儿 tool /
  T4 循环收敛（工具结果回喂后模型作答）/ T5 重复调用检测 / T6 权限门 deny 优先。
- **Mock 冒烟**：假服务端驱动"读文件"任务 → LoopOutcome=Completed，工具内容进入最终回答。

## 6. 运行

```powershell
./build.ps1 -Out agent.exe
./tests/verify_L5.ps1          # 自检 + 冒烟
./build/agent.exe --selftest   # 仅进程内自检
```
