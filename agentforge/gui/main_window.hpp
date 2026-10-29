// ---------------------------------------------------------------------------
//  L11 · AgentForgeGUI —— Qt 6 图形界面（模块 ⑪）
//
//  设计语言：Apple HIG 风格的"精密工作台"（浅色、留白、系统字体、柔软圆角、
//  单一系统蓝强调色）。Operate 型界面：工具消失在任务里，信息层级清晰。
//
//  结构：左侧边栏（品牌 / 快捷命令 / 会话 / 状态）+ 中间对话流 + 右侧工具详情
//  线程：AgentLoop 在 worker 线程跑；UI 线程只处理投递回来的事件。
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QList>
#include <QMainWindow>
#include <QString>
#include <atomic>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>

class QLabel;
class QLineEdit;
class QPushButton;

namespace af::gui {

class ChatView;      // 自绘消息流（气泡/工具卡片，内部类）
class SidePanel;     // 左侧边栏
class DetailPanel;   // 右侧工具详情
class ToolCard;      // 工具调用卡片

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void smokeDump();

private:
    void buildUi();
    void submitInput();
    void startRun(const QString& task);
    void stopRun();
    bool askConfirm(const std::string& prompt);

    void post(std::function<void()> fn);

    void handleCommand(const QString& line);

    void setBusy(bool busy);
    void setStatus(const QString& text);

    // 消息流
    ChatView*    chat_ = nullptr;
    SidePanel*   side_ = nullptr;
    DetailPanel* detail_ = nullptr;

    QLineEdit*    input_ = nullptr;
    QPushButton*  sendBtn_ = nullptr;
    QPushButton*  stopBtn_ = nullptr;
    QLabel*       status_ = nullptr;
    QLabel*       modelChip_ = nullptr;
    QLabel*       sandboxChip_ = nullptr;
    QLabel*       sessionChip_ = nullptr;
    QLabel*       tokenChip_ = nullptr;

    QList<ToolCard*> openCards_;

    std::thread       worker_;
    std::stop_source  stopSource_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> closing_{false};
};

}  // namespace af::gui
