#include "BashProcess.h"

#include "PtyImpl.h"

#include <QFileInfo>
#include <QSocketNotifier>
#include <QDebug>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>

#if defined(Q_OS_LINUX)
#include <pty.h>
#include <unistd.h>
#elif defined(Q_OS_FREEBSD)
#include <libutil.h>
#include <unistd.h>
#endif

namespace DeepLux {

// Linux POSIX PTY 实现
class LinuxPtyImpl : public PtyImpl
{
public:
    LinuxPtyImpl()
        : m_masterFd(-1)
        , m_pid(-1)
        , m_notifier(nullptr)
    {
    }

    ~LinuxPtyImpl() override
    {
        kill();
    }

    bool start(const QString& shell, const QStringList& args) override
    {
        Q_UNUSED(args);

        // 打开 PTY master
        m_masterFd = ::open("/dev/ptmx", O_RDWR | O_NOCTTY);
        if (m_masterFd < 0) {
            emit errorOccurred("Failed to open /dev/ptmx: " + QString::fromUtf8(strerror(errno)));
            return false;
        }

        // 解锁 PTY
        if (grantpt(m_masterFd) < 0 || unlockpt(m_masterFd) < 0) {
            emit errorOccurred("Failed to grant/unlock PTY");
            ::close(m_masterFd);
            m_masterFd = -1;
            return false;
        }

        // Fork
        pid_t pid = fork();
        if (pid < 0) {
            emit errorOccurred("Fork failed: " + QString::fromUtf8(strerror(errno)));
            ::close(m_masterFd);
            m_masterFd = -1;
            return false;
        }

        if (pid == 0) {
            // 子进程
            ::close(m_masterFd);

            int slaveFd = ::open(ptsname(m_masterFd), O_RDWR);
            if (slaveFd < 0) {
                _exit(1);
            }

            // 创建新的会话，使子进程成为会话首领
            setsid();

            // 设置控制终端
            ioctl(slaveFd, TIOCSCTTY, 0);

            // 重定向标准文件描述符
            ::dup2(slaveFd, STDIN_FILENO);
            ::dup2(slaveFd, STDOUT_FILENO);
            ::dup2(slaveFd, STDERR_FILENO);

            if (slaveFd > STDERR_FILENO) {
                ::close(slaveFd);
            }

            // 执行 shell
            execl(shell.toUtf8(), "bash", "--login", "-i", nullptr);
            _exit(1);
        }

        // 父进程
        m_pid = pid;
        m_shell = shell;

        // 设置 socketNotifier 监听输出
        m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &LinuxPtyImpl::onReadyRead);

        // 设置非阻塞
        int flags = fcntl(m_masterFd, F_GETFL, 0);
        fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

        return true;
    }

    void write(const QByteArray& data) override
    {
        if (m_masterFd >= 0) {
            ::write(m_masterFd, data.constData(), data.size());
        }
    }

    QByteArray read() override
    {
        QByteArray buffer;
        if (m_masterFd >= 0) {
            char buf[4096];
            int n = ::read(m_masterFd, buf, sizeof(buf));
            if (n > 0) {
                buffer.append(buf, n);
            }
        }
        return buffer;
    }

    void resize(int cols, int rows) override
    {
        if (m_masterFd >= 0) {
            struct winsize ws;
            ws.ws_col = static_cast<unsigned short>(cols);
            ws.ws_row = static_cast<unsigned short>(rows);
            ioctl(m_masterFd, TIOCSWINSZ, &ws);
        }
    }

    void kill() override
    {
        if (m_pid > 0) {
            ::kill(m_pid, SIGTERM);
            m_pid = -1;
        }
        if (m_masterFd >= 0) {
            ::close(m_masterFd);
            m_masterFd = -1;
        }
    }

    bool isRunning() const override
    {
        if (m_pid <= 0) return false;
        return ::kill(m_pid, 0) == 0;
    }

private slots:
    void onReadyRead()
    {
        QByteArray data = read();
        if (!data.isEmpty()) {
            emit outputReady(data);
        }
    }

private:
    int m_masterFd;
    pid_t m_pid;
    QString m_shell;
    QSocketNotifier* m_notifier = nullptr;
};

} // namespace DeepLux
