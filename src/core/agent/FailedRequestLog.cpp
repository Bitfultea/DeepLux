#include "FailedRequestLog.h"

#include "ILLMClient.h"
#include "common/Logger.h"
#include "manager/ConfigManager.h"
#include "platform/PathUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>

namespace DeepLux {

namespace {
constexpr int kMessagesHeadChars = 2048;
constexpr int kResponseHeadChars = 64 * 1024;
constexpr int kMaxMessagesInRecord = 16;

QString applyRedaction(const QString& input, QStringList* applied) {
    QString value = input;
    const bool hadImage = value.contains(QStringLiteral("data:image/"));
    value.replace(QRegularExpression(QStringLiteral("data:image/[^;]+;base64,[A-Za-z0-9+/=\\s]+")),
                  QStringLiteral("data:image/…;base64,[omitted]"));
    const bool hadBearer = value.contains(QRegularExpression(QStringLiteral("Bearer\\s+[A-Za-z0-9._\\-]{4,}")));
    value.replace(QRegularExpression(QStringLiteral("Bearer\\s+[A-Za-z0-9._\\-]+")), QStringLiteral("Bearer ***"));
    const bool hadKey = value.contains(
        QRegularExpression(QStringLiteral("(?i)\\b(authorization|api[_-]?key|token|secret|password)\\b\"?\\s*[:=]")));
    value.replace(QRegularExpression(QStringLiteral("(?i)\\b(authorization|api[_-]?key|token|secret|password)\\b\"?"
                                                    "\\s*[:=]\\s*\"?[^\",\\s}]+")),
                  QStringLiteral("\\1: \"***\""));
    const bool hadLongBase64 = value.contains(QRegularExpression(QStringLiteral("[A-Za-z0-9+/=]{4096,}")));
    value.replace(QRegularExpression(QStringLiteral("[A-Za-z0-9+/=]{4096,}")), QStringLiteral("[omitted-base64]"));
    if (applied) {
        if (hadBearer && !applied->contains(QStringLiteral("authorization")))
            applied->append(QStringLiteral("authorization"));
        if (hadKey && !applied->contains(QStringLiteral("apiKey")))
            applied->append(QStringLiteral("apiKey"));
        if ((hadImage || hadLongBase64) && !applied->contains(QStringLiteral("imageBase64")))
            applied->append(QStringLiteral("imageBase64"));
    }
    return value;
}

bool containsUnredactedSecret(const QString& text) {
    // 值长度 >=4 且不是 *** 才视为未脱敏（避免把 `apiKey: \"***\"` 自身当成泄漏）
    static const QRegularExpression keyPattern(
        QStringLiteral("(?i)\\b(authorization|api[_-]?key|token|secret|password)\\b\"?\\s*[:=]\\s*"
                       "(?!\\\\?\"?\\*\\*\\*)[^\"\\s,}]{4,}"));
    static const QRegularExpression bearerPattern(QStringLiteral("Bearer\\s+[A-Za-z0-9._\\-]{4,}"));
    static const QRegularExpression imagePattern(QStringLiteral("data:image/[^;]+;base64,[A-Za-z0-9+/=\\s]{16,}"));
    static const QRegularExpression longBase64Pattern(QStringLiteral("[A-Za-z0-9+/=]{4096,}"));
    return keyPattern.match(text).hasMatch() || bearerPattern.match(text).hasMatch() ||
           imagePattern.match(text).hasMatch() || longBase64Pattern.match(text).hasMatch();
}

QString sanitizeEndpoint(const QString& endpoint) {
    const QUrl url(endpoint);
    if (!url.isValid() || url.scheme().isEmpty())
        return QString();
    QString sanitized = url.scheme() + QStringLiteral("://") + url.host();
    if (url.port() > 0)
        sanitized += QStringLiteral(":%1").arg(url.port());
    sanitized += url.path();
    return sanitized;
}
} // namespace

FailedRequestLog& FailedRequestLog::instance() {
    static FailedRequestLog instance;
    return instance;
}

void FailedRequestLog::setEnabledForTest(bool enabled) {
    m_enabledOverride = enabled ? 1 : 0;
}

void FailedRequestLog::setLogDirForTest(const QString& dir) {
    m_logDirOverride = dir;
}

void FailedRequestLog::setNowForTest(const QDateTime& now) {
    m_nowOverride = now;
}

void FailedRequestLog::clearNowForTest() {
    m_nowOverride = QDateTime();
}

void FailedRequestLog::setMaxFileBytesForTest(int bytes) {
    m_maxFileBytesOverride = bytes;
}

void FailedRequestLog::setRecordMaxBytesForTest(int bytes) {
    m_recordMaxBytesOverride = bytes;
}

void FailedRequestLog::setRetentionDaysForTest(int days) {
    m_retentionDaysOverride = days;
}

void FailedRequestLog::resetForTest() {
    m_disabled = false;
    m_disabledDate = QDate();
    m_dailyCapWarned = false;
    m_retainedDateValid = false;
    m_retentionDate = QDate();
    m_consecutiveFailures = 0;
    m_droppedRecords = 0;
}

bool FailedRequestLog::enabled() const {
    if (m_enabledOverride >= 0)
        return m_enabledOverride == 1;
    if (!ConfigManager::instance().isInitialized())
        return true;
    return ConfigManager::instance().groupBool("agent", "failedRequestLogEnabled", true);
}

int FailedRequestLog::maxFileBytes() const {
    if (m_maxFileBytesOverride > 0)
        return m_maxFileBytesOverride; // 测试注入（可低于生产下限）
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "failedRequestMaxBytes", 1048576)
                               : 1048576;
    return qBound(65536, configured, 16777216);
}

int FailedRequestLog::recordMaxBytes() const {
    if (m_recordMaxBytesOverride > 0)
        return m_recordMaxBytesOverride;
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "failedRequestRecordMaxBytes", 262144)
                               : 262144;
    return qBound(16384, configured, 1048576);
}

int FailedRequestLog::retentionDays() const {
    if (m_retentionDaysOverride > 0)
        return m_retentionDaysOverride;
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "auditRetentionDays", 14)
                               : 14;
    return qBound(1, configured, 365);
}

QString FailedRequestLog::logDir() const {
    return m_logDirOverride.isEmpty() ? PathUtils::logPath() : m_logDirOverride;
}

QDateTime FailedRequestLog::now() const {
    return m_nowOverride.isValid() ? m_nowOverride : QDateTime::currentDateTime();
}

QString FailedRequestLog::filePathFor(const QDate& date) const {
    return QDir(logDir()).filePath(QStringLiteral("llm-failed-%1.json").arg(date.toString(QStringLiteral("yyyyMMdd"))));
}

void FailedRequestLog::warnOnce(const QString& message) {
    Logger::instance().warning(message, QStringLiteral("Agent"));
}

void FailedRequestLog::onWriteFailure(const QString& reason) {
    ++m_droppedRecords;
    ++m_consecutiveFailures;
    qWarning() << "[FailedRequestLog] write failed:" << reason;
    warnOnce(QStringLiteral("failed-request log write failed: %1").arg(reason));
    if (m_consecutiveFailures >= 3 && !m_disabled) {
        m_disabled = true;
        m_disabledDate = now().date();
        warnOnce(QStringLiteral("failed-request log disabled after 3 consecutive failures"));
    }
}

void FailedRequestLog::cleanupRetention(const QDate& today) {
    if (m_retainedDateValid && m_retentionDate == today)
        return;
    m_retainedDateValid = true;
    m_retentionDate = today;
    m_dailyCapWarned = false; // 新的一天 = 新文件，单日上限重新计
    QDir dir(logDir());
    if (!dir.exists())
        return;
    const QDate cutoff = today.addDays(-retentionDays());
    const QRegularExpression namePattern(QStringLiteral("^llm-failed-(\\d{8})\\.json$"));
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("llm-failed-*.json")}, QDir::Files, QDir::NoSort);
    for (const QFileInfo& info : files) {
        const QRegularExpressionMatch match = namePattern.match(info.fileName());
        QDate fileDate;
        if (match.hasMatch())
            fileDate = QDate::fromString(match.captured(1), QStringLiteral("yyyyMMdd"));
        if (!fileDate.isValid())
            fileDate = info.lastModified().date();
        if (fileDate.isValid() && fileDate < cutoff && !QFile::remove(info.absoluteFilePath()))
            warnOnce(QStringLiteral("failed-request log: failed to remove %1").arg(info.fileName()));
    }
}

QJsonObject FailedRequestLog::buildRecord(const LLMErrorInfo& info, const QByteArray& requestBody,
                                          const QString& endpoint, const QString& model,
                                          const QList<AgentMessage>& messages, QStringList& redactionApplied) const {
    QJsonObject record;
    record.insert(QStringLiteral("ts"),
                  (m_nowOverride.isValid() ? m_nowOverride : QDateTime::currentDateTime()).toString(Qt::ISODateWithMs));
    record.insert(QStringLiteral("category"), llmErrorCategoryName(info.category));
    record.insert(QStringLiteral("httpStatus"), info.httpStatus);
    record.insert(QStringLiteral("attempts"), info.attempts);
    record.insert(QStringLiteral("endpoint"), sanitizeEndpoint(endpoint));
    record.insert(QStringLiteral("model"), model);
    record.insert(QStringLiteral("contextLimitSuspected"), info.contextLimitSuspected);
    record.insert(QStringLiteral("visionDowngradeHint"), info.visionDowngradeHint);

    QJsonArray messageArray;
    int totalChars = 0;
    for (const AgentMessage& message : messages) {
        totalChars += message.content.size();
        QJsonObject item;
        item.insert(QStringLiteral("role"), message.role);
        item.insert(QStringLiteral("chars"), message.content.size());
        item.insert(QStringLiteral("head"),
                    applyRedaction(message.content.left(kMessagesHeadChars), &redactionApplied));
        item.insert(QStringLiteral("images"), message.images.size());
        if (!message.toolCalls.isEmpty())
            item.insert(QStringLiteral("toolCalls"), message.toolCalls.size());
        if (!message.toolCallId.isEmpty())
            item.insert(QStringLiteral("toolCallId"), applyRedaction(message.toolCallId, &redactionApplied));
        messageArray.append(item);
    }
    record.insert(QStringLiteral("request"),
                  QJsonObject{{QStringLiteral("messageCount"), messages.size()},
                              {QStringLiteral("totalChars"), qMax(totalChars, static_cast<int>(requestBody.size()))},
                              {QStringLiteral("messages"), messageArray}});

    const QString detail = applyRedaction(info.detail, &redactionApplied);
    record.insert(QStringLiteral("response"), QJsonObject{{QStringLiteral("chars"), info.detail.size()},
                                                          {QStringLiteral("head"), detail.left(kResponseHeadChars)}});
    record.insert(QStringLiteral("detail"), detail);
    QJsonArray applied;
    for (const QString& item : redactionApplied)
        applied.append(item);
    record.insert(QStringLiteral("redactionApplied"), applied);
    return record;
}

void FailedRequestLog::record(const LLMErrorInfo& info, const QByteArray& requestBody, const QString& endpoint,
                              const QString& model, const QList<AgentMessage>& messages) {
    // §4.1：只记录持久性失败；Auth/瞬时/中止一律不落盘
    if (info.category != LLMErrorCategory::BadRequest && info.category != LLMErrorCategory::Malformed)
        return;
    if (!enabled())
        return;
    if (m_disabled) {
        // 自禁用只持续到下一个自然日（同 §3.5）
        if (m_disabledDate.isValid() && now().date() == m_disabledDate)
            return;
        m_disabled = false;
        m_consecutiveFailures = 0;
    }

    QStringList redactionApplied;
    QJsonObject record = buildRecord(info, requestBody, endpoint, model, messages, redactionApplied);

    // 单条上限：先砍 response.head → 再砍 messages 前 8 条 → 最后省略 response.head
    QByteArray recordBytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    if (recordBytes.size() > recordMaxBytes()) {
        QJsonObject response = record.value(QStringLiteral("response")).toObject();
        response.insert(QStringLiteral("head"), response.value(QStringLiteral("head")).toString().left(16384));
        record.insert(QStringLiteral("response"), response);
        recordBytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    }
    if (recordBytes.size() > recordMaxBytes()) {
        QJsonObject request = record.value(QStringLiteral("request")).toObject();
        QJsonArray messagesArray = request.value(QStringLiteral("messages")).toArray();
        while (messagesArray.size() > 8)
            messagesArray.removeLast();
        request.insert(QStringLiteral("messages"), messagesArray);
        record.insert(QStringLiteral("request"), request);
        recordBytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    }
    if (recordBytes.size() > recordMaxBytes()) {
        QJsonObject response = record.value(QStringLiteral("response")).toObject();
        response.remove(QStringLiteral("head"));
        record.insert(QStringLiteral("response"), response);
        recordBytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    }
    // 最后兜底（设计未禁止更保守的截断）：消息 head 降到 256 字符且只留前 4 条
    if (recordBytes.size() > recordMaxBytes()) {
        QJsonObject request = record.value(QStringLiteral("request")).toObject();
        QJsonArray messagesArray = request.value(QStringLiteral("messages")).toArray();
        QJsonArray trimmed;
        for (int i = 0; i < messagesArray.size() && i < 4; ++i) {
            QJsonObject item = messagesArray.at(i).toObject();
            item.insert(QStringLiteral("head"), item.value(QStringLiteral("head")).toString().left(256));
            trimmed.append(item);
        }
        request.insert(QStringLiteral("messages"), trimmed);
        record.insert(QStringLiteral("request"), request);
        recordBytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    }
    if (recordBytes.size() > recordMaxBytes()) {
        ++m_droppedRecords;
        warnOnce(QStringLiteral("failed-request record dropped: exceeds record size limit"));
        return;
    }

    // 脱敏自检：序列化文本仍命中敏感模式 → 整条丢弃（宁可无记录，不可泄密）
    if (containsUnredactedSecret(QString::fromUtf8(recordBytes))) {
        ++m_droppedRecords;
        warnOnce(QStringLiteral("failed-request record dropped by redaction self-check"));
        return;
    }

    const QDate today = now().date();
    if (!PathUtils::ensureDirExists(logDir())) {
        onWriteFailure(QStringLiteral("cannot create log directory: %1").arg(logDir()));
        return;
    }
    cleanupRetention(today);
    const QString path = filePathFor(today);

    QJsonArray records;
    qint64 existingBytes = 0;
    QFile existing(path);
    if (existing.exists()) {
        existingBytes = existing.size();
        if (existing.open(QIODevice::ReadOnly)) {
            const QJsonDocument existingDoc = QJsonDocument::fromJson(existing.readAll());
            existing.close();
            if (existingDoc.isObject() && existingDoc.object().value(QStringLiteral("records")).isArray())
                records = existingDoc.object().value(QStringLiteral("records")).toArray();
            else if (existingBytes > 0)
                warnOnce(QStringLiteral("failed-request log: existing file is not valid JSON, appending fresh"));
        }
    }
    records.append(record);
    const QJsonObject root{{QStringLiteral("v"), 1}, {QStringLiteral("records"), records}};
    const QByteArray document = QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';

    if (document.size() > maxFileBytes()) {
        if (!m_dailyCapWarned) {
            m_dailyCapWarned = true;
            warnOnce(QStringLiteral("failed-request log daily limit reached; further records skipped"));
        }
        ++m_droppedRecords;
        return;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        onWriteFailure(QStringLiteral("cannot open %1: %2").arg(path, file.errorString()));
        return;
    }
    if (file.write(document) != document.size() || !file.commit()) {
        onWriteFailure(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
        return;
    }
    m_consecutiveFailures = 0;
}

QString FailedRequestLog::currentFilePath() const {
    return filePathFor(now().date());
}

} // namespace DeepLux
