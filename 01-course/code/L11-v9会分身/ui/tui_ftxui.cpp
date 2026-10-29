// ---------------------------------------------------------------------------
//  ui/tui_ftxui.cpp —— FTXUI 渲染器（画布模式）
//
//  ★★ 这个文件是**唯一**包含 FTXUI 头文件的编译单元。
//     库不在场时 build.ps1 直接不编它，整套代码仍能用（→ plain）。
//     与 L8 把 tree-sitter 关进 symbol_extract.cpp 是同一个决策：
//     **可选依赖必须只有唯一入口。**
//
//  ★ 为什么**不走** ScreenInteractive（交互式全屏 Loop）：
//     1. 我们的验证体系把 stdout 重定向到文件再 grep，一进交互 Loop 就全废；
//     2. 交互 Loop 会接管终端、改代码页、吞按键 —— 在 CI 里是灾难；
//     3. 真正"画布"的部分（Element 树 → 字符矩阵）与交互无关，
//        用 Screen::Create + Render + ToString() 就能拿到完全一样的产物。
//
//     所以这里做的是**离屏渲染**：状态 → Element 树 → 字符矩阵 → 字符串。
//     真要做交互 Loop，只需要把最后一步换成 screen.Loop() —— 状态机一行不用改。
//     这本身就是"渲染与 I/O 解耦"的收益。
// ---------------------------------------------------------------------------
#include "tui.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <iostream>
#include <sstream>

namespace af::ui {
namespace {

using Clock = std::chrono::steady_clock;
using namespace ftxui;

// ★ 用 ASCII 而不是 braille（⠋⠙⠹…）：
//   控制台代码页在重定向/非 UTF-8 环境下不可控，ASCII 在任何代码页下都画得出来。
//   "少一个好看的字符"换"在哪儿都不会变乱码" —— 这笔交易永远划算。
const char* kSpin = "|/-\\";

struct ToolCard {
    std::string name, id;
    bool        done = false;
    bool        err  = false;
    double      ms   = 0;
    Clock::time_point startedAt{};
};

class FtxuiRenderer final : public Renderer {
public:
    const char* name() const override { return "ftxui"; }
    bool        isTui() const override { return true; }

    void apply(const UiEvent& e) override {
        switch (e.kind) {
        case UiEvent::Kind::Text:
            pushText(e.text);
            break;

        case UiEvent::Kind::Thinking:
            pushText("[思考] " + e.text);
            break;

        case UiEvent::Kind::ToolCall:
            pushText("[模型请求工具] " + e.toolName);
            break;

        case UiEvent::Kind::ToolArgs:
            pushText("[参数] " + e.text);
            break;

        case UiEvent::Kind::ToolStart: {
            // ★ 卡片按 id 归档（不是按"最后一个"）：并行工具调用时
            //   按"当前"判会串（L5 的 K-9 换了个皮 —— 判据永远是 id 集合）。
            ToolCard c;
            c.name = e.toolName;
            c.id = e.toolId;
            c.startedAt = Clock::now();
            tools_.push_back(std::move(c));
            break;
        }

        case UiEvent::Kind::ToolDone: {
            auto it = std::find_if(tools_.begin(), tools_.end(),
                                   [&](const ToolCard& c) { return c.id == e.toolId; });
            if (it == tools_.end()) {
                ToolCard c; c.name = e.toolName; c.id = e.toolId; tools_.push_back(c);
                it = tools_.end() - 1;
            }
            it->done = true;
            it->err  = e.isError;
            it->ms   = e.elapsedMs;
            pushText((e.isError ? "[失败] " : "[完成] ") + e.toolName);
            break;
        }

        case UiEvent::Kind::Notice:
            pushText("[通知] " + e.text);
            break;

        case UiEvent::Kind::Error:
            pushText("[请求失败] " + e.text);
            break;

        case UiEvent::Kind::StreamError:
            pushText("[流内错误] " + e.text);
            break;

        case UiEvent::Kind::Usage:
            tokens_ = e.tokens;
            break;

        case UiEvent::Kind::StreamDone:
            phase_ = "完成";
            break;

        case UiEvent::Kind::PermAsk:
            pending_ = true;
            pendingId_ = e.promptId;
            pendingPrompt_ = e.text;
            break;

        case UiEvent::Kind::PermAnswer:
            if (e.promptId == pendingId_) { pending_ = false; }
            break;
        }
    }

    std::string render() override {
        // 累积的未换行正文先落地成一行（画布是可重画的，所以"当前行"是状态）
        if (!partial_.empty()) { log_.push_back(partial_); partial_.clear(); }

        spinIdx_ = (spinIdx_ + 1) % 4;

        Elements body;
        body.push_back(text("AgentForge v7（画布模式）") | bold);
        body.push_back(separator());

        // ---- 对话流（★ 只显示最后 N 行：画布的高度是有限的）----
        const std::size_t keep = 14;
        const std::size_t from = log_.size() > keep ? log_.size() - keep : 0;
        for (std::size_t i = from; i < log_.size(); ++i)
            body.push_back(text(log_[i]) | color(Color::White));

        // ---- 工具卡片 ----
        Elements cards;
        for (const auto& t : tools_) {
            if (t.done) {
                const double ms = t.ms;
                std::ostringstream os;
                os << (t.err ? " x " : " v ") << t.name
                   << "  " << static_cast<int>(ms) << "ms";
                cards.push_back(text(os.str()) |
                                color(t.err ? Color::Red : Color::Green));
            } else {
                const double ms = std::chrono::duration<double, std::milli>(
                                      Clock::now() - t.startedAt).count();
                std::ostringstream os;
                os << " " << kSpin[spinIdx_] << " " << t.name
                   << "  执行中 " << static_cast<int>(ms) << "ms";
                cards.push_back(text(os.str()) | color(Color::Yellow));
            }
        }
        if (!cards.empty()) {
            body.push_back(separator());
            for (auto& c : cards) body.push_back(std::move(c));
        }

        // ---- 权限弹窗（★ overlay：画在底部，对话流在它上面继续）----
        if (pending_) {
            body.push_back(separator());
            body.push_back(text("[权限] " + pendingPrompt_ + "  允许? [y/N]") |
                           bold | color(Color::Red) | border);
        }

        body.push_back(separator());
        body.push_back(text("状态: " + phase_ + "   tokens=" +
                            std::to_string(tokens_)) | dim);

        auto doc = vbox(std::move(body)) | border;

        // ★ 离屏渲染：Element 树 → 字符矩阵 → 字符串（可能含 ANSI）
        auto screen = Screen::Create(Dimension::Full(), Dimension::Fit(doc));
        Render(screen, doc);
        return screen.ToString();
    }

    bool toolInFlight() const override {
        return std::any_of(tools_.begin(), tools_.end(),
                           [](const ToolCard& c) { return !c.done; });
    }

    bool askPermission(const std::string& prompt, int id) override {
        pending_ = true;
        pendingId_ = id;
        pendingPrompt_ = prompt;
        // ★ 非交互环境 fail-closed 且不阻塞（与 plain 同一条规矩）。
        if (!stdinIsTty()) return false;
        std::string line;
        if (!std::getline(std::cin, line)) { pending_ = false; return false; }
        pending_ = false;
        return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
    }

    void answerPermission(int id, bool yes) override {
        if (id == pendingId_) { pending_ = false; lastAnswer_ = yes; }
    }

    bool hasPendingPrompt() const override { return pending_; }

private:
    void pushText(const std::string& s) {
        // 按行切：画布上"一行"才是可渲染单位
        std::size_t pos = 0;
        while (pos < s.size()) {
            const std::size_t nl = s.find('\n', pos);
            if (nl == std::string::npos) { partial_ += s.substr(pos); break; }
            partial_ += s.substr(pos, nl - pos);
            log_.push_back(partial_);
            partial_.clear();
            pos = nl + 1;
        }
        if (log_.size() > 500) log_.erase(log_.begin(), log_.begin() + 200);
    }

    std::vector<std::string> log_;
    std::string              partial_;
    std::vector<ToolCard>    tools_;
    std::string              phase_ = "运行中";
    int                      tokens_ = 0;
    int                      spinIdx_ = 0;
    bool                     pending_ = false;
    int                      pendingId_ = 0;
    std::string              pendingPrompt_;
    bool                     lastAnswer_ = false;
};

}  // namespace

std::unique_ptr<Renderer> makeFtxui() { return std::make_unique<FtxuiRenderer>(); }

}  // namespace af::ui
