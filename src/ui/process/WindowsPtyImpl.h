#ifndef DEEPLUX_WINDOWS_PTY_IMPL_H
#define DEEPLUX_WINDOWS_PTY_IMPL_H

#include "../PtyImpl.h"

#if defined(Q_OS_WINDOWS)

#include <QString>
#include <QTimer>

// Windows API
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace DeepLux {

/**
 * @brief Windows ConPTY 实现 (Win10 1809+)
 *
 * 使用 Windows Pseudo Console API 提供真实的终端体验：
 * - CreatePseudoConsole / ResizePseudoConsole / ClosePseudoConsole
 * - 通过命名管道读写数据
 * - 使用 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 启动 shell 进程
 */
class WindowsConPtyImpl : public PtyImpl
{
    Q_OBJECT

public:
    WindowsConPtyImpl();
    ~WindowsConPtyImpl() override;

    bool start(const QString& shell, const QStringList& args) override;
    void write(const QByteArray& data) override;
    QByteArray read() override;
    void resize(int cols, int rows) override;
    void kill() override;
    bool isRunning() const override;

private slots:
    void onReadyRead();

private:
    bool setupConPty(const QString& shell, const QStringList& args);
    void cleanup();

    HPCON m_hPC = INVALID_HANDLE_VALUE;
    HANDLE m_hPipeInWrite = INVALID_HANDLE_VALUE;   // 写入 ConPTY 输入
    HANDLE m_hPipeOutRead = INVALID_HANDLE_VALUE;   // 读取 ConPTY 输出
    HANDLE m_processHandle = INVALID_HANDLE_VALUE;
    DWORD m_processId = 0;

    QTimer* m_readTimer = nullptr;
    bool m_isRunning = false;
};

} // namespace DeepLux

#endif // Q_OS_WINDOWS

#endif // DEEPLUX_WINDOWS_PTY_IMPL_H
