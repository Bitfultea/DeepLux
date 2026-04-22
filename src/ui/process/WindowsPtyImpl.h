#ifndef DEEPLUX_WINDOWS_PTY_IMPL_H
#define DEEPLUX_WINDOWS_PTY_IMPL_H

#include "../PtyImpl.h"

#if defined(Q_OS_WINDOWS)

#include <QString>
#include <QTimer>
#include <QSocketNotifier>

// Windows API
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace DeepLux {

/**
 * @brief Windows ConPTY 实现 (Win10 1809+)
 *
 * 使用 Windows Pseudo Console API
 */
class WindowsConPtyImpl : public PtyImpl
{
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
    void onProcessFinished();

private:
    bool createConPTY(const QString& shell, const QStringList& args);
    void setupPipeHandles();

    HANDLE m_masterHandle = INVALID_HANDLE_VALUE;
    HANDLE m_slaveHandle = INVALID_HANDLE_VALUE;
    HANDLE m_processHandle = INVALID_HANDLE_VALUE;
    DWORD m_processId = 0;

    QSocketNotifier* m_notifier = nullptr;
    QTimer* m_readTimer = nullptr;

    bool m_isRunning = false;
};

} // namespace DeepLux

#endif // Q_OS_WINDOWS

#endif // DEEPLUX_WINDOWS_PTY_IMPL_H
