// ---------------------------------------------------------------------------
//  session_store.cpp —— JSONL 追加语义 + 崩溃恢复（伪码 P9 的落地）
//
//  ★★ 本讲最容易写错的地方，全部集中在 load() 里：
//     ① 用会抛异常的 json::parse → 一条坏行让整个会话加载失败；
//     ② 用"最后一个 compaction 在第几行"当边界 → fork / 手工编辑后失效；
//     ③ 截断后没兜底 → 上下文被截成空（这正是 T3 的灾难情形）。
//
//  ★ 落盘语义是"**回合结束时的上下文**"（快照），不是"事件流"（日志）：
//    见讲义 §0.5 —— 落盘边界与压缩边界都对齐在 turn 边界，
//    于是"压缩到一半被中断"这个状态**根本不存在**。
//    compaction 事件在这里的作用是**截断标记 + 审计**：
//       · replaced_upto：把 seq <= 它的消息从视图里去掉（被摘要取代了）
//       · summary / level / tokens_*：审计用（这一轮压缩花了多少、用的哪一级）
//
//  ★★ 顺序不变式（T3 的答案，代码里最要紧的一行注释）：
//      **先落盘新的（摘要 + 最近窗口），再落盘"老的可以丢了"。**
//      反过来（先写 replaced_upto、后写摘要消息）一旦中间崩溃，
//      就是"老的被标记删除、新的还没写" → **空上下文**。
//      load() 里还有一道兜底（INV-28）：截断后若为空则回退不截断。
// ---------------------------------------------------------------------------
#include "session_store.hpp"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <sstream>

namespace af {
namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
//  ★ 路径 ↔ UTF-8 字符串（K-36）
//
//  Windows 上 `fs::path::string()` 返回的是 **native 编码 = 系统 ANSI = GBK**。
//  把它塞进 json 再 dump() → nlohmann 抛 `type_error.316 invalid UTF-8 byte`。
//  这是同一族坑的**第三次发作**：
//     K-7  外部字节**内容**进上下文  → sanitizeUtf8()
//     K-29 UTF-8 **路径进** fs::path → pathFromUtf8()
//     K-36 fs::path **出来**进 JSON  → pathUtf8()
//  三个边界，三个函数。记规则比记补丁重要。
// ---------------------------------------------------------------------------
std::string pathUtf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

fs::path pathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

json messageToJson(const Message& m, int seq) {
    json j;
    j["seq"] = seq;
    j["ts"]  = SessionStore::nowMs();
    switch (m.role) {
        case Role::User:      j["t"] = "user"; break;
        case Role::Assistant: j["t"] = "assistant"; break;
        case Role::Tool:      j["t"] = "tool_result"; break;
        case Role::System:    j["t"] = "system"; break;
    }
    j["content"] = m.content;
    if (m.role == Role::Tool) j["call_id"] = m.toolCallId;
    if (!m.toolCalls.empty()) {
        json tcs = json::array();
        for (const auto& tc : m.toolCalls)
            tcs.push_back({{"id", tc.id}, {"name", tc.name}, {"args", tc.args}});
        j["tool_calls"] = tcs;
    }
    return j;
}

std::optional<Message> jsonToMessage(const json& j) {
    Message m;
    const std::string t = j.value("t", "");
    if      (t == "user")        m.role = Role::User;
    else if (t == "assistant")   m.role = Role::Assistant;
    else if (t == "tool_result") m.role = Role::Tool;
    else if (t == "system")      m.role = Role::System;
    else return std::nullopt;

    m.content    = j.value("content", "");
    m.toolCallId = j.value("call_id", "");
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        for (const auto& tc : j["tool_calls"])
            m.toolCalls.push_back(ToolCall{tc.value("id", ""), tc.value("name", ""),
                                           tc.value("args", json::object())});
    }
    return m;
}

// 扫一遍已有日志：拿到最大 seq（新事件从这里续），以及 meta
struct ScanResult { int maxSeq = 0; SessionMeta meta; };
ScanResult scanJsonl(const fs::path& p) {
    ScanResult r;
    std::ifstream ifs(p);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;      // ★ 坏行：跳过
        if (j.value("t", "") == "meta") {
            r.meta.id         = j.value("id", "");
            r.meta.cwd        = pathFromUtf8(j.value("cwd", ""));
            r.meta.createdAt  = j.value("created_at", int64_t(0));
            r.meta.jsonlPath  = p;
        }
        const int s = j.value("seq", 0);
        if (s > r.maxSeq) r.maxSeq = s;
    }
    return r;
}

}  // namespace

int64_t SessionStore::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::unique_ptr<SessionStore> SessionStore::open(fs::path dir, std::string resumeId) {
    auto s = std::unique_ptr<SessionStore>(new SessionStore());
    std::error_code ec;
    fs::create_directories(dir, ec);

    // ★ 给了 id 就用这个 id —— **绝不因为文件不存在就悄悄换个名字**。
    //
    //   我最初写的是 `if (resumeId.empty() || !exists) resumeId = "s-" + nowMs();`，
    //   于是 `--resume 打错的id` 会静默开一个新会话：用户看到"跑起来了"，
    //   以为在接着上次干，其实是从零开始，而日志里没有任何一行说"没找到"。
    //
    //   ★ 这条与 L9「降级不是瘫痪」相反：**降级必须说出来**。
    //     降级成"新会话"是合理行为，但**静默降级**不是 ——
    //     用户无法从行为上区分"恢复了"和"重开了"，只能靠 `resumed` 这个字段。
    if (resumeId.empty()) resumeId = "s-" + std::to_string(nowMs());

    s->meta_.id        = resumeId;
    s->meta_.cwd       = fs::current_path();
    s->meta_.jsonlPath = dir / (resumeId + ".jsonl");

    if (fs::exists(s->meta_.jsonlPath) && fs::file_size(s->meta_.jsonlPath, ec) > 0) {
        const ScanResult r = scanJsonl(s->meta_.jsonlPath);
        s->seq_               = r.maxSeq;
        s->meta_.createdAt    = r.meta.createdAt ? r.meta.createdAt : nowMs();
        s->meta_.resumed      = true;
    } else {
        s->meta_.createdAt = nowMs();
        s->meta_.resumed   = false;
    }
    s->meta_.lastActiveAt = nowMs();

    // ★★ 半行修复：文件最后一个字节不是 '\n' → 上次是"写到一半被杀"的。
    //
    //    不修会怎样？直接往后追加，新事件会**粘在坏行尾巴上**：
    //        ...被杀{"seq":3,"t":"user",...}      ← 一行，JSON 解析失败
    //    于是**新写的第一条也跟着作废**（I-66 实测：第二轮的第一条消息就是这么丢的）。
    //    症状特别坏：它不报错，只是"恢复后总少一条"，而且少的永远是**最新**那条。
    //
    //    修法：补一个 '\n' —— 等于宣告"上一行作废，后面的照常"。
    //    ★ 这正是 JSONL 相对"整体覆写"的优势：修复动作是**补一个字节**，
    //      而不是回滚整个文件。
    {
        std::error_code ecTail;
        if (fs::exists(s->meta_.jsonlPath)) {
            const auto sz = fs::file_size(s->meta_.jsonlPath, ecTail);
            if (!ecTail && sz > 0) {
                std::ifstream tail(s->meta_.jsonlPath, std::ios::binary);
                tail.seekg(-1, std::ios::end);
                char last = '\n';
                tail.read(&last, 1);
                if (last != '\n') {
                    std::ofstream fix(s->meta_.jsonlPath, std::ios::app | std::ios::binary);
                    fix << '\n';
                    fix.flush();
                }
            }
        }
    }

    s->jsonl_.open(s->meta_.jsonlPath, std::ios::app | std::ios::binary);

    // ★ meta 只在**第一次**写（文件为空时）。之后永不覆写已有行 —— 这是追加语义的核心。
    if (!fs::exists(s->meta_.jsonlPath) || fs::file_size(s->meta_.jsonlPath, ec) == 0) {
        json j;
        j["t"]          = "meta";
        j["v"]          = 1;
        j["id"]         = s->meta_.id;
        j["cwd"]        = pathUtf8(s->meta_.cwd);
        j["created_at"] = s->meta_.createdAt;
        s->jsonl_ << j.dump() << '\n';
        s->jsonl_.flush();
    }
    return s;
}

void SessionStore::append(const Message& m) {
    if (!jsonl_) return;
    jsonl_ << messageToJson(m, ++seq_).dump() << '\n';
    jsonl_.flush();          // ★ 每条 flush：崩溃最多丢当前事件，不是丢一个缓冲区
    meta_.lastActiveAt = nowMs();
}

void SessionStore::appendCompaction(const CompactionEvent& e, int replacedUpto) {
    if (!jsonl_) return;
    json j;
    j["seq"]            = ++seq_;
    j["ts"]             = nowMs();
    j["t"]              = "compaction";
    // ★ replaced_upto 是**内容判据**（seq），不是位置判据（行号）。
    //   行号会变（fork / 手工编辑 / 将来加过滤），seq 是内容自带的，搬到哪儿都一样。
    //   < 0 表示"这条只作审计、不参与重建"。
    j["replaced_upto"]  = replacedUpto;
    j["summary"]        = e.summary;
    j["level"]          = e.level;
    j["tokens_before"]  = e.tokensBefore;
    j["tokens_after"]   = e.tokensAfter;
    j["kept_messages"]  = e.keptMessages;
    jsonl_ << j.dump() << '\n';
    jsonl_.flush();
}

void SessionStore::appendCheckpoint(const CheckpointEvent& e) {
    if (!jsonl_) return;
    json j;
    j["seq"] = ++seq_;
    j["ts"]  = nowMs();
    j["t"]   = "checkpoint";
    j["id"]  = e.id;
    j["tool"] = e.tool;
    json snaps = json::array();
    //
    // ★★ K-36 的真身（I-62 抓出来的）：**转换不是幂等的，方向错了就炸。**
    //
    //    这里的 p **已经是** UTF-8 字符串（snapshot() 里用 pathUtf8 转过）。
    //    我原以为"保险起见再转成 fs::path 再转回来"，于是写了 `pathUtf8(fs::path(p))`。
    //    但 `fs::path(const std::string&)` 在 Windows 上按 **GBK** 解释字节，
    //    等于把 UTF-8 又解了一遍 —— 字节解不开就直接抛
    //    "No mapping for the Unicode character exists in the target multi-byte code page"。
    //
    //    教训：**编码转换只能做一次，且必须知道手里这一份是什么编码。**
    //    来回转换不会"更保险"，只会"更错"。
    //
    //    ★ 契约（类型看不出来，只能写在注释里）：
    //      `CheckpointEvent::snapshots[i].first` 是 **UTF-8 路径字符串**，不是 ANSI。
    //      要变回 `fs::path` 必须走 `pathFromUtf8()`（restore() 就是这么做的）。
    for (const auto& [p, content] : e.snapshots)
        snaps.push_back({{"path", p}, {"content", content}});
    j["snapshots"] = snaps;
    jsonl_ << j.dump() << '\n';
    jsonl_.flush();
}

// ===========================================================================
//  load() —— 伪码 P9
// ===========================================================================
LoadResult SessionStore::load() const {
    LoadResult out;
    std::ifstream ifs(meta_.jsonlPath);
    if (!ifs) return out;

    struct Item { int seq; Message m; };
    std::vector<Item> items;
    int replacedUpto = -1;                 // 已被摘要取代到的 seq
    bool sawCompaction = false;

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        // ★★ 非严格解析：坏行（半行 / 非法 JSON）**只丢自己**，不让整个文件作废。
        //    默认的 json::parse(s) 会抛异常 —— 而在"崩溃恢复"这个场景下，
        //    最后一行写了一半是**必然会出现**的输入。
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { ++out.badLines; continue; }

        const std::string t = j.value("t", "");
        if (t == "meta" || t == "checkpoint") continue;   // 不进上下文

        if (t == "compaction") {
            const int ru = j.value("replaced_upto", -1);
            const std::string summary = j.value("summary", "");
            sawCompaction = true;
            if (ru < 0) continue;                          // 纯审计，不参与重建
            if (summary.empty()) {
                // ★★ T3 的落点：摘要还没生成就先记了一条 → **这不是一次成功的压缩**。
                //    忽略它，回退到原始消息 —— 宁可长，不能空。
                out.ignoredPartialCompaction = true;
                continue;
            }
            replacedUpto = ru;
            out.hadSummary = true;
            continue;
        }

        auto m = jsonToMessage(j);
        if (!m) { ++out.badLines; continue; }
        items.push_back(Item{j.value("seq", 0), *m});
    }

    // ★ 截断：被摘要取代的消息不进视图
    std::vector<Message> kept;
    for (auto& it : items)
        if (replacedUpto < 0 || it.seq > replacedUpto) kept.push_back(it.m);

    // ★★ INV-28：截断不得把上下文截成空。
    //   这正是 T3 那个灾难情形（"老的被标记删除、新的还没写"）的兜底：
    //   真发生了，我们退回到"不截断"，宁可上下文长，也不能什么都看不见。
    if (sawCompaction && kept.empty() && !items.empty()) {
        for (auto& it : items) kept.push_back(it.m);
        out.hadSummary = false;
        replacedUpto = -1;
    }

    // 兜底：上下文不能以孤儿 tool_result 开头（否则 provider 400 —— L5 的 G8）
    while (kept.size() > 1 && kept[1].role == Role::Tool) kept.erase(kept.begin() + 1);
    if (!kept.empty() && kept[0].role == Role::Tool) kept.erase(kept.begin());

    out.messages = std::move(kept);
    return out;
}

// ===========================================================================
//  检查点（★ 必须在工具执行**之前**调用：先快照，再改）
// ===========================================================================
CheckpointEvent SessionStore::snapshot(const std::vector<fs::path>& files,
                                       const std::string& tool) const {
    CheckpointEvent e;
    e.id   = "cp-" + std::to_string(nowMs());
    e.tool = tool;
    for (const auto& p : files) {
        std::error_code ec;
        if (!fs::exists(p, ec) || fs::is_directory(p, ec)) continue;
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs) continue;
        std::ostringstream os; os << ifs.rdbuf();
        // ★ pathUtf8：K-36
        e.snapshots.emplace_back(pathUtf8(p), os.str());
    }
    return e;
}

bool SessionStore::restore(const std::string& checkpointId) {
    std::ifstream ifs(meta_.jsonlPath);
    if (!ifs) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        if (j.value("t", "") != "checkpoint") continue;
        if (j.value("id", "") != checkpointId) continue;
        for (const auto& snap : j["snapshots"]) {
            // ★ 从 JSON 回来的路径也要按 UTF-8 解释（K-29 的方向）
            const fs::path p = pathFromUtf8(snap.value("path", ""));
            std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
            if (!ofs) continue;
            const std::string c = snap.value("content", "");
            ofs.write(c.data(), std::streamsize(c.size()));
        }
        return true;
    }
    return false;
}

std::unique_ptr<SessionStore> SessionStore::fork() const {
    auto s = std::unique_ptr<SessionStore>(new SessionStore());
    s->meta_ = meta_;
    s->meta_.id        = meta_.id + "-fork-" + std::to_string(nowMs());
    s->meta_.jsonlPath = meta_.jsonlPath.parent_path() / (s->meta_.id + ".jsonl");

    std::error_code ec;
    // ★ 复制文件就是分叉：两个文件各自往后追加，天然互不干扰。
    fs::copy_file(meta_.jsonlPath, s->meta_.jsonlPath,
                  fs::copy_options::overwrite_existing, ec);
    const ScanResult r = scanJsonl(s->meta_.jsonlPath);
    s->seq_ = r.maxSeq;
    s->jsonl_.open(s->meta_.jsonlPath, std::ios::app | std::ios::binary);
    return s;
}

std::vector<SessionMeta> listSessions(const fs::path& dir) {
    std::vector<SessionMeta> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".jsonl") continue;
        const ScanResult r = scanJsonl(e.path());
        SessionMeta m;
        m.id         = r.meta.id.empty() ? e.path().stem().string() : r.meta.id;
        m.cwd        = r.meta.cwd;
        m.createdAt  = r.meta.createdAt;
        m.jsonlPath  = e.path();
        m.lastActiveAt = fs::last_write_time(e.path(), ec).time_since_epoch().count();
        out.push_back(m);
    }
    std::sort(out.begin(), out.end(),
              [](const SessionMeta& a, const SessionMeta& b) {
                  return a.lastActiveAt > b.lastActiveAt;
              });
    return out;
}

// ===========================================================================
//  三层记忆：项目知识（CLAUDE.md / AGENTS.md）+ 自动记忆（MEMORY.md）
// ===========================================================================
std::string loadProjectKnowledge(const MemoryConfig& cfg) {
    std::string out;
    for (const char* name : {"AGENTS.md", "CLAUDE.md"}) {
        std::ifstream ifs(cfg.projectDir / name);
        if (!ifs) continue;
        std::ostringstream os; os << ifs.rdbuf();
        out += "# " + std::string(name) + "\n" + os.str() + "\n";
    }
    // 自动记忆：前 N 行或 N 字节，先到者为准
    std::ifstream mifs(cfg.projectDir / "MEMORY.md");
    if (mifs) {
        std::string content, line;
        int lines = 0;
        while (std::getline(mifs, line) && lines < cfg.memoryMaxLines) {
            content += line; content += '\n';
            ++lines;
            if (content.size() >= cfg.memoryMaxBytes) break;
        }
        if (!content.empty()) out += "# 自动记忆\n" + content + "\n";
    }
    return out;
}

void appendAutoMemory(const MemoryConfig& cfg, const std::string& entry) {
    std::ofstream ofs(cfg.projectDir / "MEMORY.md", std::ios::app);
    if (ofs) ofs << "- " << entry << "\n";
}

}  // namespace af
