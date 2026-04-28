#include "BashProcess.h"

#include "PtyImpl.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <iostream>

// Linux 实现
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
#include "LinuxPtyImpl.cpp"
#endif

// Windows 实现
#if defined(Q_OS_WINDOWS)
#include "WindowsPtyImpl.cpp"
#endif

namespace DeepLux {

BashProcess::BashProcess(QObject* parent)
    : QObject(parent)
    , m_state(NotStarted)
    , m_lastError(None)
    , m_outputThrottle(new QTimer(this))
{
    m_outputThrottle->setSingleShot(true);
    m_outputThrottle->setInterval(50);  // 50ms 节流
    connect(m_outputThrottle, &QTimer::timeout, this, [this]() {
        if (!m_pendingOutput.isEmpty()) {
            emit outputReady(m_pendingOutput);
            m_pendingOutput.clear();
        }
    });

    // 延迟初始化
    QTimer::singleShot(0, this, [this]() {
        initialize();
    });
}

BashProcess::~BashProcess()
{
    if (m_impl) {
        m_impl->deleteLater();
    }
}

BashProcess& BashProcess::instance()
{
    static BashProcess instance;
    return instance;
}

bool BashProcess::initialize()
{
    if (m_state != NotStarted) return m_state == Running;

    m_state = Starting;

    // 检测可用的 shell
    m_shellPath = detectShell();
    if (m_shellPath.isEmpty()) {
        m_lastError = ShellNotFound;
        m_state = Failed;
        emit errorOccurred(ShellNotFound, "No suitable shell found");
        return false;
    }

    // 创建 PTY 实现
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    m_impl = new LinuxPtyImpl();
#elif defined(Q_OS_WINDOWS)
    m_impl = new WindowsConPtyImpl();
#else
    m_lastError = ShellNotFound;
    m_state = Failed;
    emit errorOccurred(ShellNotFound, "Platform not supported");
    return false;
#endif

    // 连接信号 — 直接传递原始字节，不做 UTF-8 转换
    // （UTF-8 解码由 AnsiParser 在 TerminalWidget 中统一处理）
    connect(m_impl, &PtyImpl::outputReady, this, [this](const QByteArray& data) {
        m_pendingOutput.append(data);
        if (!m_outputThrottle->isActive()) {
            m_outputThrottle->start(50);
        }
    });

    connect(m_impl, &PtyImpl::errorReady, this, [this](const QByteArray& data) {
        emit errorReady(data);
    });

    connect(m_impl, &PtyImpl::finished, this, [this](int exitCode) {
        emit commandFinished(exitCode);
    });

    connect(m_impl, &PtyImpl::errorOccurred, this, [this](const QString& error) {
        emit this->errorOccurred(ShellCrashed, error);
    });

    // 启动 shell
    if (!m_impl->start(m_shellPath, QStringList())) {
        m_state = Failed;
        return false;
    }

    m_state = Running;
    emit stateChanged(m_state);
    return true;
}

QString BashProcess::detectShell()
{
    QStringList candidates;
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    candidates << "/bin/bash" << "/usr/bin/bash" << "/bin/sh" << "/usr/bin/sh";
#elif defined(Q_OS_WINDOWS)
    candidates << "bash" << "cmd.exe" << "powershell.exe";
#endif

    for (const QString& shell : candidates) {
        if (QFileInfo(shell).exists() && QFileInfo(shell).isExecutable()) {
            return shell;
        }
    }
    return QString();
}

void BashProcess::writeCommand(const QString& command)
{
    if (m_state != Running || !m_impl) return;

    addToHistory(command);
    m_impl->write(command.toUtf8() + "\n");
}

void BashProcess::writeRaw(const QString& data)
{
    if (m_state != Running || !m_impl) return;
    m_impl->write(data.toUtf8());
}

QStringList BashProcess::history() const
{
    QReadLocker locker(&m_historyLock);
    return m_commandHistory;
}

void BashProcess::addToHistory(const QString& cmd)
{
    QWriteLocker locker(&m_historyLock);
    if (!cmd.isEmpty() && (m_commandHistory.isEmpty() || m_commandHistory.last() != cmd)) {
        m_commandHistory.append(cmd);
    }
}

void BashProcess::clearHistory()
{
    QWriteLocker locker(&m_historyLock);
    m_commandHistory.clear();
}

void BashProcess::resize(int cols, int rows)
{
    if (m_impl) {
        m_impl->resize(cols, rows);
    }
}

QString BashProcess::findAvailableShell()
{
    BashProcess tmp;
    return tmp.detectShell();
}

bool BashProcess::isShellAvailable(const QString& shellPath)
{
    return QFileInfo(shellPath).exists() && QFileInfo(shellPath).isExecutable();
}

QString BashProcess::cliWrapperPath()
{
    return QDir::homePath() + "/.deeplux/bash_rc";
}

bool BashProcess::createCliWrapper()
{
    QString wrapperPath = cliWrapperPath();
    QDir dir(QFileInfo(wrapperPath).dir());
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    QString content = R"(# DeepLux CLI wrapper - auto-generated
# Do not edit manually, it will be overwritten on next DeepLux startup

# Source system bashrc
[[ -f /etc/bash.bashrc ]] && source /etc/bash.bashrc

# Source user config files (if not already sourced)
[[ -f ~/.bashrc ]] && source ~/.bashrc
[[ -f ~/.bash_profile ]] && source ~/.bash_profile
[[ -f ~/.profile ]] && source ~/.profile

# DeepLux CLI command wrapper
# Usage: deeplux <command> [args...]
# Examples:
#   deeplux run
#   deeplux create-project MyProject
#   deeplux help
#   deeplux list-plugins
#
# Note: CLI commands are forwarded to DeepLux's internal CLIHandler.
deeplux() {
    local cmd="$*"
    # Send command via OSC 888 escape sequence
    printf '\e]888;deeplux;%s\a' "$cmd"
}

# Optional alias for convenience
alias dl='deeplux'
)";

    QFile file(wrapperPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(content.toUtf8());
        file.close();
        return true;
    }
    return false;
}

} // namespace DeepLux
