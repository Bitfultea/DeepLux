#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTextStream>

namespace DeepLux {

/**
 * @brief 日志类型
 */
enum class LogLevel { Debug, Info, Warning, Error, Success };

/**
 * @brief 日志条目
 */
struct LogEntry {
    QDateTime timestamp;
    LogLevel level;
    QString message;
    QString category;
};

/**
 * @brief 日志系统
 *
 * 单例模式，全局日志记录器
 */
class Logger : public QObject {
    Q_OBJECT

public:
    static Logger& instance();

    // 日志记录接口
    void addLog(const QString& message, LogLevel level = LogLevel::Info, const QString& category = QString());
    void debug(const QString& message, const QString& category = QString());
    void info(const QString& message, const QString& category = QString());
    void warning(const QString& message, const QString& category = QString());
    void error(const QString& message, const QString& category = QString());
    void success(const QString& message, const QString& category = QString());

    // 日志查询
    QList<LogEntry> logs() const;
    QList<LogEntry> logs(LogLevel level) const;
    QList<LogEntry> logs(const QString& category) const;
    void clearLogs();

    // 日志文件
    QString logFilePath() const;
    bool isLogToFileEnabled() const;
    void setLogToFile(bool enabled);
    void setMaxLogFileSize(qint64 maxBytes);

    // 日志级别过滤
    void setMinLevel(LogLevel level);
    LogLevel minLevel() const;

signals:
    void logAdded(const LogEntry& entry);
    void logsCleared();

public:
    // 辅助方法
    static QString levelToString(LogLevel level);
    static QString levelToColor(LogLevel level);

private:
    Logger();
    ~Logger();

    void writeToFile(const LogEntry& entry);
    void rotateLogFileIfNeeded();

    static QString defaultLogDirectory();
    static QString defaultLogFilePath();

    QList<LogEntry> m_logs;
    QString m_logFilePath;
    QFile* m_logFile = nullptr;
    QTextStream* m_logStream = nullptr;
    mutable QMutex m_mutex{QMutex::Recursive};
    bool m_logToFile = true;
    LogLevel m_minLevel = LogLevel::Debug;
    qint64 m_maxLogFileSize = 10 * 1024 * 1024; // 10MB
    int m_maxLogsInMemory = 1000;
};

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::LogEntry)
