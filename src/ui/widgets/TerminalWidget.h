#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QCompleter>
#include <QStringListModel>
#include <QLabel>
#include <QString>
#include <QScrollBar>

namespace DeepLux {

class BashProcess;
class TerminalScreen;
class TerminalRenderer;
class AnsiParser;

/**
 * @brief 终端组件 - 基于字符网格的真实 bash 终端
 *
 * 功能特性:
 * - 基于真实 PTY 的 bash 终端（字符网格渲染）
 * - 完整 ANSI 序列支持（光标控制、颜色、清屏等）
 * - 鼠标选中复制
 * - stdout/stderr 实时输出
 * - 与 GUI 操作双向同步
 */
class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget* parent = nullptr);
    ~TerminalWidget() override;

    // 输出接口（供外部组件调用，直接写入终端显示）
    void printCommand(const QString& cmd);
    void printOutput(const QString& text);
    void printError(const QString& text);
    void printInfo(const QString& text);
    void printSuccess(const QString& text);
    void printWarning(const QString& text);
    void printRaw(const QString& text);

    // 清除终端
    Q_INVOKABLE void clear();

    // 可用命令列表（用于自动补全）
    void setAvailableCommands(const QStringList& commands);

    // 连接到 BashProcess
    void connectToBashProcess(BashProcess* bashProcess);

    // 更新主题颜色
    void setThemeColors(const QColor& fg, const QColor& bg, const QColor& selectionBg);

signals:
    void commandEntered(const QString& command);

public slots:
    void onLogMessage(const QString& message, int level);
    void onCommandExecuted(const QString& command, int exitCode);
    void onModuleStarted(const QString& moduleName);
    void onModuleFinished(const QString& moduleName, bool success);
    void onBashOutput(const QByteArray& data);
    void onBashError(const QByteArray& data);

private slots:
    void onRendererSizeChanged(int cols, int rows);
    void onScreenUpdated();
    void onScrollBarValueChanged(int value);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    void setupUI();
    void sendKeyToPty(QKeyEvent* event);
    void sendTextToPty(const QString& text);

    TerminalScreen* m_screen = nullptr;
    TerminalRenderer* m_renderer = nullptr;
    AnsiParser* m_parser = nullptr;
    QScrollBar* m_scrollBar = nullptr;
    BashProcess* m_bashProcess = nullptr;

    QStringList m_availableCommands;
    QStringListModel* m_completerModel = nullptr;
    QCompleter* m_completer = nullptr;

    bool m_isDarkTheme = true;
};

} // namespace DeepLux
