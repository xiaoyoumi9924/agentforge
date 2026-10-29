// ---------------------------------------------------------------------------
//  L10 · session_store.hpp —— 会话持久化（模块 ⑫，SQLite）
//
//  SQLite 用官方 amalgamation（thirdparty/sqlite/sqlite3.c），零额外依赖。
//
//  为什么必须持久化：AI 的"中间迷失"决定了长任务必须能中断恢复；
//  会话恢复 = 把消息列表原样读回来继续跑，而不是让模型从头再来。
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "types.hpp"

struct sqlite3;

namespace af {

class SessionStore {
public:
    SessionStore() = default;
    ~SessionStore();
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    bool open(const std::string& dbPath);     // 创建/打开数据库并建表
    void close();
    bool isOpen() const noexcept { return db_ != nullptr; }
    const std::string& lastError() const noexcept { return err_; }

    // 会话元信息（不存在则创建）
    bool ensureSession(const std::string& id, const std::string& title);

    // 追加消息（从 startIndex 开始截取尾部保存；自动续 seq）
    bool appendMessages(const std::string& sessionId,
                        const std::vector<Message>& msgs,
                        std::size_t startIndex);

    // 读回完整历史（按 seq 排序）
    std::vector<Message> loadHistory(const std::string& sessionId);

    // 列出会话（id, updated_at），按更新时间倒序
    std::vector<std::pair<std::string, std::string>> listSessions();

private:
    bool exec(const std::string& sql);
    bool ensureSchema();
    bool upsertSession(const std::string& id, const std::string& title);
    bool setSessionError(bool ok);

    sqlite3* db_ = nullptr;
    std::string err_;
};

}  // namespace af
