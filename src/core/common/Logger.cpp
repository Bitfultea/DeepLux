#include "Logger.h"
#include "core/platform/PathUtils.h"
#include <QDebug>

namespace DeepLux {

Logger& Logger::instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger()
    : QObject(nullptr)
{
    // 设置默认日志文件路径
    m_logFilePath = defaultLogFilePath();

    // 确保日志目录存在
    QDir dir(QFileInfo(m_logFilePath).absolutePath());
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 打开日志文件
    m_logFile = new QFile(m_logFilePath, this);
    if (m_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_logStream = new QTextStream(m_logFile);
    }

    // 添加启动日志
    addLog(QString("=== DeepLux Vision Started at %1 ===").arg(
        QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")),
        LogLevel::Info, "System");
}

Logger::~Logger()
{
    if (m_logFile && m_logFile->isOpen()) {
        addLog(QString("=== DeepLux Vision Closed at %1 ===").arg(
            QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")),
            LogLevel::Info, "System");
        m_logFile->close();
    }
}

QString Logger::defaultLogDirectory()
{
    return PathUtils::logPath();
}

QString Logger::defaultLogFilePath()
{
    QString logDir = defaultLogDirectory();
    QString fileName = QString("DeepLux_%1.log")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd"));
    return logDir + "/" + fileName;
}

void Logger::addLog(const QString& message, LogLevel level, const QString& category)
{
    if (level < m_minLevel) {
        return;
    }

    QMutexLocker locker(&m_mutex);

    LogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.level = level;
    entry.message = message;
    entry.category = category;

    m_logs.append(entry);

    // 限制内存中的日志数量
    while (m_logs.size() > m_maxLogsInMemory) {
        m_logs.removeFirst();
    }

    // 写入文件
    writeToFile(entry);

    // 发送信号
    emit logAdded(entry);
}

void Logger::debug(const QString& message, const QString& category)
{
    addLog(message, LogLevel::Debug, category);
}

void Logger::info(const QString& message, const QString& category)
{
    addLog(message, LogLevel::Info, category);
}

void Logger::warning(const QString& message, const QString& category)
{
    addLog(message, LogLevel::Warning, category);
}

void Logger::error(const QString& message, const QString& category)
{
    addLog(message, LogLevel::Error, category);
}

void Logger::success(const QString& message, const QString& category)
{
    addLog(message, LogLevel::Success, category);
}

QList<LogEntry> Logger::logs(LogLevel level) const
{
    QList<LogEntry> result;
    for (const auto& log : m_logs) {
        if (log.level == level) {
            result.append(log);
        }
    }
    return result;
}

QList<LogEntry> Logger::logs(const QString& category) const
{
    QList<LogEntry> result;
    for (const auto& log : m_logs) {
        if (log.category == category) {
            result.append(log);
        }
    }
    return result;
}

void Logger::clearLogs()
{
    QMutexLocker locker(&m_mutex);
    m_logs.clear();
    emit logsCleared();
}

void Logger::setLogToFile(bool enabled)
{
    QMutexLocker locker(&m_mutex);
    m_logToFile = enabled;
}

void Logger::setMaxLogFileSize(qint64 maxBytes)
{
    QMutexLocker locker(&m_mutex);
    m_maxLogFileSize = maxBytes;
}

void Logger::setMinLevel(LogLevel level)
{
    m_minLevel = level;
}

void Logger::writeToFile(const LogEntry& entry)
{
    if (!m_logToFile || !m_logStream) {
        return;
    }

    rotateLogFileIfNeeded();

    QString logLine = QString("[%1] [%2] [%3] %4")
        .arg(entry.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(levelToString(entry.level))
        .arg(entry.category.isEmpty() ? "-" : entry.category)
        .arg(entry.message);

    *m_logStream << logLine << "\n";
    m_logStream->flush();
}

void Logger::rotateLogFileIfNeeded()
{
    if (!m_logFile || !m_logFile->isOpen()) {
        return;
    }

    if (m_logFile->size() >= m_maxLogFileSize) {
        // 关闭当前文件
        m_logFile->close();
        delete m_logStream;
        m_logStream = nullptr;

        // 备份旧文件
        QString backupPath = m_logFilePath + ".old";
        QFile::remove(backupPath);
        QFile::rename(m_logFilePath, backupPath);

        // 重新打开新文件
        if (m_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            m_logStream = new QTextStream(m_logFile);
        }
    }
}

QString Logger::levelToString(LogLevel level)
{
    switch (level) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warning:  return "WARN";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Success:  return "SUCCESS";
        default:                 return "UNKNOWN";
    }
}

QString Logger::levelToColor(LogLevel level)
{
    switch (level) {
        case LogLevel::Debug:   return "#808080";
        case LogLevel::Info:    return "#00aaff";
        case LogLevel::Warning:  return "#ffaa00";
        case LogLevel::Error:    return "#ff4444";
        case LogLevel::Success:  return "#00ff00";
        default:                 return "#ffffff";
    }
}

} // namespace DeepLux