#include "AgentAuditLog.h"

#include "AgentController.h"
#include "GuiEvent.h"
#include "common/Logger.h"
#include "manager/ConfigManager.h"
#include "platform/PathUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

namespace DeepLux {

namespace {
constexpr int kRecordMaxBytes = 16 * 1024;
constexpr int kParamsRawMaxChars = 8 * 1024;
constexpr int kMaxPartsPerDay = 99;

const QRegularExpression& sensitiveKeyPattern() {
    static const QRegularExpression re(QStringLiteral("^(api[_-]?key|authorization|token|secret|password|bearer)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression& dataImagePattern() {
    static const QRegularExpression re(QStringLiteral("data:image/[^;]+;base64,[A-Za-z0-9+/=\\s]+"));
    return re;
}

QString redactString(const QString& value) {
    QString result = value;
    result.replace(dataImagePattern(), QStringLiteral("data:image/…;base64,[omitted]"));
    return result;
}

QJsonValue redactValue(const QJsonValue& value) {
    if (value.isObject()) {
        QJsonObject out;
        for (auto it = value.toObject().constBegin(); it != value.toObject().constEnd(); ++it) {
            if (sensitiveKeyPattern().match(it.key()).hasMatch())
                out.insert(it.key(), QStringLiteral("***"));
            else
                out.insert(it.key(), redactValue(it.value()));
        }
        return out;
    }
    if (value.isArray()) {
        QJsonArray out;
        for (const QJsonValue& item : value.toArray())
            out.append(redactValue(item));
        return out;
    }
    if (value.isString())
        return redactString(value.toString());
    return value;
}

QString formatTimestamp(const QDateTime& timestamp) {
    const QDateTime value = timestamp.isValid() ? timestamp : QDateTime::currentDateTime();
    return value.toString(Qt::ISODateWithMs);
}
} // namespace

AgentAuditLog& AgentAuditLog::instance() {
    static AgentAuditLog instance;
    return instance;
}

void AgentAuditLog::setEnabledForTest(bool enabled) {
    m_enabledOverride = enabled ? 1 : 0;
}

void AgentAuditLog::setLogDirForTest(const QString& dir) {
    m_logDirOverride = dir;
}

void AgentAuditLog::setNowForTest(const QDateTime& now) {
    m_nowOverride = now;
}

void AgentAuditLog::clearNowForTest() {
    m_nowOverride = QDateTime();
}

void AgentAuditLog::setMaxFileBytesForTest(int bytes) {
    m_maxFileBytesOverride = bytes;
}

void AgentAuditLog::setRetentionDaysForTest(int days) {
    m_retentionDaysOverride = days;
}

void AgentAuditLog::resetForTest() {
    closeFile();
    m_currentDate = QDate();
    m_disabledDate = QDate();
    m_currentPart = 0;
    m_disabled = false;
    m_dayExhausted = false;
    m_inAppend = false;
    m_consecutiveFailures = 0;
    m_droppedRecords = 0;
    m_initialized = false;
}

bool AgentAuditLog::enabled() const {
    if (m_enabledOverride >= 0)
        return m_enabledOverride == 1;
    if (!ConfigManager::instance().isInitialized())
        return true;
    return ConfigManager::instance().groupBool("agent", "auditEnabled", true);
}

int AgentAuditLog::maxFileBytes() const {
    if (m_maxFileBytesOverride > 0)
        return m_maxFileBytesOverride; // 测试注入（可低于生产下限）
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "auditMaxFileBytes", 10485760)
                               : 10485760;
    return qBound(1048576, configured, 104857600);
}

int AgentAuditLog::retentionDays() const {
    if (m_retentionDaysOverride > 0)
        return m_retentionDaysOverride;
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "auditRetentionDays", 14)
                               : 14;
    return qBound(1, configured, 365);
}

QString AgentAuditLog::logDir() const {
    return m_logDirOverride.isEmpty() ? PathUtils::logPath() : m_logDirOverride;
}

QDateTime AgentAuditLog::now() const {
    return m_nowOverride.isValid() ? m_nowOverride : QDateTime::currentDateTime();
}

QString AgentAuditLog::filePathFor(const QDate& date, int part) const {
    const QString day = date.toString(QStringLiteral("yyyyMMdd"));
    const QString name = part <= 0
                             ? QStringLiteral("agent-audit-%1.jsonl").arg(day)
                             : QStringLiteral("agent-audit-%1-%2.jsonl").arg(day).arg(part, 2, 10, QLatin1Char('0'));
    return QDir(logDir()).filePath(name);
}

bool AgentAuditLog::initialize(QObject* controller) {
    auto* agentController = qobject_cast<AgentController*>(controller);
    if (!agentController)
        return false;
    // 以 controller 作为 context：AgentAuditLog 不是 QObject（设计 §3.3 的静态单例）
    if (m_signalConnection)
        QObject::disconnect(m_signalConnection);
    m_signalConnection = QObject::connect(agentController, &AgentController::actionLogEntryAdded, agentController,
                                          [this](const AgentActionLogEntry& entry) { append(entry); });
    m_disabled = false;
    m_consecutiveFailures = 0;
    m_initialized = true;
    // 启动即做一次保留清理（目录不存在时静默）
    if (enabled()) {
        const QDate today = now().date();
        m_currentDate = today;
        cleanupRetention();
    }
    return true;
}

void AgentAuditLog::shutdown() {
    flush();
    closeFile();
    if (m_signalConnection) {
        QObject::disconnect(m_signalConnection);
        m_signalConnection = QMetaObject::Connection();
    }
    m_initialized = false;
}

void AgentAuditLog::closeFile() {
    if (m_file.isOpen())
        m_file.close();
}

bool AgentAuditLog::ensureFileForCurrentDay() {
    const QDate today = now().date();
    if (m_file.isOpen() && m_currentDate == today)
        return true;
    closeFile();
    m_currentDate = today;
    m_currentPart = 0;
    m_dayExhausted = false;
    if (!PathUtils::ensureDirExists(logDir())) {
        onWriteFailure(QStringLiteral("cannot create log directory: %1").arg(logDir()));
        return false;
    }
    cleanupRetention();
    m_file.setFileName(filePathFor(m_currentDate, m_currentPart));
    if (!m_file.open(QIODevice::Append | QIODevice::WriteOnly)) {
        onWriteFailure(QStringLiteral("cannot open audit log: %1").arg(m_file.errorString()));
        return false;
    }
    return true;
}

bool AgentAuditLog::rotateToNextPart() {
    for (int part = m_currentPart + 1; part <= kMaxPartsPerDay; ++part) {
        const QString path = filePathFor(m_currentDate, part);
        const QFileInfo info(path);
        if (info.exists() && info.size() >= maxFileBytes())
            continue;
        closeFile();
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::Append | QIODevice::WriteOnly)) {
            onWriteFailure(QStringLiteral("cannot open audit log part: %1").arg(m_file.errorString()));
            return false;
        }
        m_currentPart = part;
        return true;
    }
    if (!m_dayExhausted) {
        m_dayExhausted = true;
        Logger::instance().warning(QStringLiteral("agent audit log: daily parts exhausted (%1)").arg(kMaxPartsPerDay),
                                   QStringLiteral("Agent"));
    }
    return false;
}

void AgentAuditLog::cleanupRetention() {
    QDir dir(logDir());
    if (!dir.exists())
        return;
    const QDate cutoff = now().date().addDays(-retentionDays());
    const QRegularExpression namePattern(QStringLiteral("^agent-audit-(\\d{8})(?:-\\d{2})?\\.jsonl$"));
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("agent-audit-*.jsonl")}, QDir::Files, QDir::NoSort);
    for (const QFileInfo& info : files) {
        const QRegularExpressionMatch match = namePattern.match(info.fileName());
        QDate fileDate;
        if (match.hasMatch())
            fileDate = QDate::fromString(match.captured(1), QStringLiteral("yyyyMMdd"));
        if (!fileDate.isValid())
            fileDate = info.lastModified().date();
        if (fileDate.isValid() && fileDate < cutoff) {
            if (!QFile::remove(info.absoluteFilePath()))
                Logger::instance().warning(QStringLiteral("agent audit log: failed to remove %1").arg(info.fileName()),
                                           QStringLiteral("Agent"));
        }
    }
}

void AgentAuditLog::onWriteFailure(const QString& reason) {
    ++m_droppedRecords;
    ++m_consecutiveFailures;
    qWarning() << "[AgentAuditLog] write failed:" << reason;
    Logger::instance().warning(QStringLiteral("agent audit write failed: %1").arg(reason), QStringLiteral("Agent"));
    if (m_consecutiveFailures >= 3 && !m_disabled) {
        m_disabled = true;
        m_disabledDate = now().date();
        Logger::instance().warning(QStringLiteral("agent audit log disabled after 3 consecutive failures"),
                                   QStringLiteral("Agent"));
    }
}

void AgentAuditLog::append(const AgentActionLogEntry& entry) {
    if (!enabled())
        return;
    if (m_disabled) {
        // 自禁用只持续到下一个自然日（或重新 initialize，见设计 §3.5）
        if (m_disabledDate.isValid() && now().date() == m_disabledDate)
            return;
        m_disabled = false;
        m_consecutiveFailures = 0;
    }
    if (m_inAppend)
        return;
    m_inAppend = true;

    QJsonObject record;
    record.insert(QStringLiteral("v"), 1);
    record.insert(QStringLiteral("ts"), formatTimestamp(entry.timestamp));
    record.insert(QStringLiteral("actor"), entry.actor);
    record.insert(QStringLiteral("action"), entry.action);

    // params：可解析 JSON → 脱敏后嵌入；否则 {"raw": 前 8KB}
    const QJsonDocument paramsDoc = QJsonDocument::fromJson(entry.params.toUtf8());
    if (paramsDoc.isObject())
        record.insert(QStringLiteral("params"), redactValue(QJsonValue(paramsDoc.object())));
    else if (paramsDoc.isArray())
        record.insert(QStringLiteral("params"), redactValue(QJsonValue(paramsDoc.array())));
    else {
        QString raw = entry.params.left(kParamsRawMaxChars);
        if (entry.params.size() > kParamsRawMaxChars)
            raw += QStringLiteral("…[truncated]");
        record.insert(QStringLiteral("params"), QJsonObject{{QStringLiteral("raw"), redactString(raw)}});
    }

    record.insert(QStringLiteral("result"), entry.result);
    record.insert(QStringLiteral("undoable"), entry.undoable);
    if (entry.durationMs >= 0)
        record.insert(QStringLiteral("durationMs"), entry.durationMs);
    if (!entry.errorCategory.isEmpty())
        record.insert(QStringLiteral("errorCategory"), entry.errorCategory);
    if (!entry.toolCallId.isEmpty())
        record.insert(QStringLiteral("toolCallId"), entry.toolCallId);

    QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact);
    if (line.size() > kRecordMaxBytes) {
        record.insert(QStringLiteral("params"), QJsonObject{{QStringLiteral("raw"), QStringLiteral("…[truncated]")}});
        line = QJsonDocument(record).toJson(QJsonDocument::Compact);
    }
    line += '\n';

    if (!ensureFileForCurrentDay()) {
        m_inAppend = false;
        return;
    }
    // 分片：写入后不超过上限；空文件至少写一条，避免死循环
    if (m_file.size() > 0 && m_file.size() + line.size() > maxFileBytes()) {
        if (!rotateToNextPart()) {
            ++m_droppedRecords;
            m_inAppend = false;
            return;
        }
    }

    const qint64 written = m_file.write(line);
    if (written != line.size() || !m_file.flush()) {
        onWriteFailure(QStringLiteral("short write to %1").arg(m_file.fileName()));
    } else {
        m_consecutiveFailures = 0;
    }
    m_inAppend = false;
}

void AgentAuditLog::flush() {
    if (m_file.isOpen())
        m_file.flush();
}

QString AgentAuditLog::currentFilePath() const {
    if (m_file.isOpen())
        return m_file.fileName();
    const QDate date = m_currentDate.isValid() ? m_currentDate : now().date();
    return filePathFor(date, m_currentPart);
}

} // namespace DeepLux
