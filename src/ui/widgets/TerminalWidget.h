#pragma once

#include <QWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QCompleter>
#include <QStringListModel>
#include <QLabel>
#include <QString>

namespace DeepLux {

class BashProcess;

/**
 * @brief 终端组件 - 嵌入 MainWindow 的 bash 终端
 *
 * 功能特性:
 * - 基于真实 PTY 的 bash 终端
 * - 命令历史支持（上下键导航）
 * - ANSI 颜色代码解析和显示
 * - 命令自动补全
 * - stdout/stderr 实时输出
 * - 与 GUI 操作双向同步
 */
class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget* parent = nullptr);
    ~TerminalWidget() override;

    // 输出接口
    void printCommand(const QString& cmd);           // 打印用户输入的命令
    void printOutput(const QString& text);           // 打印标准输出（白色）
    void printError(const QString& text);            // 打印错误输出（红色）
    void printInfo(const QString& text);             // 打印信息（蓝色）
    void printSuccess(const QString& text);           // 打印成功信息（绿色）
    void printWarning(const QString& text);           // 打印警告（黄色）
    void printRaw(const QString& text);              // 打印原始文本（无颜色）

    // 清除终端
    Q_INVOKABLE void clear();

    // 命令历史
    QStringList commandHistory() const { return m_commandHistory; }
    void setCommandHistory(const QStringList& history);

    // 可用命令列表（用于自动补全）
    void setAvailableCommands(const QStringList& commands);

    // 连接到 BashProcess
    void connectToBashProcess(BashProcess* bashProcess);

signals:
    void commandEntered(const QString& command);      // 用户输入了命令

public slots:
    void onLogMessage(const QString& message, int level);
    void onCommandExecuted(const QString& command, int exitCode);
    void onModuleStarted(const QString& moduleName);
    void onModuleFinished(const QString& moduleName, bool success);
    void onBashOutput(const QString& text);
    void onBashError(const QString& text);

private slots:
    void onReturnPressed();
    void onHistoryUp();
    void onHistoryDown();
    void onTextSelected();

private:
    void setupUI();
    void setupStyle();
    void executeCommand(const QString& command);
    void addToHistory(const QString& command);
    QString escapeHtml(const QString& text) const;
    QString ansiToHtml(const QString& text) const;
    void scrollToBottom();
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QTextEdit* m_outputArea = nullptr;      // 输出显示区域
    QLineEdit* m_inputLine = nullptr;       // 命令输入行
    QLabel* m_promptLabel = nullptr;        // 提示符标签

    QStringList m_commandHistory;            // 命令历史
    int m_historyIndex = -1;                 // 当前历史索引
    QString m_currentInput;                  // 当前输入（用于历史导航）

    QStringList m_availableCommands;         // 可用命令列表
    QStringListModel* m_completerModel = nullptr;
    QCompleter* m_completer = nullptr;

    bool m_isDarkTheme = true;
    int m_maxOutputLines = 10000;           // 最大输出行数限制
};

} // namespace DeepLux
