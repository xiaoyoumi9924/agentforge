// ---------------------------------------------------------------------------
//  session_store.hpp —— 会话持久化与记忆（模块 ⑫）
//
//  ★ 模块 ⑫ 是 13 个模块里**最后一个**落地的。
//    从 L2 画出模块图到 L10 兑现，中间隔了 8 讲 ——
//    代价是它一直是个空格子，收益是**它没有一次需要回头改接口**。
//
//  ★ 为什么在基础设施层（不是交互执行层）：
//    L2 定的判据是"**它依赖什么、知道什么**"，不是"谁调用它"。
//    它只依赖 ⓪ types.hpp（Message / CompactionEvent），
//    不知道模型、工具、预算、UI 的存在 → 基础设施层。
//
//  ★ types.hpp 本讲**零改动**（CR-3，连续第三讲）：
//    CheckpointEvent 只有会话层用 → 放这里，和 L9 把 UiEvent 放 ui/tui.hpp 同一判断。
//    **谁用得多就把类型放哪儿，不要什么都往契约层塞。**
//
//  为什么是 JSONL：
//    · 追加写 —— 崩溃时最多丢最后一行
//    · 一行一事件 —— 可 tail -f、可 grep（可观测性是格式自带的）
//    · 一行坏了只丢一行 —— 前提是**每行自包含**
//    · 天然支持 fork —— 复制文件就是一次分叉
//    · 可回放 —— 重放日志就是重放会话
//
//  JSONL 管事件流，SQLite 管查询（会话列表、索引、token 统计）——
//  后者不在本讲范围内。**日志层 ≠ 查询层。**
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "types.hpp"

namespace fs = std::filesystem;

namespace af {

// ===========================================================================
//  检查点（模块 ⑫ 自己的类型 —— 不放 types.hpp）
// ===========================================================================
struct CheckpointEvent {
    std::string id;                                        // cp-<毫秒>
    std::string tool;                                      // 触发它的工具（Write / Edit）
    std::vector<std::pair<std::string, std::string>> snapshots;  // (路径, 完整内容)
};

struct SessionMeta {
    std::string id;
    fs::path    cwd;               // 会话绑定目录
    int64_t     createdAt  = 0;
    int64_t     lastActiveAt = 0;
    int         totalTokens = 0;
    fs::path    jsonlPath;
    // ★ "这次 open 是接着旧会话，还是开了个新的" —— 调用方**必须**看这个字段。
    //   见 open() 的注释：id 写错时静默开新会话 = 用户以为在"接着上次干"。
    bool        resumed = false;
};

// 加载结果。★ 坏行数必须**报出来** ——
//   "静默跳过坏行"会让"日志正在损坏"这件事永远不被发现。
struct LoadResult {
    std::vector<Message> messages;
    int  badLines    = 0;
    bool hadSummary  = false;      // 是否用上了交接摘要
    bool ignoredPartialCompaction = false;   // ★ T3：有没有遇到"没写完的压缩"
};

class SessionStore {
public:
    // 打开已有会话（resumeId 非空）或创建新会话。目录不存在会创建。
    static std::unique_ptr<SessionStore> open(fs::path dir,
                                              std::string resumeId = std::string());

    // ---- 写 ----
    // 每条都 flush —— 崩溃最多丢当前事件，不是丢整个缓冲区。
    void append(const Message& m);
    void appendCompaction(const CompactionEvent& e, int replacedUpto);
    void appendCheckpoint(const CheckpointEvent& e);

    // ---- 读 ----
    // ★ 从一条**可能被截断的**日志里恢复上下文。见伪码 P9。
    LoadResult load() const;

    // 兼容老接口：只要消息。
    std::vector<Message> loadContext() const { return load().messages; }

    // ---- 检查点 ----
    // ★ 必须在工具执行**之前**调用。先快照，再改 —— 反过来等于没快照。
    CheckpointEvent snapshot(const std::vector<fs::path>& files, const std::string& tool) const;
    bool restore(const std::string& checkpointId);

    // ---- 分叉 ----
    // 复制文件就是分叉：两个文件各自往后追加，天然互不干扰。
    std::unique_ptr<SessionStore> fork() const;

    const SessionMeta& meta() const noexcept { return meta_; }
    void touch() { meta_.lastActiveAt = nowMs(); }

    // 当前已落盘的最大 seq。★ 落盘 compaction 时要拿它当 replaced_upto：
    //   "本轮开始之前日志里已有的那些消息，被这一轮的摘要取代了"。
    int seq() const noexcept { return seq_; }

    // 毫秒时间戳（静态：fork / 外部也要用）
    static int64_t nowMs();

private:
    SessionStore() = default;

    SessionMeta   meta_;
    std::ofstream jsonl_;
    int           seq_ = 0;        // ★ 下一条事件的序号（从 1 开始）
};

// 列出 dir 下的所有会话（扫 *.jsonl 的第一行 meta）
std::vector<SessionMeta> listSessions(const fs::path& dir);

// ===========================================================================
//  三层记忆
// ===========================================================================
//
//  ┌────────────────┬──────────────────────┬────────────────────┐
//  │ 会话内上下文    │ 项目知识              │ 自动记忆            │
//  │ JSONL + 内存   │ CLAUDE.md / AGENTS.md│ MEMORY.md          │
//  │ 会话结束即失    │ 持久，人工维护        │ 持久，自动积累      │
//  └────────────────┴──────────────────────┴────────────────────┘
//
//  ★★ 关键原则：**每个新会话以全新的上下文窗口开始，不带之前会话的对话历史。**
//     跨会话传承**只能**靠 CLAUDE.md（人工规则）+ MEMORY.md（自动积累的事实）。
//
//     两个理由：
//      ① 成本 —— 带 10 个历史会话 = 每次启动烧几万 token，其中 99% 无关；
//      ② 正确性 —— 旧会话里有大量**已过时**的信息。
//         **过时的正确信息比没有信息更危险**（它会让 agent 自信地做错事）。
//
//     推论：**持久规则放 CLAUDE.md，不要依赖"上次我跟你说过"。**
//
//     ★ 这条与"--resume 能继续"不矛盾：
//       "继续"= 同一个任务跨进程继续（走 JSONL）；
//       "继承"= 新任务带上旧任务的对话（★ 不做）。
//       **"继续"与"继承"是两件事。**
// ===========================================================================

struct MemoryConfig {
    fs::path projectDir;
    int    memoryMaxLines = 200;             // MEMORY.md 前 200 行
    size_t memoryMaxBytes = 25 * 1024;       // 或 25KB，先到者为准
};

// 加载项目知识 + 自动记忆，作为 L1 层注入（★ 落点是 L2 就预留的 setProjectKnowledge）
std::string loadProjectKnowledge(const MemoryConfig& cfg);

// 追加一条自动记忆
void appendAutoMemory(const MemoryConfig& cfg, const std::string& entry);

}  // namespace af
