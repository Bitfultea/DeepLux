#ifndef DEEPLUX_PTY_IMPL_H
#define DEEPLUX_PTY_IMPL_H

#include <QObject>
#include <QByteArray>
#include <QString>

namespace DeepLux {

/**
 * @brief PTY 抽象基类，屏蔽平台差异
 *
 * Linux 使用 POSIX PTY (/dev/ptmx)
 * Windows 使用 ConPTY (CreatePseudoConsole)
 */
class PtyImpl : public QObject
{
    Q_OBJECT

public:
    ~PtyImpl() override = default;

    /**
     * @brief 启动 shell 进程
     * @param shell shell 路径
     * @param args shell 参数
     * @return 启动是否成功
     */
    virtual bool start(const QString& shell, const QStringList& args) = 0;

    /**
     * @brief 写入数据到 PTY
     * @param data 要写入的数据
     */
    virtual void write(const QByteArray& data) = 0;

    /**
     * @brief 从 PTY 读取数据
     * @return 读取到的数据
     */
    virtual QByteArray read() = 0;

    /**
     * @brief 调整终端大小
     * @param cols 列数
     * @param rows 行数
     */
    virtual void resize(int cols, int rows) = 0;

    /**
     * @brief 终止进程
     */
    virtual void kill() = 0;

    /**
     * @brief 检查进程是否正在运行
     */
    virtual bool isRunning() const = 0;

signals:
    void outputReady(const QByteArray& data);
    void errorReady(const QByteArray& data);
    void finished(int exitCode);
    void errorOccurred(const QString& error);
};

} // namespace DeepLux

#endif // DEEPLUX_PTY_IMPL_H
