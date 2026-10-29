// ---------------------------------------------------------------------------
//  L10 · session_store.cpp —— SQLite 会话存储实现
// ---------------------------------------------------------------------------
#include "session_store.hpp"

#include <sqlite3.h>

#include <cstdio>
#include <ctime>

namespace af {

namespace {

const char* kSchema =
    "CREATE TABLE IF NOT EXISTS sessions("
    "  id TEXT PRIMARY KEY,"
    "  title TEXT,"
    "  updated_at TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS messages("
    "  session_id TEXT NOT NULL,"
    "  seq INTEGER NOT NULL,"
    "  role INTEGER NOT NULL,"
    "  content TEXT,"
    "  tool_calls TEXT,"
    "  tool_call_id TEXT,"
    "  finish_reason INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY(session_id, seq)"
    ");";

std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

int roleToInt(Role r) { return static_cast<int>(r); }
Role intToRole(int v) { return static_cast<Role>(v); }

}  // namespace

SessionStore::~SessionStore() { close(); }

void SessionStore::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SessionStore::exec(const std::string& sql) {
    if (!db_) return false;
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        err_ = err ? err : sqlite3_errmsg(db_);
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool SessionStore::open(const std::string& dbPath) {
    close();
    const int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        err_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open 失败";
        close();
        return false;
    }
    return ensureSchema();
}

bool SessionStore::ensureSchema() {
    // 一次 exec 多条语句
    return exec(kSchema);
}

bool SessionStore::ensureSession(const std::string& id, const std::string& title) {
    return upsertSession(id, title);
}

bool SessionStore::upsertSession(const std::string& id, const std::string& title) {
    if (!db_) return false;
    const char* sql2 =
        "INSERT OR IGNORE INTO sessions(id, title, updated_at) VALUES(?,?,?)";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql2, -1, &st, nullptr) != SQLITE_OK) {
        err_ = sqlite3_errmsg(db_);
        return false;
    }
    const std::string ts = nowStamp();
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        err_ = sqlite3_errmsg(db_);
        return false;
    }
    // 更新 updated_at
    sqlite3_stmt* st2 = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE sessions SET updated_at=? WHERE id=?", -1, &st2,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st2, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st2, 2, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st2);
        sqlite3_finalize(st2);
    }
    return true;
}

bool SessionStore::appendMessages(const std::string& sessionId,
                                  const std::vector<Message>& msgs,
                                  std::size_t startIndex) {
    if (!db_ || startIndex >= msgs.size()) return true;

    // 取当前最大 seq
    int maxSeq = 0;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(seq),0) FROM messages WHERE session_id=?",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) maxSeq = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
    }

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT OR REPLACE INTO messages(session_id, seq, role, content, tool_calls, tool_call_id, finish_reason) "
        "VALUES(?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err_ = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    bool ok = true;
    for (std::size_t i = startIndex; i < msgs.size(); ++i) {
        const Message& m = msgs[i];
        json calls = json::array();
        for (const auto& tc : m.toolCalls)
            calls.push_back(json{{"id", tc.id}, {"name", tc.name}, {"args", tc.args}});
        const std::string callStr = calls.empty() ? std::string() : calls.dump();

        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, ++maxSeq);
        sqlite3_bind_int(st, 3, roleToInt(m.role));
        sqlite3_bind_text(st, 4, m.content.c_str(), -1, SQLITE_TRANSIENT);
        if (callStr.empty()) sqlite3_bind_null(st, 5);
        else                sqlite3_bind_text(st, 5, callStr.c_str(), -1, SQLITE_TRANSIENT);
        if (m.toolCallId.empty()) sqlite3_bind_null(st, 6);
        else                      sqlite3_bind_text(st, 6, m.toolCallId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 7, static_cast<int>(m.finishReason));

        if (sqlite3_step(st) != SQLITE_DONE) {
            ok = false;
            err_ = sqlite3_errmsg(db_);
            break;
        }
    }
    sqlite3_finalize(st);
    sqlite3_exec(db_, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
    if (ok) upsertSession(sessionId, "");
    return ok;
}

std::vector<Message> SessionStore::loadHistory(const std::string& sessionId) {
    std::vector<Message> out;
    if (!db_) return out;

    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT role, content, tool_calls, tool_call_id, finish_reason "
        "FROM messages WHERE session_id=? ORDER BY seq";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err_ = sqlite3_errmsg(db_);
        return out;
    }
    sqlite3_bind_text(st, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        Message m;
        m.role = intToRole(sqlite3_column_int(st, 0));
        if (const unsigned char* c = sqlite3_column_text(st, 1)) m.content = (const char*)c;
        if (const unsigned char* c = sqlite3_column_text(st, 2)) {
            const json calls = json::parse((const char*)c, nullptr, false);
            if (calls.is_array()) {
                for (const auto& cj : calls) {
                    ToolCall tc;
                    tc.id   = cj.value("id", std::string());
                    tc.name = cj.value("name", std::string());
                    tc.args = cj.value("args", json::object());
                    m.toolCalls.push_back(std::move(tc));
                }
            }
        }
        if (const unsigned char* c = sqlite3_column_text(st, 3)) m.toolCallId = (const char*)c;
        m.finishReason = static_cast<FinishReason>(sqlite3_column_int(st, 4));
        out.push_back(std::move(m));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<std::pair<std::string, std::string>> SessionStore::listSessions() {
    std::vector<std::pair<std::string, std::string>> out;
    if (!db_) return out;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, updated_at FROM sessions ORDER BY updated_at DESC",
                           -1, &st, nullptr) != SQLITE_OK) {
        err_ = sqlite3_errmsg(db_);
        return out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        const char* ts = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        out.emplace_back(id ? id : "", ts ? ts : "");
    }
    sqlite3_finalize(st);
    return out;
}

}  // namespace af
