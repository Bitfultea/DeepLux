#ifndef DEEPLUX_FAILED_REQUEST_LOG_H
#define DEEPLUX_FAILED_REQUEST_LOG_H

#include "platform/Platform.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace DeepLux {

struct AgentMessage;
struct LLMErrorInfo;

/**
 * @brief LLM 失败请求报文落盘（设计 §4）
 *
 * - 仅记录 BadRequest / Malformed（Auth 永不落盘），同一请求只在最终失败时记一条
 * - 文件：<logs>/llm-failed-YYYYMMDD.json，结构 {"v":1,"records":[...]}，QSaveFile 原子重写
 * - 上限：单条 256 KiB（可截断）、单日 1 MiB（达到后停止追加）；保留 14 天
 * - R1~R5 脱敏 + 落盘前自检；自检不过整条丢弃（宁可无记录，不可泄密）
 * - 写失败只记日志，不影响原有错误处理/对话
 */
class DEEPLUX_API FailedRequestLog {
public:
    static FailedRequestLog& instance();

    /// requestBody = 序列化后的请求体（只用于统计与脱敏标记，不整体落盘）
    void record(const LLMErrorInfo& info, const QByteArray& requestBody, const QString& endpoint, const QString& model,
                const QList<AgentMessage>& messages);

    // ---- 测试/诊断可见 ----
    DEEPLUX_TEST_VISIBILITY : void setEnabledForTest(bool enabled);
    void clearEnabledOverrideForTest() {
        m_enabledOverride = -1;
    }
    bool enabledForTest() const {
        return enabled();
    }
    void setLogDirForTest(const QString& dir);
    void setNowForTest(const QDateTime& now);
    void clearNowForTest();
    void setMaxFileBytesForTest(int bytes);
    void setRecordMaxBytesForTest(int bytes);
    void setRetentionDaysForTest(int days);
    void resetForTest();
    QString currentFilePath() const;
    int droppedRecords() const {
        return m_droppedRecords;
    }

private:
    FailedRequestLog() = default;

    bool enabled() const;
    int maxFileBytes() const;
    int recordMaxBytes() const;
    int retentionDays() const;
    QString logDir() const;
    QDateTime now() const;
    QString filePathFor(const QDate& date) const;
    QJsonObject buildRecord(const LLMErrorInfo& info, const QByteArray& requestBody, const QString& endpoint,
                            const QString& model, const QList<AgentMessage>& messages,
                            QStringList& redactionApplied) const;
    void cleanupRetention(const QDate& today);
    void warnOnce(const QString& message);
    void onWriteFailure(const QString& reason);

    bool m_disabled = false;
    bool m_dailyCapWarned = false;
    bool m_retainedDateValid = false;
    QDate m_retentionDate;
    QDate m_disabledDate; // 自禁用所在日；跨天后自动恢复（§4.2 同 §3.5）
    int m_consecutiveFailures = 0;
    int m_droppedRecords = 0;

    // 测试注入
    QString m_logDirOverride;
    QDateTime m_nowOverride;
    int m_enabledOverride = -1;
    int m_maxFileBytesOverride = -1;
    int m_recordMaxBytesOverride = -1;
    int m_retentionDaysOverride = -1;
};

} // namespace DeepLux

#endif // DEEPLUX_FAILED_REQUEST_LOG_H
