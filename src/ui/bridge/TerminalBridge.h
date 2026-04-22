#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMutex>

namespace DeepLux {

class TerminalWidget;
class ICommand;
class LogEntry;
class Project;
class AgentBridge;
struct RunResult;

/**
 * @brief 终端桥接层 - 连接 GUI 操作、CLIHandler、Logger 和 TerminalWidget
 *
 * 职责:
 * 1. 监听来自各组件的信号，格式化后发送到 TerminalWidget 显示
 * 2. 将用户在 TerminalWidget 输入的命令转发给 CLIHandler 执行
 * 3. 当 CLI 命令执行完成后，触发 GUI 相应更新
 * 4. 拦截 GUI 操作并转换为命令文本显示在终端
 */
class TerminalBridge : public QObject
{
    Q_OBJECT

public:
    static TerminalBridge& instance();

    // 初始化连接
    void initialize(TerminalWidget* terminal);
    void shutdown();

    // TerminalWidget 命令输入处理
    bool executeCommand(const QString& commandLine);

    // GUI 操作 -> 终端显示
    Q_INVOKABLE void onGuiAction(const QString& action, const QString& detail = QString());

    // CLI 命令执行（供 TerminalWidget 调用）
    void executeCliCommand(const QString& commandLine);

    // stdout/stderr 捕获接口
    void appendStdout(const QString& text);
    void appendStderr(const QString& text);

signals:
    // 发送命令到终端显示
    void commandOutput(const QString& text);
    void commandError(const QString& text);
    void commandSuccess(const QString& text);

    // GUI 同步信号（当 CLI 执行了会改变 GUI 状态的命令时）
    void projectOpened(const QString& projectPath);
    void projectCreated(const QString& projectName);
    void pluginLoaded(const QString& pluginName);
    void flowModuleAdded(const QString& moduleName, const QString& instanceName);

private slots:
    // Logger 信号处理
    void onLogAdded(const LogEntry& entry);

    // RunEngine 信号处理
    void onModuleStarted(const QString& moduleId);
    void onModuleFinished(const QString& moduleId, bool success);
    void onRunStarted();
    void onRunFinished(const RunResult& result);

    // ProjectManager 信号处理
    void onProjectCreated(Project* project);
    void onProjectOpened(Project* project);
    void onProjectSaved(Project* project);
    void onProjectClosed();

    // PluginManager 信号处理
    void onPluginLoaded(const QString& name);
    void onPluginUnloaded(const QString& name);

    // 命令执行结果处理
    void onCommandFinished(int exitCode);

    // 用户在 TerminalWidget 输入了命令
    void onCommandEntered(const QString& command);

private:
    TerminalBridge(QObject* parent = nullptr);
    ~TerminalBridge() override;

    // 禁用拷贝
    TerminalBridge(const TerminalBridge&) = delete;
    TerminalBridge& operator=(const TerminalBridge&) = delete;

    // 格式化输出
    QString formatTimestamp() const;
    QString formatCommand(const QString& cmd) const;

    // GUI 同步
    void syncProjectToGui(const QString& projectPath);

    TerminalWidget* m_terminal = nullptr;
    bool m_initialized = false;

    // 命令输出缓冲
    QString m_outputBuffer;
    QString m_errorBuffer;
    QMutex m_bufferMutex;
    QString m_lastCommand;
};

} // namespace DeepLux
