// ---------------------------------------------------------------------------
//  ui/tui.hpp —— 终端 UI 的接口层：事件、队列、渲染循环
//
//  ★ 这个头文件**不**包含任何 FTXUI 头文件。
//    这样"没有 FTXUI"的构建也能编译整套代码 —— 可选依赖被关在
//    tui_ftxui.cpp 一个编译单元里（与 L8 的 symbol_extract 同一个决策）。
//
//  ★ 为什么不用端口（IRenderer）？
//    端口是给"模块边界**之外**的第二个实现者"用的。渲染器只有 TTY / 非 TTY
//    两种形态，**变化不会来自这个模块之外** → 用"两个 .cpp 实现同一个头文件"，
//    运行时选，不额外引入一层多态。
//    判据：**"该不该加接口"问的不是"它将来会不会变"，而是"变化会不会来自外部"。**
//
//  ★ 为什么不加进 IUIPort？
//    IUIPort 是"核心 → 问 UI"（confirm / notice）；
//    LoopHooks 是"核心 → 告诉 UI"（事件流）。**两个方向，两个抽象，不要合并。**
//    往 IUIPort 里塞流式输出，等于让核心知道 UI 需要什么 —— 分层就破了。
//
//  ★ types.hpp 本讲**零改动**（CR-3）：UiEvent 是 UI 层的概念，放这里。
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace af::ui {

// ===========================================================================
//  事件
//
//  三个来源，**不合并成一个大 struct**：
//    · ChatChunk（L3 的 variant，8 个分支）→ 协议层"模型说了什么"
//    · LoopHooks（L5 的观察者，6 个回调）→ 循环层"发生了什么"
//    · UI 层自己                          → 权限询问与回答
//
//  ★ ToolCallDelta 故意**没有**对应的事件：
//    参数 delta 是"半个 JSON"，画不出来也没必要画。它在 ToolArgs 时一次性落地。
//    （这也是"事件 ≠ 数据帧"的一个例子：UI 只该收到**能消费的东西**。）
// ===========================================================================
struct UiEvent {
    enum class Kind {
        // —— 来自 ChatChunk ——
        Text,          // 正文增量
        Thinking,      // 思考过程（可折叠）
        ToolCall,      // 模型请求了工具（ToolCallStart）
        ToolArgs,      // 参数收全了（ToolCallDone）
        StreamError,   // 流内错误
        Usage,         // 用量
        StreamDone,    // 本轮流结束

        // —— 来自 LoopHooks ——
        ToolStart,     // 工具**开始执行**
        ToolDone,      // 工具**执行完毕**（★ H7 的 elapsedMs 在这里）
        Notice,        // 循环的通知
        Error,         // 请求失败

        // —— UI 层自己 ——
        PermAsk,       // 弹一个权限询问
        PermAnswer     // 用户回答了
    };

    Kind        kind      = Kind::Text;
    std::string text;                 // Text/Thinking/Notice/Error/ToolArgs 的正文
    std::string toolName, toolId;     // ToolCall / ToolArgs / ToolStart / ToolDone
    bool        isError   = false;    // ToolDone / Error / StreamError
    double      elapsedMs = 0;        // ★ ToolDone：H7 的"耗时"落在这里
    int         tokens    = 0;        // Usage
    int         promptId  = 0;        // ★ PermAsk/PermAnswer 的配对 id（见 K-9）
    bool        answer    = false;    // PermAnswer
};

// ===========================================================================
//  事件队列（生产者 = Agent 线程，消费者 = 渲染线程）
//
//  ★★ 必须有界 + 背压。
//    无界队列在"模型高速吐 token、渲染线程卡了一下"时会把内存吃光。
//    有界队列满时**阻塞生产者**，是"把压力还给源头" —— 模型那边本来就有
//    TCP 窗口在限流，多一层背压不会丢数据，只会让上游稍微慢一点。
//
//  ★ 这是本讲"渲染线程与 Agent 线程分离"的地基：
//    两个线程之间**只**通过这一个队列说话，没有共享的可变状态。
// ===========================================================================
class EventQueue {
public:
    explicit EventQueue(std::size_t cap = 4096) : cap_(cap) {}

    // 生产者侧。★ 满则阻塞（背压）。队列已关闭时直接返回（不阻塞、不抛）
    void push(UiEvent e);

    // 消费者侧。一次取空。
    std::vector<UiEvent> drain();

    void close();
    bool closed() const;
    std::size_t size() const;

private:
    mutable std::mutex       mu_;
    std::condition_variable  notFull_, notEmpty_;
    std::deque<UiEvent>      q_;
    std::size_t              cap_;
    bool                     closed_ = false;
};

// ===========================================================================
//  渲染器（★ 只有两个实现：tui_ftxui.cpp / tui_plain.cpp）
// ===========================================================================
class Renderer {
public:
    virtual ~Renderer() = default;

    virtual const char* name() const = 0;         // "ftxui" | "plain"
    virtual bool        isTui() const = 0;        // 是不是画布模式

    // 把事件吸收成状态。**不画图。**
    virtual void apply(const UiEvent& e) = 0;

    // 状态 → 一帧。
    // ★ 返回的是"这一帧的文本"，**可能含 ANSI**（画布模式）。
    //   调用方负责在写入前决定要不要剥 —— 见 writeFrame()。
    virtual std::string render() = 0;

    // 权限询问。★ 阻塞的是**调用它的那条线程**（Agent 线程），
    //   渲染线程照常在跑 —— 这正是"弹窗不阻塞渲染"的实现方式。
    virtual bool askPermission(const std::string& prompt, int promptId) = 0;

    // 用户批准/拒绝后由 UI 侧回填（供 askPermission 内部等待）
    virtual void answerPermission(int promptId, bool yes) = 0;

    // 当前有没有一个正在等待回答的询问（给"弹窗不阻塞渲染"做判据）
    virtual bool hasPendingPrompt() const = 0;

    // ★ 有工具正在执行中吗？（渲染循环据此决定要不要继续出帧 —— 见 UiLoop）
    virtual bool toolInFlight() const = 0;
};

// 选择渲染器：wantTui = 是 TTY 且用户没强制 plain。
// ★ FTXUI 没被编进来时，这里永远返回 plain —— 降级不是瘫痪（C32）。
std::unique_ptr<Renderer> makeRenderer(bool wantTui);

// ===========================================================================
//  渲染循环（★ 伪码 P11 的落地）
//
//  Agent 线程只 push，渲染线程只 render —— 中间只有 EventQueue。
// ===========================================================================
class UiLoop {
public:
    UiLoop(std::unique_ptr<Renderer> r, bool isTty);
    ~UiLoop();

    UiLoop(const UiLoop&)            = delete;
    UiLoop& operator=(const UiLoop&) = delete;

    void push(UiEvent e);              // ★ 非阻塞语义：只做入队，立刻返回
    void stop();                       // 关闭队列 + join（幂等）
    Renderer& renderer() { return *r_; }

    int events() const { return events_.load(); }   // 入队事件数（C28）
    int frames() const { return frames_.load(); }   // 出帧数（C29 / C30）
    int  spinnerTicks() const { return spins_.load(); }

private:
    void loop();

    std::unique_ptr<Renderer> r_;
    bool                      isTty_;
    EventQueue                q_;
    std::thread               th_;
    std::atomic<int>          events_{0}, frames_{0}, spins_{0};
    std::atomic<bool>         running_{true};
};

// ===========================================================================
//  ANSI 与 TTY
//
//  ★★ 本讲最重要的工程决定：非 TTY 时输出里**不许有 ANSI 转义**。
//     理由：全部验证用例都靠"stdout 重定向到文件再 grep"，
//     一条漏网的转义就足以让某条断言失配，且报错会出现在**另一个**用例里
//     （K-21 那一类：犯错的地方和报错的地方不是同一个）。
//     见讲义 §0.3 / §1.6（K-32）。
// ===========================================================================

// 剥掉 CSI 等转义序列。
// ★ 不能写成"找到 \x1b 就跳过 3 个字符" —— CSI 序列长度不定：
//   "\x1b[1m" 是 4 字节，"\x1b[38;5;196m" 是 11 字节。必须**吃到终止字母**。
std::string stripAnsi(std::string_view s);

// stdout / stdin 是不是终端（而不是文件/管道）
bool stdoutIsTty();
bool stdinIsTty();

// 把一帧写出去。isTty == false 时先剥 ANSI。
// ★ 这是"降级"唯一起作用的地方 —— 集中在一处，别散落。
void writeFrame(const std::string& frame, bool isTty);

}  // namespace af::ui
