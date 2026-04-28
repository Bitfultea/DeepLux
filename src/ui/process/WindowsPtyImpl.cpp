#include "WindowsPtyImpl.h"

#if defined(Q_OS_WINDOWS)

#include "BashProcess.h"

#include <QTimer>
#include <QDebug>
#include <QFile>

namespace DeepLux {

WindowsConPtyImpl::WindowsConPtyImpl()
    : m_hPC(INVALID_HANDLE_VALUE)
    , m_hPipeInWrite(INVALID_HANDLE_VALUE)
    , m_hPipeOutRead(INVALID_HANDLE_VALUE)
    , m_processHandle(INVALID_HANDLE_VALUE)
    , m_processId(0)
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
    if (m_isRunning) {
        kill();
    }

    BashProcess::instance().createCliWrapper();

    if (!setupConPty(shell, args)) {
        return false;
    }

    m_isRunning = true;

    m_readTimer = new QTimer(this);
    m_readTimer->setInterval(10);
    connect(m_readTimer, &QTimer::timeout, this, &WindowsConPtyImpl::onReadyRead);
    m_readTimer->start();

    return true;
}

bool WindowsConPtyImpl::setupConPty(const QString& shell, const QStringList& args)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hPipeInRead = INVALID_HANDLE_VALUE;
    HANDLE hPipeInWrite = INVALID_HANDLE_VALUE;
    HANDLE hPipeOutRead = INVALID_HANDLE_VALUE;
    HANDLE hPipeOutWrite = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&hPipeInRead, &hPipeInWrite, &sa, 0)) {
        emit errorOccurred("Failed to create input pipe: " + QString::number(GetLastError()));
        return false;
    }

    if (!CreatePipe(&hPipeOutRead, &hPipeOutWrite, &sa, 0)) {
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        emit errorOccurred("Failed to create output pipe: " + QString::number(GetLastError()));
        return false;
    }

    COORD size;
    size.X = 80;
    size.Y = 24;

    HPCON hPC = INVALID_HANDLE_VALUE;
    HRESULT hr = CreatePseudoConsole(size, hPipeInRead, hPipeOutWrite, 0, &hPC);

    if (FAILED(hr)) {
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        CloseHandle(hPipeOutWrite);
        emit errorOccurred("Failed to create Pseudo Console: " + QString::number(hr, 16));
        return false;
    }

    STARTUPINFOEX si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEX);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);

    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));

    if (!si.lpAttributeList) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        CloseHandle(hPipeOutWrite);
        emit errorOccurred("Failed to allocate process attribute list");
        return false;
    }

    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize)) {
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        CloseHandle(hPipeOutWrite);
        emit errorOccurred("Failed to initialize process attribute list");
        return false;
    }

    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hPC, sizeof(HPCON), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        CloseHandle(hPipeOutWrite);
        emit errorOccurred("Failed to set Pseudo Console attribute");
        return false;
    }

    QString cmdLine = QString("\"%1\"").arg(shell);
    if (!args.isEmpty()) {
        for (const QString& arg : args) {
            cmdLine += " " + arg;
        }
    }

    QString wrapperPath = BashProcess::instance().cliWrapperPath();
    if (shell.contains("bash", Qt::CaseInsensitive) && QFile::exists(wrapperPath)) {
        cmdLine += QString(" --rcfile \"%1\" -i").arg(wrapperPath);
    }

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::wstring wideCmdLine = cmdLine.toStdWString();

    BOOL success = CreateProcessW(
        nullptr,
        wideCmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        nullptr,
        &si.StartupInfo,
        &pi
    );

    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);

    CloseHandle(hPipeInRead);
    CloseHandle(hPipeOutWrite);

    if (!success) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        emit errorOccurred("Failed to start shell process: " + QString::number(GetLastError()));
        return false;
    }

    CloseHandle(pi.hThread);

    m_hPC = hPC;
    m_hPipeInWrite = hPipeInWrite;
    m_hPipeOutRead = hPipeOutRead;
    m_processHandle = pi.hProcess;
    m_processId = pi.dwProcessId;

    return true;
}

void WindowsConPtyImpl::write(const QByteArray& data)
{
    if (!m_isRunning || m_hPipeInWrite == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(m_hPipeInWrite, data.constData(), static_cast<DWORD>(data.size()), &written, nullptr);
}

QByteArray WindowsConPtyImpl::read()
{
    QByteArray buffer;
    if (!m_isRunning || m_hPipeOutRead == INVALID_HANDLE_VALUE) {
        return buffer;
    }

    DWORD available = 0;
    if (!PeekNamedPipe(m_hPipeOutRead, nullptr, 0, nullptr, &available, nullptr)) {
        return buffer;
    }

    if (available == 0) {
        return buffer;
    }

    buffer.resize(static_cast<int>(available));
    DWORD bytesRead = 0;

    if (ReadFile(m_hPipeOutRead, buffer.data(), available, &bytesRead, nullptr)) {
        buffer.resize(static_cast<int>(bytesRead));
    } else {
        buffer.clear();
    }

    return buffer;
}

void WindowsConPtyImpl::resize(int cols, int rows)
{
    if (!m_isRunning || m_hPC == INVALID_HANDLE_VALUE) {
        return;
    }

    COORD size;
    size.X = static_cast<SHORT>(cols);
    size.Y = static_cast<SHORT>(rows);
    ResizePseudoConsole(m_hPC, size);
}

void WindowsConPtyImpl::kill()
{
    m_isRunning = false;

    if (m_readTimer) {
        m_readTimer->stop();
        delete m_readTimer;
        m_readTimer = nullptr;
    }

    if (m_processHandle != INVALID_HANDLE_VALUE) {
        TerminateProcess(m_processHandle, 0);
        WaitForSingleObject(m_processHandle, 5000);
        CloseHandle(m_processHandle);
        m_processHandle = INVALID_HANDLE_VALUE;
    }

    if (m_hPipeInWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipeInWrite);
        m_hPipeInWrite = INVALID_HANDLE_VALUE;
    }

    if (m_hPipeOutRead != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipeOutRead);
        m_hPipeOutRead = INVALID_HANDLE_VALUE;
    }

    if (m_hPC != INVALID_HANDLE_VALUE) {
        ClosePseudoConsole(m_hPC);
        m_hPC = INVALID_HANDLE_VALUE;
    }

    m_processId = 0;
}

bool WindowsConPtyImpl::isRunning() const
{
    if (!m_isRunning || m_processHandle == INVALID_HANDLE_VALUE) {
        return false;
    }
    return WaitForSingleObject(m_processHandle, 0) == WAIT_TIMEOUT;
}

void WindowsConPtyImpl::onReadyRead()
{
    QByteArray data = read();
    if (!data.isEmpty()) {
        emit outputReady(data);
    }

    if (!isRunning()) {
        m_isRunning = false;
        if (m_readTimer) {
            m_readTimer->stop();
        }
        emit finished(0);
    }
}

} // namespace DeepLux

#endif // Q_OS_WINDOWS
