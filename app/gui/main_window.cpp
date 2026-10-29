// ---------------------------------------------------------------------------
//  L11 · main_window.cpp —— AgentForge 桌面工作台（Apple HIG 风格）
//
//  设计语言：
//    · 底色 #F5F5F7 / 卡片纯白 / 发丝线 #E8E8ED，单一系统蓝 #0071E3 强调
//    · 系统字体栈（Segoe UI Variable / 微软雅黑），等宽字体只用于数据与代码
//    · 柔软圆角（12–22px）、克制阴影、充足留白（Operate 型：工具消失于任务）
//    · 语义色：完成 #248A3D、失败 #D70015、提示 #6E6E73
//
//  结构：左侧边栏（品牌/快捷/会话/状态）| 中央对话流 | 右侧工具详情 + 底部状态条
//  线程：AgentLoop 在 worker 线程运行；所有 UI 更新经 QueuedConnection 投递。
// ---------------------------------------------------------------------------
#include "main_window.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStatusBar>
#include <QTextCharFormat>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "agent_loop.hpp"
#include "config.hpp"
#include "context_budget.hpp"
#include "logging.hpp"
#include "model_provider.hpp"
#include "repo_index.hpp"
#include "sandbox.hpp"
#include "secret.hpp"
#include "session_store.hpp"
#include "tool_registry.hpp"
#include "tools.hpp"
#include "types.hpp"

namespace af::gui {

// ============================================================================
//  设计令牌（一处定义，全局复用）
// ============================================================================
namespace {

constexpr const char* kFontUI   = "Segoe UI Variable Display, Segoe UI, Microsoft YaHei UI, Microsoft YaHei";
constexpr const char* kFontMono = "Cascadia Mono, Consolas, Microsoft YaHei UI";

constexpr const char* kSystemPrompt =
    "你是一个编码 Agent。原则：\n"
    "1. 需要文件内容时必须**调用工具去读**，不要凭猜测回答。\n"
    "2. 工具报错是信息不是失败 —— 读它，然后换方法。\n"
    "3. 回答要给出依据（哪个文件、哪几行）。";

QString styleSheet() {
    return QStringLiteral(R"(
* { font-family: "Segoe UI Variable Display","Segoe UI","Microsoft YaHei UI"; }
QMainWindow, QWidget { background: #F5F5F7; color: #1D1D1F; }
QLabel#brand    { font-size: 21px; font-weight: 700; letter-spacing: 0.2px; color: #1D1D1F; }
QLabel#tagline  { color: #86868B; font-size: 11px; }
QLabel#sideSection { color: #86868B; font-size: 10px; font-weight: 700; letter-spacing: 1.2px; }

QWidget#side { background: #FBFBFD; border-right: 1px solid #E8E8ED; }

QPushButton#sideItem { text-align: left; padding: 7px 10px; border: none; border-radius: 8px;
                       color: #1D1D1F; font-size: 13px; background: transparent; }
QPushButton#sideItem:hover { background: rgba(0,0,0,0.045); }
QPushButton#sideItem:pressed { background: rgba(0,0,0,0.07); }

QFrame#metaCard { background: #FFFFFF; border: 1px solid #E8E8ED; border-radius: 12px; }
QLabel#metaKey { color: #86868B; font-size: 11px; }
QLabel#metaVal { color: #1D1D1F; font-size: 11px; font-weight: 600; }

QFrame#chatArea { background: transparent; }
QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: rgba(0,0,0,0.14); border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: rgba(0,0,0,0.24); }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

QLabel#emptyHint   { color: #86868B; font-size: 15px; font-weight: 600; }
QLabel#emptySub    { color: #A1A1A6; font-size: 12px; }

QFrame#bubbleUser { background: #0071E3; border-radius: 18px; }
QLabel#bubbleUserText { color: #FFFFFF; font-size: 13px; background: transparent; }

QFrame#cardAssistant { background: #FFFFFF; border: 1px solid #E8E8ED; border-radius: 14px; }
QLabel#roleCap { color: #A1A1A6; font-size: 10px; font-weight: 700; letter-spacing: 1.4px; background: transparent; }
QLabel#assistantText { color: #1D1D1F; font-size: 13px; background: transparent; }

QFrame#toolCard { background: #FBFBFD; border: 1px solid #E8E8ED; border-radius: 12px; }
QFrame#toolCard:hover { border-color: #D6D6DC; background: #FFFFFF; }
QLabel#toolChip { background: rgba(0,113,227,0.10); color: #0071E3; font-size: 10px;
                  font-weight: 700; padding: 2px 8px; border-radius: 8px; }
QLabel#toolName { color: #1D1D1F; font-size: 12px; font-weight: 600; }
QLabel#toolPreview { color: #6E6E73; font-size: 11px; }
QLabel#toolState { color: #86868B; font-size: 10px; font-weight: 700; letter-spacing: 0.6px; }

QLabel#notice { color: #86868B; font-size: 11px; }
QLabel#errorPill { background: rgba(215,0,21,0.08); color: #D70015; font-size: 11px;
                   padding: 4px 10px; border-radius: 8px; }
QLabel#donePill { background: rgba(36,138,61,0.08); color: #248A3D; font-size: 11px;
                  padding: 4px 10px; border-radius: 8px; }

QFrame#composer { background: #FFFFFF; border: 1px solid #E8E8ED; border-radius: 22px; }
QFrame#composer[focused="true"] { border-color: #B9D2F7; }
QLineEdit#input { border: none; background: transparent; font-size: 13px; padding: 6px 8px; color: #1D1D1F; }
QPushButton#sendBtn { background: #0071E3; color: #FFFFFF; border: none; border-radius: 15px;
                      padding: 7px 18px; font-size: 12px; font-weight: 700; }
QPushButton#sendBtn:hover { background: #0077ED; }
QPushButton#sendBtn:disabled { background: #C9D8F2; }
QPushButton#stopBtn { background: transparent; color: #6E6E73; border: 1px solid #E8E8ED;
                      border-radius: 15px; padding: 6px 14px; font-size: 12px; }
QPushButton#stopBtn:hover { color: #D70015; border-color: #F3C7CD; background: #FFF9F9; }

QPushButton#chipBtn { background: rgba(0,0,0,0.04); color: #6E6E73; border: none;
                      border-radius: 9px; padding: 4px 10px; font-size: 11px; }
QPushButton#chipBtn:hover { background: rgba(0,113,227,0.10); color: #0071E3; }

QLabel#statusChip { background: #FFFFFF; border: 1px solid #E8E8ED; color: #6E6E73;
                    border-radius: 9px; padding: 2px 8px; font-size: 10px; }
QLabel#statusLabel { color: #86868B; font-size: 11px; }

QFrame#detailCard { background: #FFFFFF; border: 1px solid #E8E8ED; border-radius: 12px; }
QLabel#detailTitle { color: #1D1D1F; font-size: 12px; font-weight: 700; }
QLabel#detailSub   { color: #A1A1A6; font-size: 10px; }
QPlainTextEdit#detail { background: #FFFFFF; border: none; color: #1D1D1F; font-size: 11px;
                        selection-background-color: #0071E3; selection-color: #ffffff; }
)");
}

}  // namespace

// ============================================================================
//  ToolCard —— 工具调用卡片（可点击，详情进右侧面板）
// ============================================================================
class ToolCard : public QFrame {
public:
    using ClickFn = std::function<void(const QString& name, const QString& args,
                                       const QString& result, bool error)>;

    ToolCard(QString name, QString args, ClickFn onClick, QWidget* parent = nullptr)
        : QFrame(parent), name_(std::move(name)), args_(std::move(args)),
          onClick_(std::move(onClick)) {
        setObjectName("toolCard");
        setCursor(Qt::PointingHandCursor);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 9, 12, 9);
        v->setSpacing(4);

        auto* head = new QHBoxLayout();
        head->setSpacing(8);
        auto* chip = new QLabel("工具");
        chip->setObjectName("toolChip");
        auto* nameL = new QLabel(name_);
        nameL->setObjectName("toolName");
        state_ = new QLabel("运行中…");
        state_->setObjectName("toolState");
        head->addWidget(chip);
        head->addWidget(nameL);
        head->addStretch(1);
        head->addWidget(state_);
        v->addLayout(head);

        argsL_ = new QLabel(elide(args_, 180));
        argsL_->setObjectName("toolPreview");
        argsL_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(argsL_);

        resultL_ = new QLabel();
        resultL_->setObjectName("toolPreview");
        resultL_->setWordWrap(false);
        resultL_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        resultL_->setVisible(false);
        v->addWidget(resultL_);
    }

    void finish(const QString& result, bool error) {
        if (finished_) return;
        finished_ = true;
        error_ = error;
        result_ = result;
        resultL_->setText(elide(result, 220));
        resultL_->setVisible(true);
        state_->setText(error ? "失败" : "完成");
        state_->setStyleSheet(error
                                  ? "color:#D70015; font-size:10px; font-weight:700; letter-spacing:0.6px;"
                                  : "color:#248A3D; font-size:10px; font-weight:700; letter-spacing:0.6px;");
    }

    static QString elide(const QString& s, int max) {
        QString one = s;
        one.replace('\n', ' ');
        return one.size() > max ? one.left(max) + "…" : one;
    }

protected:
    void mousePressEvent(QMouseEvent* ev) override {
        if (ev->button() == Qt::LeftButton && onClick_)
            onClick_(name_, args_, result_, error_);
        QFrame::mousePressEvent(ev);
    }

private:
    QString name_;
    QString args_;
    QString result_;
    bool    error_ = false;
    bool    finished_ = false;
    QLabel* state_ = nullptr;
    QLabel* argsL_ = nullptr;
    QLabel* resultL_ = nullptr;
    ClickFn onClick_;
};

// ============================================================================
//  ChatView —— 消息流（用户气泡 / 助手卡片 / 工具卡片 / 提示）
// ============================================================================
class ChatView : public QScrollArea {
public:
    using ClickFn = std::function<void(const QString&, const QString&,
                                       const QString&, bool)>;

    explicit ChatView(QWidget* parent = nullptr) : QScrollArea(parent) {
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        inner_ = new QWidget;
        inner_->setObjectName("chatArea");
        box_ = new QVBoxLayout(inner_);
        box_->setContentsMargins(28, 22, 28, 22);
        box_->setSpacing(12);
        box_->addStretch(1);
        setWidget(inner_);

        empty_ = new QWidget(this);
        auto* ev = new QVBoxLayout(empty_);
        ev->setSpacing(8);
        auto* h = new QLabel("向 AgentForge 提出第一个任务");
        h->setObjectName("emptyHint");
        auto* s = new QLabel("例如：读一下 mock_target.txt 里的函数，或用 /help 查看命令");
        s->setObjectName("emptySub");
        ev->addStretch(1);
        ev->addWidget(h, 0, Qt::AlignHCenter);
        ev->addWidget(s, 0, Qt::AlignHCenter);
        ev->addStretch(1);

        insertBeforeStretch(empty_, 0);
    }

    void setToolClick(ClickFn fn) { onToolClick_ = std::move(fn); }

    void addUser(const QString& text) {
        removeEmpty();
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 4, 0, 4);
        h->addStretch(1);
        auto* bubble = new QFrame;
        bubble->setObjectName("bubbleUser");
        auto* v = new QVBoxLayout(bubble);
        v->setContentsMargins(15, 10, 15, 11);
        auto* label = new QLabel(text);
        label->setObjectName("bubbleUserText");
        label->setWordWrap(true);
        label->setMaximumWidth(460);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(label);
        h->addWidget(bubble, 0, Qt::AlignTop);
        insertBeforeStretch(row);
        scrollBottomSoon();
    }

    // 返回可被流式追加文本的标签
    QLabel* addAssistantCard() {
        removeEmpty();
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 4, 0, 4);
        auto* card = new QFrame;
        card->setObjectName("cardAssistant");
        card->setMaximumWidth(680);
        auto* v = new QVBoxLayout(card);
        v->setContentsMargins(16, 12, 16, 14);
        v->setSpacing(6);
        auto* cap = new QLabel("AGENTFORGE");
        cap->setObjectName("roleCap");
        text_ = new QLabel;
        text_->setObjectName("assistantText");
        text_->setWordWrap(true);
        text_->setTextFormat(Qt::PlainText);
        text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(cap);
        v->addWidget(text_);
        h->addWidget(card, 0, Qt::AlignTop);
        h->addStretch(1);
        insertBeforeStretch(row);
        scrollBottomSoon();
        return text_;
    }

    void setAssistantText(QLabel* label, const QString& full) {
        if (!label) return;
        label->setText(full);
        scrollBottomSoon();
    }

    ToolCard* addToolCard(const QString& name, const QString& args) {
        removeEmpty();
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 2, 0, 2);
        auto* card = new ToolCard(name, args, [this](const QString& n, const QString& a,
                                                     const QString& r, bool e) {
            if (onToolClick_) onToolClick_(n, a, r, e);
        });
        card->setMaximumWidth(680);
        h->addWidget(card, 0, Qt::AlignTop);
        h->addStretch(1);
        insertBeforeStretch(row);
        scrollBottomSoon();
        return card;
    }

    void addNotice(const QString& text) {
        removeEmpty();
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 2, 0, 2);
        auto* label = new QLabel(text);
        label->setObjectName("notice");
        label->setWordWrap(true);
        h->addWidget(label);
        insertBeforeStretch(row);
        scrollBottomSoon();
    }

    void addError(const QString& text) {
        removeEmpty();
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 2, 0, 2);
        auto* pill = new QLabel(QString::fromUtf8("⚠ ") + text);
        pill->setObjectName("errorPill");
        pill->setWordWrap(true);
        h->addWidget(pill);
        h->addStretch(1);
        insertBeforeStretch(row);
        scrollBottomSoon();
    }

    void addDone(const QString& text) {
        removeEmpty();
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 6, 0, 6);
        auto* pill = new QLabel(text);
        pill->setObjectName("donePill");
        pill->setWordWrap(true);
        h->addWidget(pill);
        h->addStretch(1);
        insertBeforeStretch(row);
        scrollBottomSoon();
    }

    void clearAll() {
        QLayoutItem* item = nullptr;
        while ((item = box_->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        box_->addStretch(1);
        empty_ = nullptr;   // 允许重新出现空态
        empty_ = new QWidget(this);
        auto* ev = new QVBoxLayout(empty_);
        ev->setSpacing(8);
        auto* h = new QLabel("向 AgentForge 提出第一个任务");
        h->setObjectName("emptyHint");
        auto* s = new QLabel("例如：读一下 mock_target.txt 里的函数，或用 /help 查看命令");
        s->setObjectName("emptySub");
        ev->addStretch(1);
        ev->addWidget(h, 0, Qt::AlignHCenter);
        ev->addWidget(s, 0, Qt::AlignHCenter);
        ev->addStretch(1);
        insertBeforeStretch(empty_, 0);
    }

private:
    void insertBeforeStretch(QWidget* w, int /*pos*/ = -1) {
        // 始终插到末尾 stretch 之前
        box_->insertWidget(box_->count() - 1, w, 0, Qt::AlignTop);
    }

    void removeEmpty() {
        if (!empty_) return;
        empty_->hide();
        empty_->deleteLater();
        empty_ = nullptr;
    }

    void scrollBottomSoon() {
        QTimer::singleShot(0, this, [this] {
            auto* bar = verticalScrollBar();
            bar->setValue(bar->maximum());
        });
    }

    QWidget*     inner_ = nullptr;
    QVBoxLayout* box_ = nullptr;
    QWidget*     empty_ = nullptr;
    QLabel*      text_ = nullptr;
    ClickFn      onToolClick_;
};

// ============================================================================
//  SidePanel —— 左侧边栏
// ============================================================================
class SidePanel : public QWidget {
public:
    explicit SidePanel(std::function<void(const QString&)> onCommand, QWidget* parent = nullptr)
        : QWidget(parent), onCommand_(std::move(onCommand)) {
        setObjectName("side");
        setFixedWidth(236);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(18, 20, 18, 16);
        v->setSpacing(6);

        auto* brand = new QLabel("AgentForge");
        brand->setObjectName("brand");
        auto* tag = new QLabel("Coder 类 AI Agent Harness");
        tag->setObjectName("tagline");
        v->addWidget(brand);
        v->addWidget(tag);
        v->addSpacing(18);

        addSection(v, "快捷");
        addItem(v, "沙箱自检", "/sandbox");
        addItem(v, "仓库索引", "/index");
        addItem(v, "会话库", "/sessions");
        addItem(v, "上下文预算", "/context");
        addItem(v, "帮助", "/help");

        v->addSpacing(14);
        addSection(v, "会话");
        sessions_ = new QVBoxLayout();
        sessions_->setSpacing(2);
        v->addLayout(sessions_);
        addItem(v, "查看全部", "/sessions");

        v->addStretch(1);

        // ---- 状态卡 ----
        auto* card = new QFrame;
        card->setObjectName("metaCard");
        auto* cv = new QVBoxLayout(card);
        cv->setContentsMargins(12, 10, 12, 10);
        cv->setSpacing(6);
        modelKey_   = makeMeta(cv, "模型");
        sandboxKey_ = makeMeta(cv, "沙箱");
        sessionKey_ = makeMeta(cv, "会话");
        v->addWidget(card);
        v->addSpacing(6);
        auto* foot = new QLabel("MockLLM 演示 · 零 API 费用");
        foot->setObjectName("tagline");
        v->addWidget(foot);
    }

    void setMeta(const QString& model, const QString& sandbox, const QString& session) {
        modelKey_->setText(elide(model));
        sandboxKey_->setText(elide(sandbox));
        sessionKey_->setText(elide(session));
    }

    void setSessions(const std::vector<std::pair<std::string, std::string>>& rows) {
        QLayoutItem* it = nullptr;
        while ((it = sessions_->takeAt(0)) != nullptr) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        int n = 0;
        for (const auto& [id, ts] : rows) {
            if (n++ >= 4) break;
            auto* b = new QPushButton(QString::fromStdString(id));
            b->setObjectName("sideItem");
            b->setToolTip(QString::fromStdString(ts));
            connect(b, &QPushButton::clicked, this, [this, id] {
                onCommand_("/sessions");
                (void)id;
            });
            sessions_->addWidget(b);
        }
        if (rows.empty()) {
            auto* l = new QLabel("暂无会话");
            l->setObjectName("tagline");
            sessions_->addWidget(l);
        }
    }

private:
    static QString elide(const QString& s, int max = 18) {
        return s.size() > max ? s.left(max - 1) + "…" : s;
    }

    void addSection(QVBoxLayout* v, const char* text) {
        auto* l = new QLabel(QString::fromUtf8(text));
        l->setObjectName("sideSection");
        v->addWidget(l);
        v->addSpacing(2);
    }

    void addItem(QVBoxLayout* v, const char* text, const char* cmd) {
        auto* b = new QPushButton(QString::fromUtf8(text));
        b->setObjectName("sideItem");
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, cmd] { onCommand_(cmd); });
        v->addWidget(b);
    }

    QLabel* makeMeta(QVBoxLayout* v, const char* key) {
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        auto* k = new QLabel(QString::fromUtf8(key));
        k->setObjectName("metaKey");
        auto* val = new QLabel("—");
        val->setObjectName("metaVal");
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(k);
        row->addStretch(1);
        row->addWidget(val);
        v->addLayout(row);
        return val;
    }

    std::function<void(const QString&)> onCommand_;
    QLabel* modelKey_ = nullptr;
    QLabel* sandboxKey_ = nullptr;
    QLabel* sessionKey_ = nullptr;
    QVBoxLayout* sessions_ = nullptr;
};

// ============================================================================
//  DetailPanel —— 右侧工具/变更详情（monospace）
// ============================================================================
class DetailPanel : public QFrame {
public:
    explicit DetailPanel(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName("detailCard");
        setFixedWidth(320);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(14, 12, 14, 12);
        v->setSpacing(8);

        title_ = new QLabel("工具详情");
        title_->setObjectName("detailTitle");
        sub_ = new QLabel("点击左侧工具卡片查看参数与完整结果");
        sub_->setObjectName("detailSub");
        sub_->setWordWrap(true);
        v->addWidget(title_);
        v->addWidget(sub_);

        body_ = new QPlainTextEdit(this);
        body_->setObjectName("detail");
        body_->setReadOnly(true);
        QFont mono(kFontMono);
        mono.setStyleHint(QFont::Monospace);
        mono.setPixelSize(11);
        body_->setFont(mono);
        body_->setPlaceholderText("（暂无内容）");
        v->addWidget(body_, 1);
    }

    void showTool(const QString& name, const QString& args, const QString& result, bool error) {
        title_->setText(QString::fromUtf8("工具 · ") + name);
        sub_->setText(error ? "失败 —— 已作为 observation 回喂模型"
                            : "完成 —— 结果已回喂模型");
        body_->setPlainText(
            QString::fromUtf8("参数\n────\n") + pretty(args) +
            QString::fromUtf8("\n\n结果\n────\n") + (result.isEmpty() ? "（运行中…）" : result));
    }

    void showText(const QString& title, const QString& sub, const QString& text) {
        title_->setText(title);
        sub_->setText(sub);
        body_->setPlainText(text);
    }

private:
    static QString pretty(const QString& s) {
        // 简单格式化 JSON；失败则原样返回
        auto parsed = nlohmann::json::parse(s.toStdString(), nullptr, false);
        if (parsed.is_discarded()) return s;
        return QString::fromStdString(parsed.dump(2));
    }

    QLabel* title_ = nullptr;
    QLabel* sub_ = nullptr;
    QPlainTextEdit* body_ = nullptr;
};

// ============================================================================
//  MainWindow
// ============================================================================
namespace {

struct EventPayload {
    enum class Kind { Text, ToolStart, ToolDone, Notice, Error, Done };
    Kind kind = Kind::Text;
    std::string name;
    std::string content;
    std::string extra;
    bool error = false;
};

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("AgentForge —— Coder 类 AI Agent Harness");
    resize(1180, 800);
    buildUi();

    Logger::instance().setFile("agentforge_gui.log");
    Logger::instance().info("gui", "start", json::object());

    // 初始元信息
    try {
        const af::AppConfig cfg = af::loadConfig(".");
        QString model = QString::fromStdString(cfg.provider.model);
        if (model.isEmpty()) model = "未配置";
        side_->setMeta(model, "Job Object", QString::fromStdString(cfg.sessionId));

        af::SessionStore store;
        std::error_code ec;
        std::filesystem::create_directories(".af-sessions", ec);
        if (store.open(".af-sessions/sessions.db"))
            side_->setSessions(store.listSessions());
    } catch (...) {
    }
}

MainWindow::~MainWindow() {
    closing_ = true;
    stopSource_.request_stop();
    if (worker_.joinable()) worker_.join();
}

// ============================================================================
//  UI 组装
// ============================================================================
void MainWindow::buildUi() {
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
        app->setStyleSheet(styleSheet());

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* rowWidget = new QWidget(central);
    auto* row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    side_ = new SidePanel([this](const QString& cmd) { handleCommand(cmd); }, rowWidget);

    // ---- 中间列：对话 + 快捷 chips + 输入条 ----
    auto* mid = new QWidget(rowWidget);
    auto* mv = new QVBoxLayout(mid);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(0);

    chat_ = new ChatView(mid);
    chat_->setToolClick([this](const QString& n, const QString& a, const QString& r, bool e) {
        detail_->showTool(n, a, r, e);
    });

    auto* chips = new QWidget(mid);
    auto* ch = new QHBoxLayout(chips);
    ch->setContentsMargins(22, 2, 22, 0);
    ch->setSpacing(6);
    auto addChip = [&](const char* text, const char* cmd) {
        auto* b = new QPushButton(QString::fromUtf8(text));
        b->setObjectName("chipBtn");
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, cmd] { handleCommand(cmd); });
        ch->addWidget(b);
    };
    addChip("沙箱", "/sandbox");
    addChip("索引", "/index");
    addChip("会话", "/sessions");
    addChip("上下文", "/context");
    addChip("帮助", "/help");
    addChip("清空", "/clear");
    ch->addStretch(1);

    auto* composerWrap = new QWidget(mid);
    auto* cw = new QHBoxLayout(composerWrap);
    cw->setContentsMargins(22, 8, 22, 14);
    cw->setSpacing(8);

    auto* composer = new QFrame(composerWrap);
    composer->setObjectName("composer");
    auto* cv = new QHBoxLayout(composer);
    cv->setContentsMargins(16, 6, 8, 6);
    cv->setSpacing(8);

    input_ = new QLineEdit(composer);
    input_->setObjectName("input");
    input_->setPlaceholderText("向 AgentForge 提问，或输入 /help 查看命令…");
    input_->setClearButtonEnabled(false);

    sendBtn_ = new QPushButton("发送", composer);
    sendBtn_->setObjectName("sendBtn");
    sendBtn_->setCursor(Qt::PointingHandCursor);

    stopBtn_ = new QPushButton("停止", composer);
    stopBtn_->setObjectName("stopBtn");
    stopBtn_->setCursor(Qt::PointingHandCursor);
    stopBtn_->setVisible(false);

    cv->addWidget(input_, 1);
    cv->addWidget(stopBtn_);
    cv->addWidget(sendBtn_);
    cw->addWidget(composer, 1);
    mv->addWidget(chat_, 1);
    mv->addWidget(chips);
    mv->addWidget(composerWrap);

    detail_ = new DetailPanel(rowWidget);

    row->addWidget(side_);
    row->addWidget(mid, 1);
    row->addWidget(detail_);

    // ---- 底部状态条 ----
    auto* statusBarRow = new QWidget(central);
    statusBarRow->setFixedHeight(30);
    auto* sb = new QHBoxLayout(statusBarRow);
    sb->setContentsMargins(24, 0, 24, 8);
    sb->setSpacing(8);
    status_ = new QLabel("就绪");
    status_->setObjectName("statusLabel");
    sb->addWidget(status_);
    sb->addStretch(1);
    tokenChip_ = new QLabel("tokens —");
    tokenChip_->setObjectName("statusChip");
    modelChip_ = new QLabel("mock-1");
    modelChip_->setObjectName("statusChip");
    sandboxChip_ = new QLabel("沙箱 · Job Object");
    sandboxChip_->setObjectName("statusChip");
    sessionChip_ = new QLabel("会话 default");
    sessionChip_->setObjectName("statusChip");
    sb->addWidget(modelChip_);
    sb->addWidget(sandboxChip_);
    sb->addWidget(sessionChip_);
    sb->addWidget(tokenChip_);

    root->addWidget(rowWidget, 1);
    root->addWidget(statusBarRow);
    setCentralWidget(central);

    // ---- 事件 ----
    connect(sendBtn_, &QPushButton::clicked, this, [this] { submitInput(); });
    connect(input_, &QLineEdit::returnPressed, this, [this] { submitInput(); });
    connect(stopBtn_, &QPushButton::clicked, this, [this] { stopRun(); });

    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this] {
        if (busy_) stopRun();
    });

    input_->setFocus();
}

void MainWindow::submitInput() {
    const QString t = input_->text().trimmed();
    if (t.isEmpty()) return;
    input_->clear();
    if (t.startsWith('/')) handleCommand(t);
    else                  startRun(t);
}

void MainWindow::setBusy(bool busy) {
    busy_ = busy;
    sendBtn_->setEnabled(!busy);
    stopBtn_->setVisible(busy);
    input_->setEnabled(!busy);
}

void MainWindow::setStatus(const QString& text) {
    status_->setText(text);
}

// ============================================================================
//  事件投递（worker → UI 线程）
// ============================================================================
void MainWindow::post(std::function<void()> fn) {
    QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
}

bool MainWindow::askConfirm(const std::string& prompt) {
    std::promise<bool> p;
    auto fut = p.get_future();
    QMetaObject::invokeMethod(this,
        [this, &prompt, &p]() {
            const auto btn = QMessageBox::question(
                this, "权限确认", QString::fromStdString(prompt),
                QMessageBox::Yes | QMessageBox::No);
            p.set_value(btn == QMessageBox::Yes);
        },
        Qt::QueuedConnection);
    return fut.get();
}

// ============================================================================
//  命令面板
// ============================================================================
void MainWindow::handleCommand(const QString& line) {
    const QString t = line.trimmed();
    if (t == "/help" || t == "/?") {
        detail_->showText(
            QString::fromUtf8("命令"),
            "以 / 开头直接运行；其余内容作为任务发送",
            QString::fromUtf8(
                "/help        显示这份帮助\n"
                "/sandbox     沙箱能力自检\n"
                "/index [词]  仓库索引统计 / 检索\n"
                "/sessions    列出 SQLite 会话\n"
                "/context     上下文预算配置\n"
                "/clear       清空对话\n"));
        return;
    }
    if (t == "/clear") {
        chat_->clearAll();
        detail_->showText(QString::fromUtf8("工具详情"), "点击左侧工具卡片查看参数与完整结果", "");
        return;
    }
    if (t == "/sandbox") {
        af::SandboxConfig scfg;
        scfg.workspace = ".";
        auto sb = af::createSandbox(scfg);
        std::string report;
        sb->selfCheck(report);
        detail_->showText(QString::fromUtf8("沙箱自检"), "如实报告已强制/未强制能力",
                          QString::fromStdString(report));
        return;
    }
    if (t.startsWith("/index")) {
        af::RepoIndex idx;
        if (!idx.build(".")) {
            detail_->showText(QString::fromUtf8("仓库索引"), "当前目录没有可索引的源码", "");
            return;
        }
        const QString q = t.mid(6).trimmed();
        std::ostringstream os;
        if (q.isEmpty()) {
            os << idx.stats() << "\n\n";
            for (const auto& e : idx.dependencyEdges()) os << "  " << e << "\n";
        } else {
            for (const auto& c : idx.retrieve(q.toStdString(), 5, 40))
                os << "== " << c.path << ":" << c.startLine << "-" << c.endLine
                   << " score=" << c.score << " ==\n" << c.content << "\n";
        }
        detail_->showText(QString::fromUtf8("仓库索引"),
                          q.isEmpty() ? "统计与依赖边" : ("检索：" + q),
                          QString::fromStdString(os.str()));
        return;
    }
    if (t == "/sessions") {
        af::SessionStore store;
        std::ostringstream os;
        if (!store.open(".af-sessions/sessions.db")) {
            os << "无法打开会话库";
        } else {
            for (const auto& [id, ts] : store.listSessions()) os << id << "   " << ts << "\n";
        }
        detail_->showText(QString::fromUtf8("会话库"), "SQLite 持久化", QString::fromStdString(os.str()));
        return;
    }
    if (t == "/context") {
        af::BudgetConfig b;
        std::ostringstream os;
        os << "上下文预算\n窗口 " << b.contextWindow << " / 留答 " << b.reserveForReply
           << " / 输出 " << b.maxOutputTokens << " / 可用 " << b.available() << "\n\n";
        for (int i = 0; i <= 6; ++i) {
            const af::Layer l = static_cast<af::Layer>(i);
            os << "  L" << i << "  配额 " << b.forLayer(l) << "\n";
        }
        detail_->showText(QString::fromUtf8("上下文预算"), "ADR-5 分层配额",
                          QString::fromStdString(os.str()));
        return;
    }
    chat_->addNotice(QString::fromUtf8("未知命令：") + t + "（输入 /help 查看）");
}

// ============================================================================
//  运行 / 停止
// ============================================================================
void MainWindow::stopRun() {
    stopSource_.request_stop();
    setStatus("正在停止…");
}

void MainWindow::startRun(const QString& task) {
    if (busy_ && busy_.load()) {
        chat_->addNotice("上一次任务还在运行…");
        return;
    }
    setBusy(true);
    setStatus("运行中…");
    chat_->addUser(task);
    QLabel* streamLabel = chat_->addAssistantCard();

    const std::string taskStr = task.toStdString();
    stopSource_ = std::stop_source();
    const auto stopTok = stopSource_.get_token();

    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, taskStr, stopTok, streamLabel] {
        af::AppConfig cfg = af::loadConfig(".");

        auto post = [this](EventPayload::Kind kind, std::string name = {},
                           std::string content = {}, std::string extra = {},
                           bool err = false) {
            EventPayload p;
            p.kind    = kind;
            p.name    = std::move(name);
            p.content = std::move(content);
            p.extra   = std::move(extra);
            p.error   = err;
            QMetaObject::invokeMethod(this,
                [this, p]() {
                    switch (p.kind) {
                        case EventPayload::Kind::ToolStart: {
                            ToolCard* card = chat_->addToolCard(
                                QString::fromStdString(p.name),
                                QString::fromStdString(p.content));
                            openCards_.append(card);
                            detail_->showTool(QString::fromStdString(p.name),
                                              QString::fromStdString(p.content),
                                              QString(), false);
                            break;
                        }
                        case EventPayload::Kind::ToolDone: {
                            if (!openCards_.isEmpty()) {
                                ToolCard* card = openCards_.takeFirst();
                                card->finish(QString::fromStdString(p.extra), p.error);
                            }
                            break;
                        }
                        case EventPayload::Kind::Notice:
                            chat_->addNotice(QString::fromStdString(p.content));
                            break;
                        case EventPayload::Kind::Error:
                            chat_->addError(QString::fromStdString(p.content));
                            break;
                        case EventPayload::Kind::Done: {
                            chat_->addDone(QString::fromStdString(p.content));
                            setStatus(QString::fromStdString(p.extra));
                            setBusy(false);
                            break;
                        }
                        case EventPayload::Kind::Text:
                        default:
                            break;
                    }
                },
                Qt::QueuedConnection);
        };

        // ---- 核心对象（与 CLI 同一个组合根）----
        af::ToolRegistry registry;
        af::registerReadTool(registry);
        af::registerWriteTool(registry, cfg.workspace);
        af::registerEditTool(registry, cfg.workspace);
        af::SandboxConfig scfg = cfg.sandbox;
        scfg.workspace = cfg.workspace;
        auto sandbox = af::createSandbox(scfg);
        af::registerBashTool(registry, sandbox, cfg.workspace);

        {
            std::vector<af::PermissionRule> rules{
                {af::DecisionKind::Deny,  "Bash(rm -rf *)", "删除类命令一律拒绝"},
                {af::DecisionKind::Allow, "Read",           "只读操作直接放行"},
            };
            registry.setGate(std::make_shared<af::DefaultPermissionGate>(std::move(rules)));
        }

        af::ContextAssembler assembler(cfg.budget, nullptr);
        assembler.setSystem(kSystemPrompt);
        assembler.setTools(registry.defs());

        af::RepoIndex index;
        if (index.build(cfg.workspace)) assembler.setRepoMap(index.repoMap());

        auto provider = af::createProvider(cfg.provider);
        af::LoopLimits limits = cfg.limits;
        limits.stop = stopTok;
        af::AgentLoop loop(*provider, registry, assembler, limits);

        af::Turn turn;
        turn.input = taskStr;
        for (const auto& c : index.retrieve(taskStr, 3, 40)) {
            af::Message m;
            m.role = af::Role::User;
            m.content = "[检索命中] " + c.path + ":" + std::to_string(c.startLine) + "-" +
                        std::to_string(c.endLine) + "\n" + c.content;
            turn.retrieved.push_back(std::move(m));
        }

        af::SessionStore store;
        std::error_code ec;
        std::filesystem::create_directories(".af-sessions", ec);
        const bool sessionOk = store.open(".af-sessions/sessions.db");
        std::size_t historyStart = 0;
        if (sessionOk) {
            store.ensureSession(cfg.sessionId, taskStr.substr(0, 40));
            turn.history = store.loadHistory(cfg.sessionId);
            historyStart = turn.history.size();
            if (!turn.history.empty())
                post(EventPayload::Kind::Notice, "",
                     "已恢复会话 " + cfg.sessionId + " 历史 " +
                         std::to_string(historyStart) + " 条");
        }

        std::string acc;

        af::LoopHooks hooks;
        hooks.onChunk = [this, streamLabel, &acc](const af::ChatChunk& c) {
            if (const auto* d = std::get_if<af::TextDelta>(&c)) {
                if (d->text.empty()) return;
                acc += d->text;
                const QString full = QString::fromStdString(acc);
                QLabel* const lbl = streamLabel;
                QMetaObject::invokeMethod(this,
                    [this, lbl, full]() { chat_->setAssistantText(lbl, full); },
                    Qt::QueuedConnection);
            }
        };
        hooks.onToolStart = [post](const af::ToolCall& c) {
            post(EventPayload::Kind::ToolStart, c.name, c.args.dump(), "", false);
        };
        hooks.onToolDone = [post](const af::ToolResult& r) {
            std::string preview = r.content.substr(0, 300);
            if (r.content.size() > 300) preview += " ...";
            post(EventPayload::Kind::ToolDone, "", preview, r.content, r.isError);
        };
        hooks.onNotice = [post](const std::string& m) {
            post(EventPayload::Kind::Notice, "", m, "", false);
        };
        hooks.onError = [post](const std::string& e) {
            post(EventPayload::Kind::Error, "", e, "", true);
        };
        hooks.confirm = [this](const std::string& prompt) { return askConfirm(prompt); };

        const af::LoopOutcome outcome = loop.run(turn, hooks);

        if (!acc.empty()) {
            const QString full = QString::fromStdString(acc);
            QMetaObject::invokeMethod(this,
                [this, streamLabel, full]() { chat_->setAssistantText(streamLabel, full); },
                Qt::QueuedConnection);
        }

        if (sessionOk)
            store.appendMessages(cfg.sessionId, turn.history, historyStart);

        std::ostringstream msg;
        msg << "完成：" << std::string(af::toString(outcome)) << " · 计费 "
            << loop.spentTokens() << " tokens · 历史 " << turn.history.size() << " 条";
        if (!turn.finalText.empty()) msg << "\n" << turn.finalText;
        std::string shortState = std::string(af::toString(outcome));

        post(EventPayload::Kind::Done, "", msg.str(), shortState, false);
    });
}

void MainWindow::smokeDump() {
    setWindowTitle("AgentForge [smoke-ok]");
}

}  // namespace af::gui


