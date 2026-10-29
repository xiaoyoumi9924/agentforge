// ---------------------------------------------------------------------------
//  ui/tui_plain.cpp —— 纯文本渲染器（非 TTY / 无 FTXUI 时的降级实现）
//
//  ★ 这个文件**永远参与编译**，永远可用。
//    FTXUI 不在场时整套 UI 由它兜住 —— 这正是 C32 要证明的事：
//    "降级不是瘫痪"。
//
//  模型：行打印机（不是画布）。写出去就收不回来，所以：
//    · 文本只能往下写，没有"重画"这回事；
//    · 工具卡片在讲义 §0.1 那段"黑掉的时间"里**本来是静默的** ——
//      本讲靠 render() 每 300ms 补一行「执行中」把它补上（H7 的耗时条同源）；
//    · 权限弹窗就是一行 `[权限] ... 允许? [y/N]` 加一次阻塞读。
//
//  ★★ 但它有一个 TUI 没有的好处：**输出零 ANSI，日志永远可读**。
//     所以它是所有验证用例的默认路径 —— 这不是妥协，是分工：
//     TUI 负责"人看着舒服"，plain 负责"机器断言得住"。
//
//  ★ 输出格式与前 8 讲**逐字节保持一致**（`[模型请求工具]` / `[事件] onToolStart` /
//    `[工具返回]` 等）。L9 是加一层 UI，不是重写一遍输出 ——
//    133 条回归用例正是靠这一点活下来的。
// ---------------------------------------------------------------------------
#include "tui.hpp"

#include "tui_web.hpp"          // ★ 题目八：AF_UI=web 的后端工厂

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace af::ui {

#ifdef AF_HAVE_FTXUI
// 定义在 tui_ftxui.cpp。只有那个文件被编进来时才声明 —— 否则链接不到。
std::unique_ptr<Renderer> makeFtxui();
#endif

// ===========================================================================
//  事件队列
// ===========================================================================
void EventQueue::push(UiEvent e) {
    std::unique_lock<std::mutex> lk(mu_);
    // ★ 满则阻塞生产者（背压）。关闭后不再阻塞，直接返回。
    notFull_.wait(lk, [&] { return q_.size() < cap_ || closed_; });
    if (closed_) return;
    q_.push_back(std::move(e));
    notEmpty_.notify_one();
}

std::vector<UiEvent> EventQueue::drain() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<UiEvent> out(q_.begin(), q_.end());
    q_.clear();
    notFull_.notify_all();
    return out;
}

void EventQueue::close() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
    }
    notFull_.notify_all();
    notEmpty_.notify_all();
}

bool EventQueue::closed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return closed_;
}

std::size_t EventQueue::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

// ===========================================================================
//  ANSI / TTY
// ===========================================================================
std::string stripAnsi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b') {
            std::size_t j = i + 1;
            if (j < s.size() && s[j] == '[') {
                // CSI：一直吃到终止字节（字母）
                while (j < s.size() && !((s[j] >= 'A' && s[j] <= 'Z') ||
                                         (s[j] >= 'a' && s[j] <= 'z'))) ++j;
                if (j < s.size()) ++j;
            } else if (j < s.size()) {
                j = i + 2;                       // 其它两字节转义
            }
            i = j - 1;                            // for 循环会 ++i
            continue;
        }
        out += s[i];
    }
    return out;
}

bool stdoutIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

bool stdinIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

void writeFrame(const std::string& frame, bool isTty) {
    if (frame.empty()) return;
    if (isTty) {
        std::cout << frame << std::flush;
    } else {
        // ★ 降级唯一发生的地方：剥掉 ANSI 再写。
        std::cout << stripAnsi(frame) << std::flush;
    }
}

// ===========================================================================
//  PlainRenderer
// ===========================================================================
namespace {

using Clock = std::chrono::steady_clock;

class PlainRenderer final : public Renderer {
public:
    const char* name() const override { return "plain"; }
    bool        isTui() const override { return false; }

    void apply(const UiEvent& e) override {
        switch (e.kind) {
        case UiEvent::Kind::Text:
            std::cout << e.text << std::flush;
            break;

        case UiEvent::Kind::Thinking:
            // ★ 行打印机**不输出任何 ANSI**（哪怕是为了"变暗"）。
            //   一旦开了这个口子，C27 那种"输出里不得有 \x1b"的断言就守不住了。
            std::cout << "[思考] " << e.text << "\n" << std::flush;
            break;

        case UiEvent::Kind::ToolCall:
            std::cout << "\n[模型请求工具] " << e.toolName << "\n" << std::flush;
            break;

        case UiEvent::Kind::ToolArgs:
            std::cout << "[参数] " << e.text << "\n" << std::flush;
            break;

        case UiEvent::Kind::ToolStart:
            std::cout << "[事件] onToolStart: " << e.toolName << "\n" << std::flush;
            inFlight_ = true;
            inFlightName_ = e.toolName;
            inFlightId_ = e.toolId;
            inFlightAt_ = Clock::now();
            break;

        case UiEvent::Kind::ToolDone:
            inFlight_ = false;
            std::cout << "[事件] onToolDone: " << (e.isError ? "错误" : "成功")
                      << " (" << static_cast<int>(e.elapsedMs) << "ms)\n"
                      << (e.isError ? "[工具失败] " : "[工具返回] ")
                      << e.text << "\n\n" << std::flush;
            break;

        case UiEvent::Kind::Notice:
            std::cout << "[事件] onNotice: " << e.text << "\n" << std::flush;
            break;

        case UiEvent::Kind::Error:
            std::cerr << "\n[请求失败] " << e.text << "\n" << std::flush;
            break;

        case UiEvent::Kind::StreamError:
            std::cerr << "\n[流内错误] " << e.text << "\n" << std::flush;
            break;

        case UiEvent::Kind::Usage:
            lastTokens_ = e.tokens;
            break;

        case UiEvent::Kind::StreamDone:
            break;

        case UiEvent::Kind::PermAsk:
            pending_ = true;
            pendingId_ = e.promptId;
            pendingPrompt_ = e.text;
            // ★ 行打印机画不出"浮在上面的一层"，就只能把它**按顺序**写下来。
            //   但顺序本身就是信息：它出现在"对话流之后"，和画布里的 overlay 同源。
            std::cout << "\n[权限] " << e.text << "\n  允许? [y/N] " << std::flush;
            break;

        case UiEvent::Kind::PermAnswer:
            if (e.promptId == pendingId_) pending_ = false;
            break;
        }
    }

    // ★ 行打印机没有"一帧"的概念：状态本来就已经写出去了。
    //   唯一的例外是「执行中」—— 它是**渲染循环**造出来的中间态，
    //   Agent 线程那边根本没有对应的事件（这正是"渲染线程独立"的第一个收益）。
    std::string render() override {
        if (inFlight_) {
            const double ms = std::chrono::duration<double, std::milli>(
                                  Clock::now() - inFlightAt_).count();
            std::cout << "[执行中] " << inFlightName_
                      << " 已运行 " << static_cast<int>(ms) << "ms\n" << std::flush;
        }
        return {};
    }

    bool toolInFlight() const override { return inFlight_; }

    bool askPermission(const std::string& prompt, int id) override {
        pending_ = true;
        pendingPrompt_ = prompt;
        pendingId_ = id;
        std::cout << "\n[权限] " << prompt << "\n  允许? [y/N] " << std::flush;

        // ★ 非交互 stdin 必须 fail-closed 且**不能阻塞**。
        //   阻塞在这里的后果是：CI 里一次权限询问把整轮卡死，
        //   而日志里什么都看不出来（连 "[权限]" 都可能因为缓冲没落盘）。
        if (!stdinIsTty()) {
            std::cout << "\n[权限] 非交互环境，按拒绝处理\n" << std::flush;
            pending_ = false;
            return false;
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n[权限] 读不到输入，按拒绝处理\n" << std::flush;
            pending_ = false;
            return false;
        }
        pending_ = false;
        return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
    }

    void answerPermission(int, bool) override {
        // 纯文本路径是同步阻塞读的，不需要回填。
    }

    bool hasPendingPrompt() const override { return pending_; }

private:
    bool        inFlight_ = false;
    std::string inFlightName_, inFlightId_;
    Clock::time_point inFlightAt_{};

    bool        pending_ = false;
    int         pendingId_ = 0;
    std::string pendingPrompt_;

    int         lastTokens_ = 0;
};

}  // namespace

std::unique_ptr<Renderer> makeRenderer(bool wantTui) {
    // ---- ★ 题目八：AF_UI=web ------------------------------------------------
    //  ★ 为什么在这里读环境变量，而不是让 main 传一个"后端枚举"进来：
    //    makeRenderer 的职责就是"按环境挑一个后端"，而 wantTui 这个 bool 已经
    //    装不下第三个后端了。改签名就要改 main 的调用点 —— 那是核心代码。
    //    在这里读，main 一行不动（这正是 CR-3 "Renderer 接口零改动"的含义）。
    //
    //  ★ 失败必须降级：端口被占 / WSA 起不来 → 落到下面返回 plain。
    //    "makeRenderer 永远返回非空" 是 I-44/I-46 守了两讲的不变量，不能破。
    {
        const char* ui = std::getenv("AF_UI");
        if (ui && std::string(ui) == "web") {
            auto web = makeWebRenderer(webPortFromEnv());
            if (web) {
                // ★ 题目验收「无浏览器时退回 PlainRenderer」的判据（AF_WEB_WAIT_MS）：
                //   ① 显式设了：按设的值等，等不到就退回 plain；
                //   ② 没设，且是 --web（AF_WEB_REQUIRE_CLIENT=1）：不等，直接给 WebRenderer
                //      —— 用户双击完马上就会开浏览器，等在这里只会让人以为"卡住了"；
                //   ③ 没设，也不是 --web（脚本 / 验收里的纯 AF_UI=web）：默认等 3 秒，
                //      等不到就**真的**返回 PlainRenderer。这样"无浏览器 → plain"
                //      不需要额外设变量也成立（否则这条只在 AF_WEB_WAIT_MS>0 时才真）。
                const char* w = std::getenv("AF_WEB_WAIT_MS");
                const char* need = std::getenv("AF_WEB_REQUIRE_CLIENT");
                const bool requireClient = need && std::string(need) == "1";
                const int waitMs = (w && *w) ? std::atoi(w) : (requireClient ? 0 : 3000);
                if (waitMs <= 0 || webWaitClient(*web, waitMs)) return web;
                std::cout << "[UI] web：等了 " << waitMs
                          << "ms 没有浏览器连上，降级为 plain"
                             "（AF_WEB_WAIT_MS 可调；--web 不等）\n" << std::flush;
                web.reset();   // 关掉监听，别占着端口
            } else {
                // 走到这里说明 web 起不来 —— 静默降级会让人以为"我明明设了 web"
                std::cout << "[UI] web 后端不可用（端口 " << webPortFromEnv()
                          << " 被占？），降级为 plain\n" << std::flush;
            }
        }
    }

#ifdef AF_HAVE_FTXUI
    if (wantTui) {
        auto r = makeFtxui();
        if (r) return r;
        // ★ 拿不到就退回 plain —— 降级**永远**有路可走。
    }
#else
    (void)wantTui;
#endif
    return std::make_unique<PlainRenderer>();
}

// ===========================================================================
//  渲染循环（伪码 P11）
// ===========================================================================
UiLoop::UiLoop(std::unique_ptr<Renderer> r, bool isTty)
    : r_(std::move(r)), isTty_(isTty) {
    th_ = std::thread([this] { loop(); });
}

UiLoop::~UiLoop() { stop(); }

void UiLoop::push(UiEvent e) {
    q_.push(std::move(e));
    events_.fetch_add(1);
}

void UiLoop::stop() {
    if (!running_.exchange(false)) { /* 已经停了 */ }
    q_.close();
    if (th_.joinable()) th_.join();
}

void UiLoop::loop() {
    using Ms16 = std::chrono::milliseconds;
    bool dirty = false;
    auto lastSpin = std::chrono::steady_clock::now();

    // ★ 退出条件是 "running 假 且 队列空"：
    //   只判 running 会把最后几个事件吞掉 —— 表现是"模型没说话就结束了"。
    while (running_.load() || q_.size() > 0) {
        const auto frameStart = std::chrono::steady_clock::now();

        const auto evs = q_.drain();
        for (const auto& e : evs) { r_->apply(e); dirty = true; }

        // ★ 工具执行期间**没有事件也会出帧**：spinner / 耗时条就靠它。
        //   这是"渲染线程独立"才做得到的事 —— 由 Agent 线程推事件的话，
        //   工具跑 30 秒，这 30 秒里界面上什么都不会发生。
        if (r_->toolInFlight() &&
            frameStart - lastSpin >= std::chrono::milliseconds(300)) {
            lastSpin = frameStart;
            spins_.fetch_add(1);
            dirty = true;
        }

        if (dirty) {
            const std::string frame = r_->render();
            writeFrame(frame, isTty_);
            frames_.fetch_add(1);
            dirty = false;
        }
        std::this_thread::sleep_until(frameStart + Ms16(16));
    }

    // 收尾：把队列里剩下的事件全部消费掉再走。
    for (const auto& e : q_.drain()) r_->apply(e);
    writeFrame(r_->render(), isTty_);
}

}  // namespace af::ui
