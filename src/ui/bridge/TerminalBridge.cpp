#include "TerminalBridge.h"

#include "widgets/TerminalWidget.h"
#include "process/BashProcess.h"
#include "core/common/Logger.h"
#include "core/engine/RunEngine.h"
#include "core/manager/ProjectManager.h"
#include "core/manager/PluginManager.h"
#include "core/model/Project.h"
#include "core/common/CLIHandler.h"

#include <QTimer>
#include <QCoreApplication>
#include <QDebug>

namespace DeepLux {

TerminalBridge::TerminalBridge(QObject* parent)
    : QObject(parent)
{
}

TerminalBridge::~TerminalBridge() = default;

TerminalBridge& TerminalBridge::instance()
{
    static TerminalBridge instance;
    return instance;
}

void TerminalBridge::initialize(TerminalWidget* terminal)
{
    if (m_initialized) return;
    m_terminal = terminal;

    // 连接 TerminalWidget 命令输入
    connect(this, &TerminalBridge::commandOutput, terminal, &TerminalWidget::printOutput);
    connect(this, &TerminalBridge::commandError, terminal, &TerminalWidget::printError);
    connect(this, &TerminalBridge::commandSuccess, terminal, &TerminalWidget::printSuccess);

    // 连接 Logger 信号
    connect(&Logger::instance(), &Logger::logAdded, this, &TerminalBridge::onLogAdded);

    // 连接 RunEngine 信号
    connect(&RunEngine::instance(), &RunEngine::moduleStarted, this, &TerminalBridge::onModuleStarted);
    connect(&RunEngine::instance(), &RunEngine::moduleFinished, this, &TerminalBridge::onModuleFinished);
    connect(&RunEngine::instance(), &RunEngine::runStarted, this, &TerminalBridge::onRunStarted);
    connect(&RunEngine::instance(), &RunEngine::runFinished, this, &TerminalBridge::onRunFinished);

    // 连接 ProjectManager 信号
    connect(&ProjectManager::instance(), &ProjectManager::projectCreated, this, &TerminalBridge::onProjectCreated);
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, this, &TerminalBridge::onProjectOpened);
    connect(&ProjectManager::instance(), &ProjectManager::projectSaved, this, &TerminalBridge::onProjectSaved);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this, &TerminalBridge::onProjectClosed);

    // 连接 PluginManager 信号
    connect(&PluginManager::instance(), &PluginManager::pluginLoaded, this, &TerminalBridge::onPluginLoaded);
    connect(&PluginManager::instance(), &PluginManager::pluginUnloaded, this, &TerminalBridge::onPluginUnloaded);

    // 连接 TerminalWidget 命令信号 -> 写入 BashProcess
    connect(terminal, &TerminalWidget::commandEntered, this, &TerminalBridge::onCommandEntered);

    // 连接 BashProcess 输出到 TerminalWidget
    BashProcess& bash = BashProcess::instance();
    terminal->connectToBashProcess(&bash);

    connect(&bash, &BashProcess::outputReady, this, [terminal](const QString& text) {
        terminal->printRaw(text);
    }, Qt::QueuedConnection);

    connect(&bash, &BashProcess::errorReady, this, [terminal](const QString& text) {
        terminal->printError(text);
    }, Qt::QueuedConnection);

    // 设置可用命令列表
    QStringList commands = {
        "help", "version", "info", "list-plugins", "list-commands",
        "run", "create-project", "add-module", "connect",
        "save-project", "list-modules"
    };
    terminal->setAvailableCommands(commands);

    m_initialized = true;

    // 显示欢迎信息
    terminal->printInfo("==========================================");
    terminal->printInfo("       DeepLux Vision Terminal v1.0.0      ");
    terminal->printInfo("==========================================");
    terminal->printInfo("Type 'help' for available commands.");
    terminal->printInfo("");

    // 同步当前状态
    Project* currentProject = ProjectManager::instance().currentProject();
    if (currentProject) {
        terminal->printInfo(QString("当前项目: %1").arg(currentProject->name()));
    }
}

void TerminalBridge::shutdown()
{
    if (!m_initialized) return;

    // 断开所有连接
    disconnect(&Logger::instance(), nullptr, this, nullptr);
    disconnect(&RunEngine::instance(), nullptr, this, nullptr);
    disconnect(&ProjectManager::instance(), nullptr, this, nullptr);
    disconnect(&PluginManager::instance(), nullptr, this, nullptr);

    if (m_terminal) {
        disconnect(m_terminal, nullptr, this, nullptr);
    }

    m_initialized = false;
    m_terminal = nullptr;
}

bool TerminalBridge::executeCommand(const QString& commandLine)
{
    if (!m_terminal || commandLine.trimmed().isEmpty()) {
        return false;
    }

    QString trimmed = commandLine.trimmed();
    m_lastCommand = trimmed;

    // 解析命令和参数
    QStringList parts = trimmed.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;

    QString cmdName = parts.first();
    QStringList args = parts.mid(1);

    // 查找命令
    CLIHandler& cli = CLIHandler::instance();
    ICommand* cmd = cli.findCommand(cmdName);

    if (!cmd) {
        m_terminal->printError(QString("Unknown command: %1").arg(cmdName));
        m_terminal->printInfo("Type 'help' for available commands.");
        return false;
    }

    // 准备执行上下文
    CommandContext ctx;
    ctx.setGuiMode(true);
    ctx.setVerbose(true);

    if (!args.isEmpty()) {
        ctx.setProjectPath(args.first());
    }

    // 显示正在执行的命令
    m_terminal->printInfo(QString("Executing '%1'...").arg(cmdName));

    // 执行命令
    int result = cmd->execute(args, ctx);

    if (ctx.hasError()) {
        m_terminal->printError(ctx.errorString());
    } else if (result == 0) {
        m_terminal->printSuccess(QString("Command '%1' completed successfully").arg(cmd->name()));
    } else {
        m_terminal->printError(QString("Command '%1' failed with exit code %2").arg(cmd->name()).arg(result));
    }

    return true;
}

void TerminalBridge::onGuiAction(const QString& action, const QString& detail)
{
    if (!m_terminal) return;

    QString cmd;
    QString args;

    if (action == "create-project") {
        cmd = "create-project";
        args = detail.isEmpty() ? "unnamed" : detail;
    } else if (action == "open-project") {
        cmd = "open-project";
        args = detail;
    } else if (action == "save-project") {
        cmd = "save-project";
        args = detail;
    } else if (action == "add-module") {
        cmd = "add-module";
        args = detail;
    } else if (action == "run") {
        cmd = "run";
        args = detail;
    } else if (action == "stop") {
        cmd = "stop";
        args = detail;
    }

    if (!cmd.isEmpty()) {
        QString fullCmd = args.isEmpty() ? cmd : QString("%1 %2").arg(cmd).arg(args);
        m_terminal->printCommand(fullCmd);
    }
}

void TerminalBridge::executeCliCommand(const QString& commandLine)
{
    executeCommand(commandLine);
}

void TerminalBridge::appendStdout(const QString& text)
{
    QMutexLocker locker(&m_bufferMutex);
    m_outputBuffer.append(text);
}

void TerminalBridge::appendStderr(const QString& text)
{
    QMutexLocker locker(&m_bufferMutex);
    m_errorBuffer.append(text);
}

void TerminalBridge::onLogAdded(const LogEntry& entry)
{
    if (!m_terminal) return;

    QString message = entry.message;
    switch (entry.level) {
    case LogLevel::Debug:
        m_terminal->printOutput(QString("[%1] [Debug] [%2] %3")
                                    .arg(entry.timestamp.toString("hh:mm:ss"))
                                    .arg(entry.category)
                                    .arg(message));
        break;
    case LogLevel::Info:
        m_terminal->printInfo(QString("[%1] [Info] [%2] %3")
                                   .arg(entry.timestamp.toString("hh:mm:ss"))
                                   .arg(entry.category)
                                   .arg(message));
        break;
    case LogLevel::Warning:
        m_terminal->printWarning(QString("[%1] [Warning] [%2] %3")
                                     .arg(entry.timestamp.toString("hh:mm:ss"))
                                     .arg(entry.category)
                                     .arg(message));
        break;
    case LogLevel::Error:
        m_terminal->printError(QString("[%1] [Error] [%2] %3")
                                   .arg(entry.timestamp.toString("hh:mm:ss"))
                                   .arg(entry.category)
                                   .arg(message));
        break;
    case LogLevel::Success:
        m_terminal->printSuccess(QString("[%1] [Success] [%2] %3")
                                     .arg(entry.timestamp.toString("hh:mm:ss"))
                                     .arg(entry.category)
                                     .arg(message));
        break;
    }
}

void TerminalBridge::onModuleStarted(const QString& moduleId)
{
    if (!m_terminal) return;
    m_terminal->onModuleStarted(moduleId);
}

void TerminalBridge::onModuleFinished(const QString& moduleId, bool success)
{
    if (!m_terminal) return;
    m_terminal->onModuleFinished(moduleId, success);
}

void TerminalBridge::onRunStarted()
{
    if (!m_terminal) return;
    m_terminal->printInfo("========== 流程开始执行 ==========");
}

void TerminalBridge::onRunFinished(const RunResult& result)
{
    if (!m_terminal) return;
    if (result.success) {
        m_terminal->printSuccess(QString("========== 流程执行完成 (耗时: %1 ms) ==========")
                                      .arg(result.elapsedMs));
    } else {
        m_terminal->printError(QString("========== 流程执行失败: %1 ==========")
                                    .arg(result.errorMessage));
    }
}

void TerminalBridge::onProjectCreated(Project* project)
{
    if (!m_terminal) return;
    if (project) {
        m_terminal->printSuccess(QString("项目已创建: %1").arg(project->name()));
        emit projectCreated(project->name());
    }
}

void TerminalBridge::onProjectOpened(Project* project)
{
    if (!m_terminal) return;
    if (project) {
        m_terminal->printSuccess(QString("项目已打开: %1").arg(project->name()));
        emit projectOpened(project->filePath());
    }
}

void TerminalBridge::onProjectSaved(Project* project)
{
    if (!m_terminal) return;
    if (project) {
        m_terminal->printSuccess(QString("项目已保存: %1").arg(project->filePath()));
    }
}

void TerminalBridge::onProjectClosed()
{
    if (!m_terminal) return;
    m_terminal->printInfo("项目已关闭");
}

void TerminalBridge::onPluginLoaded(const QString& name)
{
    if (!m_terminal) return;
    m_terminal->printSuccess(QString("插件已加载: %1").arg(name));
}

void TerminalBridge::onPluginUnloaded(const QString& name)
{
    if (!m_terminal) return;
    m_terminal->printInfo(QString("插件已卸载: %1").arg(name));
}

void TerminalBridge::onCommandFinished(int exitCode)
{
    if (!m_terminal) return;

    if (exitCode == 0) {
        m_terminal->printSuccess("命令执行成功");
    } else {
        m_terminal->printError(QString("命令执行失败 (退出码: %1)").arg(exitCode));
    }
}

void TerminalBridge::onCommandEntered(const QString& command)
{
    // 将用户输入的命令写入 BashProcess
    BashProcess::instance().writeCommand(command);
}

QString TerminalBridge::formatTimestamp() const
{
    return QTime::currentTime().toString("hh:mm:ss");
}

QString TerminalBridge::formatCommand(const QString& cmd) const
{
    return QString("<span style='color: #27ae60;'>></span> <span style='color: #ffffff;'>%1</span>")
               .arg(cmd);
}

void TerminalBridge::syncProjectToGui(const QString& projectPath)
{
    // 当 CLI 打开项目时，同步到 GUI
    ProjectManager::instance().openProject(projectPath);
}

} // namespace DeepLux
