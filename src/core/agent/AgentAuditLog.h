#ifndef DEEPLUX_AGENT_AUDIT_LOG_H
#define DEEPLUX_AGENT_AUDIT_LOG_H

#include "platform/Platform.h"

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QObject>
#include <QString>

namespace DeepLux {

struct AgentActionLogEntry;

/**
 * @brief Agent 审计日志落盘（JSON Lines，设计 §3）
 *
 * - 目录：PathUtils::logPath()（`<appData>/logs`，DEEPLUX_APP_DATA_DIR 可覆盖）
 * - 当日文件：agent-audit-YYYYMMDD.jsonl；单文件 >= agent/auditMaxFileBytes（默认 10 MiB）
 *   时换分片 -01/-02…（单日最多 99 片）；保留 agent/auditRetentionDays（默认 14 天）
 * - 写失败只记日志、不改变对话；连续 3 次失败本进程自禁用
 * - 只通过 AgentController::actionLogEntryAdded 信号接线，保证所有审计条目都落盘
 */
class DEEPLUX_API AgentAuditLog {
public:
    static AgentAuditLog& instance();

    /// 连接控制器审计信号；失败仅日志，不阻塞初始化
    bool initialize(QObject* controller);
    void shutdown();

    /// 同步追加一行（GUI 线程；重入保护）
    void append(const AgentActionLogEntry& entry);
    void flush();

    // ---- 测试/诊断可见 ----
    DEEPLUX_TEST_VISIBILITY : void setEnabledForTest(bool enabled);
    void setLogDirForTest(const QString& dir);
    void setNowForTest(const QDateTime& now);
    void clearNowForTest();
    void setMaxFileBytesForTest(int bytes);
    void setRetentionDaysForTest(int days);
    void resetForTest();
    QString currentFilePath() const;
    int droppedRecords() const {
        return m_droppedRecords;
    }

private:
    AgentAuditLog() = default;

    bool enabled() const;
    int maxFileBytes() const;
    int retentionDays() const;
    QString logDir() const;
    QDateTime now() const;

    bool ensureFileForCurrentDay();
    bool rotateToNextPart();
    void cleanupRetention();
    QString filePathFor(const QDate& date, int part) const;
    void onWriteFailure(const QString& reason);
    void closeFile();

    QFile m_file;
    QMetaObject::Connection m_signalConnection;
    QDate m_currentDate;
    QDate m_disabledDate; // 连续失败自禁用所在日；跨天后自动恢复（设计 §3.5）
    int m_currentPart = 0;
    bool m_disabled = false;
    bool m_dayExhausted = false;
    bool m_inAppend = false;
    bool m_initialized = false;
    int m_consecutiveFailures = 0;
    int m_droppedRecords = 0;

    // 测试注入（生产为无效/默认）
    QString m_logDirOverride;
    QDateTime m_nowOverride;
    int m_enabledOverride = -1; // -1 = 跟随配置
    int m_maxFileBytesOverride = -1;
    int m_retentionDaysOverride = -1;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_AUDIT_LOG_H
