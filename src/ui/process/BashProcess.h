#ifndef DEEPLUX_BASH_PROCESS_H
#define DEEPLUX_BASH_PROCESS_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QReadWriteLock>
#include <QTimer>

namespace DeepLux {

class PtyImpl;

/**
 * @brief Bash 进程管理器（单例）
 *
 * 跨平台支持：
 * - Linux: POSIX PTY
 * - Windows: ConPTY (Win10 1809+)
 *
 * 功能：
 * - 真正的 bash 终端
 * - 命令历史管理（线程安全）
 * - 输出节流（50ms）
 */
class BashProcess : public QObject
{
    Q_OBJECT

public:
    static BashProcess& instance();

    // 状态枚举
    enum State { NotStarted, Starting, Running, Failed };
    Q_ENUM(State)

    // 错误枚举
    enum Error { None, ShellNotFound, PtyOpenFailed, ForkFailed, ShellCrashed, Timeout };
    Q_ENUM(Error)

    State state() const { return m_state; }
    Error lastError() const { return m_lastError; }

    /**
     * @brief 写入命令到 bash（自动添加换行）
     */
    Q_INVOKABLE void writeCommand(const QString& command);

    /**
     * @brief 写入原始数据到 bash
     */
    Q_INVOKABLE void writeRaw(const QString& data);

    /**
     * @brief 获取命令历史
     */
    QStringList history() const;

    /**
     * @brief 添加命令到历史
     */
    void addToHistory(const QString& cmd);

    /**
     * @brief 清空历史
     */
    void clearHistory();

    /**
     * @brief 调整终端大小
     */
    void resize(int cols, int rows);

    /**
     * @brief 查找可用的 shell
     */
    static QString findAvailableShell();

    /**
     * @brief 检查 shell 是否可用
     */
    static bool isShellAvailable(const QString& shellPath);

signals:
    void stateChanged(State state) const;
    void outputReady(const QString& data) const;
    void errorReady(const QString& data) const;
    void commandFinished(int exitCode) const;
    void errorOccurred(Error error, const QString& details) const;

private:
    BashProcess(QObject* parent = nullptr);
    ~BashProcess() override;

    bool initialize();
    QString detectShell();

    PtyImpl* m_impl = nullptr;
    State m_state = NotStarted;
    Error m_lastError = None;
    QString m_shellPath;
    QStringList m_commandHistory;
    mutable QReadWriteLock m_historyLock;

    // 输出节流
    QTimer* m_outputThrottle = nullptr;
    QString m_pendingOutput;
};

} // namespace DeepLux

#endif // DEEPLUX_BASH_PROCESS_H
