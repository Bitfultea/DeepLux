#include "WindowsPtyImpl.h"

#if defined(Q_OS_WINDOWS)

#include "BashProcess.h"

#include <QCoreApplication>
#include <QDebug>
#include <qt_windows.h>
#include <QThread>
#include <QTimer>

#include <conio.h>
#include <stdio.h>
#include <stdlib.h>

namespace DeepLux {

WindowsConPtyImpl::WindowsConPtyImpl()
    : m_masterHandle(INVALID_HANDLE_VALUE)
    , m_slaveHandle(INVALID_HANDLE_VALUE)
    , m_processHandle(INVALID_HANDLE_VALUE)
    , m_processId(0)
    , m_notifier(nullptr)
    , m_readTimer(nullptr)
    , m_isRunning(false)
{
}

WindowsConPtyImpl::~WindowsConPtyImpl()
{
    kill();
}

bool WindowsConPtyImpl::start(const QString& shell, const QStringList& args)
{
    Q_UNUSED(args);

    // Windows ConPTY requires Win10 1809+
    OSVERSIONINFOW osvi;
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExW(&osvi);

    if (osvi.dwMajorVersion < 10 || (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber < 17763)) {
        emit errorOccurred("Windows ConPTY requires Windows 10 1809 or later");
        return false;
    }

    if (!createConPTY(shell, args)) {
        return false;
    }

    m_isRunning = true;
    return true;
}

bool WindowsConPtyImpl::createConPTY(const QString& shell, const QStringList& args)
{
    // Create Pseudo Console
    HPCON hPty = INVALID_HANDLE_VALUE;
    SIZE_T size = 0;

    // Create the ConPTY
    if (!CreatePseudoConsole(CENTER, 80, 24, &hPty)) {
        emit errorOccurred("Failed to create Pseudo Console: " + QString::number(GetLastError()));
        return false;
    }

    // For Windows, we use a simplified QProcess approach as ConPTY requires complex setup
    // This is a fallback implementation using QProcess + pipes

    QProcess* process = new QProcess();
    process->setProgram(shell);
    if (!args.isEmpty()) {
        process->setArguments(args.mid(1));
    }
    process->setEnvironment(QProcess::systemEnvironment());
    process->start();

    if (!process->waitForStarted()) {
        emit errorOccurred("Failed to start shell: " + process->errorString());
        delete process;
        ClosePseudoConsole(hPty);
        return false;
    }

    m_processHandle = process->pid()->hProcess;
    m_processId = process->pid()->dwProcessId;

    // 连接输出信号
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        QByteArray data = process->readAllStandardOutput();
        if (!data.isEmpty()) {
            emit outputReady(data);
        }
    });

    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        QByteArray data = process->readAllStandardError();
        if (!data.isEmpty()) {
            emit errorReady(data);
        }
    });

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &WindowsConPtyImpl::onProcessFinished);

    m_readTimer = new QTimer(this);
    m_readTimer->setInterval(50);
    connect(m_readTimer, &QTimer::timeout, this, &WindowsConPtyImpl::onReadyRead);
    m_readTimer->start();

    return true;
}

void WindowsConPtyImpl::write(const QByteArray& data)
{
    if (!m_isRunning) return;
    // 对于 QProcess 实现，直接使用 write
    // 注意：这需要特殊处理
    emit errorOccurred("Windows ConPTY write not fully implemented - use bash wrapper");
}

QByteArray WindowsConPtyImpl::read()
{
    QByteArray buffer;
    // Windows 实现返回空，使用信号式读取
    return buffer;
}

void WindowsConPtyImpl::resize(int cols, int rows)
{
    if (!m_isRunning) return;

    // ConPTY resize
    // SetConsoleScreenBufferSize doesn't directly resize the ConPTY
    // This would require recreating the ConPTY or using a different API
    Q_UNUSED(cols);
    Q_UNUSED(rows);
}

void WindowsConPtyImpl::kill()
{
    m_isRunning = false;

    if (m_readTimer) {
        m_readTimer->stop();
        m_readTimer->deleteLater();
        m_readTimer = nullptr;
    }

    if (m_notifier) {
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }

    if (m_processHandle != INVALID_HANDLE_VALUE) {
        TerminateProcess(m_processHandle, 0);
        CloseHandle(m_processHandle);
        m_processHandle = INVALID_HANDLE_VALUE;
    }

    if (m_masterHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_masterHandle);
        m_masterHandle = INVALID_HANDLE_VALUE;
    }

    if (m_slaveHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_slaveHandle);
        m_slaveHandle = INVALID_HANDLE_VALUE;
    }
}

bool WindowsConPtyImpl::isRunning() const
{
    return m_isRunning && m_processHandle != INVALID_HANDLE_VALUE;
}

void WindowsConPtyImpl::onReadyRead()
{
    // 使用 QTimer 轮询读取
}

void WindowsConPtyImpl::onProcessFinished()
{
    m_isRunning = false;
    emit finished(0);
}

} // namespace DeepLux

#endif // Q_OS_WINDOWS
