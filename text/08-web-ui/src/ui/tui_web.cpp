// ---------------------------------------------------------------------------
//  ui/tui_web.cpp —— Web UI 后端（题目八）
//
//  ★ 它只实现 Renderer 的 9 个纯虚函数，**不加新接口、不改 tui.hpp**：
//    "第三个后端" 能接进来，靠的是 L9 已经把渲染循环和 Agent 线程用
//    EventQueue 隔开了 —— 这一讲一行核心代码都不用动。
//
//  ★ 关键设计：WebSocket 上跑的是**事件增量**，不是"整帧字符串"。
//    理由：
//      ① Renderer::apply() 本来就是"吸收一个事件"，把它编码成 JSON 是最短路径；
//      ② 浏览器端按事件累加 DOM，与 plain 的行打印机是同一个模型；
//      ③ 刷新页面时**回放事件流**就能重建整段对话，不需要服务端记住画面。
//    反例是"每次 render() 发整屏文本"，那等于把浏览器当成一个哑终端 ——
//    工具卡片、折叠、点击就都做不了了。
//
//  ★ 线程：
//    · apply()/render() 在**渲染线程**（UiLoop 那个线程）上跑；
//    · askPermission() 在**Agent 线程**上跑（它要阻塞 Agent，不能阻塞渲染）；
//    · onMessage() 在**每个连接自己的读线程**上跑。
//    三者只通过本文件的 mu_/cv_ 和 EventQueue 说话。
// ---------------------------------------------------------------------------
#include "tui_web.hpp"

#include "web_json.hpp"
#include "web_page.hpp"
#include "web_server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace af::ui {
namespace {

using Clock = std::chrono::steady_clock;

int envInt(const char* name, int defv) {
    const char* v = std::getenv(name);
    if (!v || !*v) return defv;
    return std::atoi(v);
}

std::string envStr(const char* name, const std::string& defv) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : defv;
}

// ---------------------------------------------------------------------------
//  极简 JSON 读取（只服务客户端发来的那三种消息）
//
//  ★ 为什么不用 nlohmann：types.hpp 已经引入了它，但 UI 层不该为了读
//    {"t":"permAnswer","id":3,"allow":true} 去把整份 900KB 的头文件再拉一遍。
//    这里只需要"取一个字段"，30 行足够，且**不会**因为异常把读线程炸掉。
// ---------------------------------------------------------------------------
std::size_t fieldPos(const std::string& j, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    std::size_t p = j.find(pat);
    if (p == std::string::npos) return p;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return p;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    return p;
}

int hex4(const std::string& s, std::size_t at) {
    int v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const char c = (at + i < s.size()) ? s[at + i] : '0';
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
    }
    return v;
}

void appendUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string jsonUnescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') { o += s[i]; continue; }
        if (++i >= s.size()) break;
        switch (s[i]) {
        case 'n': o += '\n'; break;
        case 'r': o += '\r'; break;
        case 't': o += '\t'; break;
        case 'b': o += '\b'; break;
        case 'f': o += '\f'; break;
        case 'u': {
            unsigned cp = static_cast<unsigned>(hex4(s, i + 1));
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() &&
                s[i + 1] == '\\' && s[i + 2] == 'u') {
                const unsigned lo = static_cast<unsigned>(hex4(s, i + 3));
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i += 6;
            }
            appendUtf8(o, cp);
            break;
        }
        default: o += s[i];
        }
    }
    return o;
}

std::string jStr(const std::string& j, const char* key) {
    const std::size_t p = fieldPos(j, key);
    if (p == std::string::npos || p >= j.size() || j[p] != '"') return {};
    std::size_t i = p + 1;
    while (i < j.size()) {
        if (j[i] == '\\') { i += 2; continue; }
        if (j[i] == '"') break;
        ++i;
    }
    return jsonUnescape(j.substr(p + 1, i - p - 1));
}

long long jInt(const std::string& j, const char* key, long long defv) {
    const std::size_t p = fieldPos(j, key);
    if (p == std::string::npos || p >= j.size()) return defv;
    const char* b = j.c_str() + p;
    char* e = nullptr;
    const long long v = std::strtoll(b, &e, 10);
    return (e == b) ? defv : v;
}

bool jBool(const std::string& j, const char* key) {
    const std::size_t p = fieldPos(j, key);
    if (p == std::string::npos) return false;
    return j.compare(p, 4, "true") == 0;
}

// ===========================================================================
//  WebRenderer
// ===========================================================================
class WebRenderer final : public Renderer {
public:
    explicit WebRenderer(int port) : port_(port) {
        // ★ 每次启动换一个 epoch：页面把"seq + epoch"一起存进对话里。
        //   只存 seq 的后果：重启后 seq 从 1 重新数，而 localStorage 里还留着
        //   上一轮的大 seq，新事件全被当成"见过"丢掉 —— 页面看起来彻底不动了。
        epoch_ = std::to_string(static_cast<unsigned long long>(
            Clock::now().time_since_epoch().count()));
    }

    ~WebRenderer() override {
        if (srv_) srv_->stop();      // 先停线程，再析构自己 —— 回调可能正打进来
    }

    bool start() {
        web::WebServer::Options o;
        o.port         = port_;
        o.html         = webPage();
        o.onConnect    = [this] { this->onConnect(); };
        o.onMessage    = [this](int clientId, const std::string& m) { this->onMessage(clientId, m); };
        o.onDisconnect = [this](int clientId) { this->onDisconnect(clientId); };
        srv_ = std::make_unique<web::WebServer>(std::move(o));
        if (!srv_->start()) { srv_.reset(); return false; }
        return true;
    }

    // ---- Renderer 接口 -----------------------------------------------------
    const char* name() const override { return "web"; }
    bool        isTui() const override { return false; }

    void apply(const UiEvent& e) override {
        const bool live = srv_ && srv_->clientCount() > 0;

        switch (e.kind) {
        case UiEvent::Kind::Text:
            if (!live) { std::cout << e.text << std::flush; }     // 降级：终端镜像
            textBuf_ += e.text;
            if (textBuf_.size() >= 2048) flushText();
            return;

        case UiEvent::Kind::Thinking:
            if (!live) { std::cout << "[思考] " << e.text << "\n" << std::flush; }
            thinkBuf_ += e.text;
            if (thinkBuf_.size() >= 2048) flushThink();
            return;

        case UiEvent::Kind::ToolStart:
            inFlight_     = true;
            inFlightName_ = e.toolName;
            inFlightId_   = e.toolId;
            inFlightAt_   = Clock::now();
            break;

        case UiEvent::Kind::ToolDone:
            inFlight_ = false;
            break;

        case UiEvent::Kind::Usage:
            tokens_ = e.tokens;
            break;

        default:
            break;
        }

        flushText();
        flushThink();
        if (!live) mirror(e);
        publish(eventToJson(e));
    }

    std::string render() override {
        flushText();
        flushThink();

        if (!inFlight_) return {};

        const auto now = Clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(now - inFlightAt_).count();
        if (now - lastTick_ < std::chrono::milliseconds(300)) return {};
        lastTick_ = now;

        if (srv_ && srv_->clientCount() > 0) {
            // tick 不进历史（回放一串 tick 没有意义，还会把卡片时长搞乱）
            srv_->send("{\"t\":\"tick\",\"id\":\"" + jsonEscape(inFlightId_) +
                       "\",\"ms\":" + std::to_string(static_cast<long long>(ms)) + "}");
        } else {
            std::cout << "[执行中] " << inFlightName_ << " 已运行 "
                      << static_cast<int>(ms) << "ms\n" << std::flush;
        }
        // ★ 永远返回空串：writeFrame() 就不会往 stdout 写任何东西。
        //   否则 web 模式下 stdout 会混进一份"帧文本"，既有断言全乱。
        return {};
    }

    bool askPermission(const std::string& prompt, int id) override {
        // ★ 没有浏览器：fail-closed，而且**必须立刻返回**。
        //   在这里阻塞的后果是整轮任务卡死，而终端上什么都看不见（K-9 那一类）。
        if (!srv_ || srv_->clientCount() == 0) {
            std::cout << "\n[权限] " << prompt
                      << "\n  [web] 没有浏览器连接，按拒绝处理\n" << std::flush;
            return false;
        }

        flushText();
        flushThink();
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_       = true;
            pendingId_     = id;
            pendingPrompt_ = prompt;
            answered_      = false;
            answerYes_     = false;
        }
        srv_->send("{\"t\":\"perm\",\"id\":" + std::to_string(id) +
                   ",\"prompt\":\"" + jsonEscape(prompt) + "\"}");

        // ★ 默认只等 30 秒。以前是 300 秒：只要浏览器里漏看/关掉了弹窗，
        //   Agent 就会像“思考卡住”一样干等 5 分钟，用户完全不知道它在等权限。
        //   30 秒足够看清并做选择；超时按拒绝继续，不把整轮任务拖死。
        const int timeoutMs = envInt("AF_WEB_PERM_TIMEOUT_MS", 30000);
        bool finished = false;
        bool yes = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            finished = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                    [this, id] {
                                        // ★ 三个条件缺一不可：
                                        //   answered_  —— 浏览器答了（**最初漏了这条**：
                                        //                 答完却还在等，表现成"点了允许没反应"）
                                        //   !pending_  —— 关页面/超时后被撤回
                                        //   id 变了    —— 上一个询问已经翻篇
                                        return answered_ || !pending_ || pendingId_ != id;
                                    });
            yes       = answered_ && answerYes_;
            pending_  = false;
            answered_ = false;
        }
        srv_->send("{\"t\":\"permEnd\",\"id\":" + std::to_string(id) +
                   ",\"allow\":" + (yes ? "true" : "false") + "}");
        if (!finished) {
            std::cout << "\n[权限] 浏览器 " << timeoutMs / 1000
                      << " 秒没有回答，按拒绝处理\n" << std::flush;
        }
        return yes;
    }

    // Renderer 接口（没有连接身份）：内部/测试路径，按可信来源处理
    void answerPermission(int id, bool yes) override { answerPermissionFrom(0, id, yes); }

    // ★ 浏览器回答的唯一入口：带上"是哪条连接发来的"。
    //   服务端在 accept 时给每条连接编号，onMessage 把它一路传到这里 ——
    //   所以日志里能说清"连接 #3 点了允许"，而不是"有个人点了允许"。
    //   ① 只认当前询问（id 对得上）：迟到/重放的回答一律丢弃；
    //   ② 只认第一个回答：两个页面同时点，第二个不会覆盖第一个。
    bool answerPermissionFrom(int clientId, int id, bool yes) {
        std::lock_guard<std::mutex> lk(mu_);
        const bool stale = !pending_ || pendingId_ != id;
        if (debug_) {
            std::cerr << "[web] 连接 #" << clientId << " 回答权限 id=" << id
                      << " yes=" << yes << " pending=" << pending_
                      << " pid=" << pendingId_ << (stale ? "（不是当前询问，丢弃）" : "") << "\n";
        }
        if (stale) return false;
        if (answered_) return false;             // 第一个回答生效，后面的一律忽略
        answered_  = true;
        answerYes_ = yes;
        cv_.notify_all();
        return true;
    }

    bool hasPendingPrompt() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_;
    }

    bool toolInFlight() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return inFlight_.load();
    }

    // ★ 浏览器点"压缩上下文"：这里只落一个待办标志，真正的压缩在 main
    //   （它才拿得到历史 + 压缩器）。渲染线程只管转发，不碰模型。
    bool takeCompact(std::string& cid) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!compactReq_) return false;
        compactReq_ = false;
        cid = compactCid_;
        return true;
    }

    // 系统提示行（压缩进度/结果）。走和事件同一条通路，页面照常渲染。
    void notice(const std::string& s) {
        publish("{\"t\":\"notice\",\"s\":\"" + jsonEscape(s) + "\"}");
    }

    // ---- 扩展（非 Renderer 接口）-------------------------------------------
    int  clientCount() const { return srv_ ? srv_->clientCount() : 0; }
    int  port() const { return port_; }

    bool waitTask(std::string& out, int timeoutMs, std::string* cid = nullptr) {
        std::unique_lock<std::mutex> lk(mu_);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
        for (;;) {
            if (!tasks_.empty()) {
                Task t = std::move(tasks_.front());
                tasks_.pop_front();
                // ★ 从这里开始，这一轮产生的所有事件都带这个 cid（publish 里打标）。
                runCid_ = t.cid;
                out = std::move(t.text);
                if (cid) *cid = t.cid;
                busy_.store(true);
                return true;
            }
            if (timeoutMs >= 0) {
                if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) return false;
            } else {
                // 一直等：用短超时轮询，避免"进程活着但叫不醒"
                cv_.wait_for(lk, std::chrono::milliseconds(200));
            }
        }
    }

    // 设置面板改过的东西：这里只**收**，应用在 main（下一轮生效）。
    bool takeSettings(WebSettings& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cfgDirty_) return false;
        out.dirty  = true;
        out.apiKey  = cfgKey_;
        out.baseUrl = cfgBase_;
        out.model   = cfgModel_;
        cfgDirty_ = false;
        return true;
    }

private:
    // ---- 出站 --------------------------------------------------------------
    //
    //  ★ 每条事件都带一个自增 seq。为什么必须编号：
    //    掉线期间生成的事件照样进 hist_，重连时随 replay 一起补发；页面靠 seq
    //    判断"这条我有没有见过"。没有编号就只剩"对话是不是空的"这一种判据 ——
    //    而复连时对话显然不空，于是补发永远不生效，掉线那一段答案永久丢失。
    // ★ cidOverride 只在"替别人回显"时用（例如 user 气泡：它属于提交任务的那个对话，
    //   而不是此刻正在跑的那个）。其余事件一律用 runCid_ —— 也就是"这轮是谁的"。
    void publish(const std::string& msg, const std::string& cidOverride = std::string()) {
        if (msg.empty()) return;

        std::string tag = cidOverride;
        if (tag.empty()) {
            std::lock_guard<std::mutex> lk(mu_);
            tag = runCid_;
        }

        std::string out = msg;
        if (out.size() > 1 && out[0] == '{') {
            const unsigned long long n = ++seq_;
            std::string head = "\"seq\":" + std::to_string(n) + ",";
            // ★ 每条事件带 cid：页面据此把事件存进**它所属**的对话。
            //   没有这一步，前端只能按"当前选中的对话"入库 —— 运行中切一下对话就串味。
            if (!tag.empty()) head += "\"cid\":\"" + jsonEscape(tag) + "\",";
            out = "{" + head + out.substr(1);

            // ★ 顺手记一下"这一轮还在不在跑"：
            //   收到任务(user) → 忙；发出"本轮结束"(notice) → 闲。
            //   复连时把 busy 告诉页面，页面才不会因为错过那条通知而永远锁着发送键。
            const std::string t = jStr(out, "t");
            if (t == "user") {
                busy_.store(true);
            } else if (t == "notice" &&
                       jStr(out, "s").find("本轮结束") != std::string::npos) {
                // ★ 队列里还有任务就仍然算"忙"：否则页面会闪一下"可以输入"，
                //   紧接着下一条又开始跑。
                std::lock_guard<std::mutex> lk(mu_);
                busy_.store(!tasks_.empty());
            }
        }

        {
            std::lock_guard<std::mutex> lk(histMu_);
            hist_.push_back(out);
            if (hist_.size() > 1500) hist_.erase(hist_.begin(), hist_.begin() + 500);
        }
        if (srv_) srv_->send(out);
    }

    void flushText() {
        if (textBuf_.empty()) return;
        std::string s;
        s.swap(textBuf_);
        publish("{\"t\":\"text\",\"s\":\"" + jsonEscape(s) + "\"}");
    }

    void flushThink() {
        if (thinkBuf_.empty()) return;
        std::string s;
        s.swap(thinkBuf_);
        publish("{\"t\":\"think\",\"s\":\"" + jsonEscape(s) + "\"}");
    }

    // ---- 无浏览器时的终端镜像（降级的"可见"部分）--------------------------
    void mirror(const UiEvent& e) {
        switch (e.kind) {
        case UiEvent::Kind::ToolCall:
            std::cout << "\n[模型请求工具] " << e.toolName << "\n" << std::flush;
            break;
        case UiEvent::Kind::ToolArgs:
            std::cout << "[参数] " << e.text << "\n" << std::flush;
            break;
        case UiEvent::Kind::ToolStart:
            std::cout << "[事件] onToolStart: " << e.toolName << "\n" << std::flush;
            break;
        case UiEvent::Kind::ToolDone:
            std::cout << "[事件] onToolDone: " << (e.isError ? "错误" : "成功")
                      << " (" << static_cast<int>(e.elapsedMs) << "ms)\n"
                      << (e.isError ? "[工具失败] " : "[工具返回] ") << e.text
                      << "\n\n" << std::flush;
            break;
        case UiEvent::Kind::Notice:
            std::cout << "[事件] onNotice: " << e.text << "\n" << std::flush;
            break;
        case UiEvent::Kind::Error:
            std::cout << "\n[请求失败] " << e.text << "\n" << std::flush;
            break;
        case UiEvent::Kind::StreamError:
            std::cout << "\n[流内错误] " << e.text << "\n" << std::flush;
            break;
        case UiEvent::Kind::PermAsk:
            std::cout << "\n[权限] " << e.text << "\n" << std::flush;
            break;
        default:
            break;
        }
    }

    // ---- 浏览器事件 --------------------------------------------------------
    void onConnect() {
        // ★ 走 stderr：这两条由**服务端读线程**打印，和 Agent 线程的 stdout
        //   抢同一行会把「[自检] PASS」拆开（I-103c 就撞过）。stdout 留给断言。
        std::cerr << "[web] 浏览器已连接  http://127.0.0.1:" << port_ << "\n" << std::flush;

        const bool hasKey = !envStr("OPENAI_API_KEY", "").empty() ||
                            !envStr("ANTHROPIC_API_KEY", "").empty();
        srv_->send("{\"t\":\"hello\",\"port\":" + std::to_string(port_) +
                   ",\"model\":\"" + jsonEscape(envStr("AF_MODEL", "unknown")) +
                   "\",\"hasKey\":" + (hasKey ? "true" : "false") +
                   ",\"provider\":\"" + jsonEscape(envStr("AF_PROVIDER", "openai")) + "\"}");

        std::vector<std::string> hist;
        std::string cid;
        {
            std::lock_guard<std::mutex> lk(histMu_);
            hist = hist_;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            cid = runCid_;
        }
        // ★ 顶层 cid 只是"最后跑的是哪个对话"的提示；真正决定事件落哪个对话的，
        //   是 `e` 里每条事件自带的 cid（缓冲现在是多对话混排的，不再按 cid 清空）。
        //   epoch 仍然保留：页面靠它识别"服务端重启过，seq 从 1 重新数了"。
        std::string rep = "{\"t\":\"replay\",\"epoch\":\"" + jsonEscape(epoch_) +
                          "\",\"cid\":\"" + jsonEscape(cid) + "\",\"e\":[";
        for (std::size_t i = 0; i < hist.size(); ++i) {
            if (i) rep += ',';
            rep += hist[i];
        }
        rep += "]}";
        srv_->send(rep);

        // ★ 复连时必须同步一次"这轮还在不在跑"。
        //   掉线的那一分钟里如果正好跑完一轮，页面就收不到"本轮结束"，
        //   busy 会永远为真 —— 症状是"右上角已连接，但发送键是灰的、回车没反应"。
        srv_->send(std::string("{\"t\":\"state\",\"busy\":") +
                   (busy_.load() ? "true" : "false") + "}");

        // 刷新页面时如果正卡在权限询问上，弹窗必须回来 —— 否则 Agent 就永远等着
        std::string prompt;
        int  pid  = 0;
        bool pend = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pend = pending_;
            pid  = pendingId_;
            prompt = pendingPrompt_;
        }
        if (pend) {
            srv_->send("{\"t\":\"perm\",\"id\":" + std::to_string(pid) +
                       ",\"prompt\":\"" + jsonEscape(prompt) + "\"}");
        }
    }

    void onDisconnect(int clientId) {
        std::cerr << "[web] 连接 #" << clientId << " 断开";
        if (!srv_ || srv_->clientCount() == 0) std::cerr << "，后续改走终端输出";
        std::cerr << "\n" << std::flush;
        // ★ 演示可见性：浏览器关掉后，终端里要有一块明确的"画面"，
        //   而不是只剩几行零散日志。Agent 进程仍活着，后续事件继续打在这里。
        if (!srv_ || srv_->clientCount() == 0) {
            std::cerr
                << "\n============================================================\n"
                << "  浏览器已关闭 —— Agent 仍在运行，未退出\n"
                << "  输出已切换到终端（plain 模式）\n"
                << "  重新打开 http://127.0.0.1:" << port_ << " 可恢复网页\n"
                << "  结束进程：在这个窗口按 Ctrl-C\n"
                << "============================================================\n"
                << std::flush;
        }
        // ★ 还有别的页面连着就不算"人走了"。
        //   不加这一条的后果：开两个标签页，关掉其中一个 → 正在等的权限询问
        //   被当成"用户关了页面"直接判拒绝，而人明明还在另一个标签页上看着。
        if (srv_ && srv_->clientCount() > 0) return;
        // 卡在权限询问上时立刻 fail-closed —— 关页面 = 拒绝，不能让 Agent 干等
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_) {
            pending_  = false;
            answered_ = false;
            cv_.notify_all();
        }
    }

    void onMessage(int clientId, const std::string& msg) {
        if (debug_) {
            std::cerr << "[web] 连接 #" << clientId << " recv " << msg.size()
                      << "B: " << msg.substr(0, 160) << "\n";
        }
        const std::string t = jStr(msg, "t");
        if (t == "permAnswer") {
            answerPermissionFrom(clientId, static_cast<int>(jInt(msg, "id", 0)),
                                 jBool(msg, "allow"));
        } else if (t == "compact") {
            // ★ 手动压缩：只记下来，压缩在 main 里做（要历史、要压缩器、可能要调模型）
            {
                std::lock_guard<std::mutex> lk(mu_);
                compactReq_ = true;
                compactCid_ = jStr(msg, "cid");
            }
            std::cout << "[web] 收到压缩请求 cid=" << jStr(msg, "cid") << "\n" << std::flush;
        } else if (t == "task" || t == "start") {
            const std::string s = jStr(msg, "s");
            if (s.empty()) return;
            const std::string cid = jStr(msg, "cid");
            // ★ 这里**不再清**回放缓冲。
            //   以前"切对话就 hist_.clear()"，在多会话下是错的方向：B 提一条任务，
            //   正在跑的 A 那半段历史就被清掉了，刷新页面 A 的答案直接消失。
            //   现在每条事件自己带 cid，页面按 cid 分拣 —— 缓冲留着才是对的。
            bool rejected = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (tasks_.size() >= kMaxQueuedTasks) {
                    rejected = true;
                } else {
                    // ★ 运行中提交 → 排队，**不覆盖**上一条（覆盖就是"任务凭空消失"）
                    tasks_.push_back(Task{s, cid});
                    busy_.store(true);
                }
            }
            if (rejected) {
                publish("{\"t\":\"notice\",\"s\":\"任务队列已满（最多 " +
                            std::to_string(kMaxQueuedTasks) +
                            " 条），这条没有提交；等当前任务跑完再发\"}",
                        cid);
                std::cout << "[web] 队列已满，拒绝任务[" << cid << "]\n" << std::flush;
                return;
            }
            std::cout << "[web] 任务入队[" << cid << "]：" << s.substr(0, 60)
                      << "（待跑 " << tasks_.size() << " 条）\n" << std::flush;
            // ★ 把"用户说了什么"当成一条事件回显给**所有**页面。
            //   为什么不让前端自己画完就算：那样"刷新页面"或"开第二个标签页"
            //   就看不到提问，只剩孤零零的答案 —— 回放缓冲里根本没有这条。
            //   `d` 是给人看的版本（附件展开后的原文太长，气泡里只显示一句）。
            //   ★ 显式带上 cid：它属于**提交任务的那个对话**，不是此刻正在跑的那个。
            const std::string shown = jStr(msg, "d");
            publish("{\"t\":\"user\",\"s\":\"" + jsonEscape(shown.empty() ? s : shown) + "\"}",
                    cid);
            cv_.notify_all();
        } else if (t == "config") {
            // 设置面板：只存下来，应用在 main 的下一轮开始处
            std::lock_guard<std::mutex> lk(mu_);
            cfgKey_   = jStr(msg, "apiKey");
            cfgBase_  = jStr(msg, "baseUrl");
            cfgModel_ = jStr(msg, "model");
            cfgDirty_ = true;
            std::cout << "[设置] 浏览器提交了新设置"
                      << "（模型=" << (cfgModel_.empty() ? "未改" : cfgModel_)
                      << "，密钥=" << (cfgKey_.empty() ? "未改" : "已提供") << "）\n"
                      << std::flush;
            cv_.notify_all();
        }
        // 其余（hello/ping）忽略：浏览器不需要服务端回话
    }

    // ---- 状态 --------------------------------------------------------------
    int port_ = 8080;
    std::unique_ptr<web::WebServer> srv_;
    // AF_WEB_DEBUG=1：把收到的每一帧打到 stderr（排"消息到底有没有到"用）
    bool debug_ = (std::getenv("AF_WEB_DEBUG") != nullptr);

    mutable std::mutex      mu_;                 // pending/任务/inFlight 的状态锁
    std::condition_variable cv_;

    bool        pending_ = false;
    int         pendingId_ = 0;
    std::string pendingPrompt_;
    bool        answered_ = false;
    bool        answerYes_ = false;

    // ★ 任务队列：以前只有一个"待处理任务槽"，运行中再提交就把上一条**覆盖**掉。
    //   多会话下这是真的会丢任务（A 在跑、B 和 C 各提一条，B 就没了）。
    //   改成 FIFO：谁先提交谁先跑，队列上限 kMaxQueuedTasks，满了明确拒绝而不是静默覆盖。
    struct Task { std::string text; std::string cid; };
    static constexpr std::size_t kMaxQueuedTasks = 16;

    std::deque<Task> tasks_;      // 待跑的任务（FIFO）
    std::string      runCid_;     // 当前这一轮属于哪个对话：事件靠它打 cid 标记

    // 设置面板（浏览器 → C++）；只有 main 在每轮开始前取一次
    bool        cfgDirty_ = false;
    std::string cfgKey_, cfgBase_, cfgModel_;

    // ★ 手动压缩（浏览器 → C++）；同样只有 main 取得到
    bool        compactReq_ = false;
    std::string compactCid_;

    // 渲染线程独占（inFlight_ 是 atomic：toolInFlight() 可能从别处被问）
    std::string textBuf_, thinkBuf_;
    std::atomic<bool> inFlight_{false};
    std::string inFlightName_, inFlightId_;
    Clock::time_point inFlightAt_{}, lastTick_{};
    int         tokens_ = 0;

    std::mutex               histMu_;
    std::vector<std::string> hist_;

    // ★ seq 是**进程级**的：一条事件一个号，跨对话也一直往上走。
    //   publish() 可能被渲染线程和连接读线程同时调用（user 回显在读线程里发），
    //   所以必须是 atomic —— 两个线程同时 ++ 会撞出重复号，重复号 = 页面丢事件。
    std::atomic<unsigned long long> seq_{0};
    std::atomic<bool>               busy_{false};
    std::string                     epoch_;
};

}  // namespace

// ===========================================================================
//  对外函数
// ===========================================================================
int webPortFromEnv() {
    const int p = envInt("AF_WEB_PORT", 8080);
    return (p > 0 && p < 65536) ? p : 8080;
}

std::unique_ptr<Renderer> makeWebRenderer(int port) {
    auto r = std::make_unique<WebRenderer>(port);
    if (!r->start()) return nullptr;
    return r;
}

bool webIsBackend(const Renderer& r) {
    return dynamic_cast<const WebRenderer*>(&r) != nullptr;
}

int webClientCount(const Renderer& r) {
    if (const auto* w = dynamic_cast<const WebRenderer*>(&r)) return w->clientCount();
    return 0;
}

bool webWaitClient(Renderer& r, int timeoutMs) {
    auto* w = dynamic_cast<WebRenderer*>(&r);
    if (!w) return false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (w->clientCount() > 0) return true;
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool webWaitForTask(Renderer& r, std::string& out, int timeoutMs) {
    auto* w = dynamic_cast<WebRenderer*>(&r);
    if (!w) return false;
    return w->waitTask(out, timeoutMs);
}

bool webWaitForTask(Renderer& r, std::string& out, std::string& cid, int timeoutMs) {
    auto* w = dynamic_cast<WebRenderer*>(&r);
    if (!w) return false;
    return w->waitTask(out, timeoutMs, &cid);
}

bool webTakeSettings(Renderer& r, WebSettings& out) {
    auto* w = dynamic_cast<WebRenderer*>(&r);
    if (!w) return false;
    return w->takeSettings(out);
}

bool webHasClient(const Renderer& r) {
    return webClientCount(r) > 0;
}

bool webTakeCompact(Renderer& r, std::string& cid) {
    auto* w = dynamic_cast<WebRenderer*>(&r);
    if (!w) return false;
    return w->takeCompact(cid);
}

void webNotice(Renderer& r, const std::string& text) {
    if (auto* w = dynamic_cast<WebRenderer*>(&r)) w->notice(text);
}

}  // namespace af::ui
