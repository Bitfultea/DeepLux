#ifndef DEEPLUX_AGENT_CONTROLLER_H
#define DEEPLUX_AGENT_CONTROLLER_H

#include "ILLMClient.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <functional>
#include <memory>

class QTimer;

namespace DeepLux {

class AgentObserver;
class AgentActor;
class Project;
struct GuiEvent;
struct AgentActionLogEntry;

class DEEPLUX_API AgentController : public QObject {
    Q_OBJECT

public:
    static AgentController& instance();

    // 批3 复核五轮（P2-4）：图像附件上限——与 AgentChatPanel::kMaxImageAttachments
    // 为同一契约（测试断言两者相等）；UI 源头限额之外，控制器在
    // sendUserMessageWithImages 参数校验阶段显式拒绝并给出用户可见反馈
    static constexpr int kMaxImageAttachments = 4;

    bool initialize();
    void shutdown();
    bool isInitialized() const {
        return m_initialized;
    }

    enum class PermissionLevel { Observer, Advisor, Autopilot };
    PermissionLevel permissionLevel() const {
        return m_permissionLevel;
    }
    void setPermissionLevel(PermissionLevel level);

    enum class AgentState { Idle, Thinking, Confirming, Executing };
    AgentState state() const {
        QMutexLocker locker(&m_stateMutex);
        return m_state;
    }

    void onGuiEvent(const GuiEvent& event);
    QJsonObject handleToolCall(const QString& toolName, const QJsonObject& params);

    /**
     * @brief 阶段1 §2：统一入口权限判定——聊天路径、Bridge handleToolCall、
     * 确认执行前复验三处共用，杜绝策略分叉（F08）。
     */
    enum class ToolGate { Allow, RequireConfirmation, Deny };
    ToolGate evaluateToolGate(const QString& toolName, const QJsonObject& params = {}) const;
    static bool toolRequiresConfirmation(PermissionLevel level, const QString& toolName);

    /**
     * @brief 阶段1 §4：当前待确认批次的完整载荷（confirmationId + tools + context
     * 摘要/工程/版本快照/副作用/可撤销性），供 UI 展示与审计；无待确认批次时返回空对象。
     */
    QJsonObject pendingConfirmationPayload() const;

    void setLLMClient(ILLMClient* client);
    using DataSourceViewHandler = std::function<QJsonObject(const QString&, const QJsonObject&)>;
    void registerDataSourceViewHandler(QObject* owner, DataSourceViewHandler handler);
    void unregisterDataSourceViewHandler(QObject* owner);
    QJsonObject executeDataSourceViewAction(const QString& toolName, const QJsonObject& params) const;
    QJsonObject confirmedActiveAnnotationSessionSaveSnapshot(const QString& outputPath) const;
    QJsonObject confirmedActiveAnnotationSessionOpenSnapshot(const QString& sessionPath) const;
    ILLMClient* llmClient() const {
        return m_llmClient;
    }
    void setSystemPrompt(const QString& prompt);
    QString systemPrompt() const {
        return m_systemPrompt;
    }

    void sendUserMessage(const QString& message);
    void sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images);
    bool cancelCurrentRequest();
    void clearConversation();
    bool undoLastAgentAction();
    void abortCurrentTurn();
    quint64 turnGeneration() const {
        return m_turnGeneration;
    }
    QString lastLLMErrorCategory() const {
        return m_lastErrorCategory;
    }
    QString lastResponseReasoning() const {
        return m_lastResponseReasoning;
    }

    void logAction(const AgentActionLogEntry& entry);
    AgentActor* actor() const {
        return m_actor;
    }

    bool hasPendingTools() const {
        return !m_pendingToolCalls.isEmpty();
    }
    QJsonArray pendingToolCalls() const {
        return m_pendingToolCalls;
    }
    QString pendingConfirmationId() const {
        return m_pendingConfirmationId;
    }

    /**
     * @brief 批准/拒绝当前待确认批次（阶段1 审核整改 R2;复审2 R1 收口）。
     *
     * confirmationId 为必填且必须与当前待确认批次一致——批准必须绑定到用户
     * 所看到的那一批操作。空 id 或错配（旧批次迟到事件）一律拒绝：返回 false,
     * 零副作用,当前批次保持待确认。
     */
    bool confirmPendingTools(const QString& confirmationId);
    bool rejectPendingTools(const QString& confirmationId);

    // 测试/调试支持：只读访问对话历史大小
    int conversationHistorySize() const {
        return m_conversationHistory.size();
    }
    const QList<AgentMessage>& conversationHistory() const {
        return m_conversationHistory;
    }

signals:
    void actionLogEntryAdded(const AgentActionLogEntry& entry);
    void undoRejected(const QString& reason);
    void permissionLevelChanged(PermissionLevel level);
    void agentActionReceived(const QString& action, const QJsonObject& params);
    void agentActionCompleted(const QString& action, const QJsonObject& result);
    void llmResponseReceived(const QString& content, const QJsonArray& toolCalls);
    void llmErrorOccurred(const QString& error);
    void toolsPendingConfirmation(const QJsonArray& toolCalls);
    void agentLoopIterating();
    void llmUsageReported(int promptTokens, int completionTokens);
    void agentStateChanged(AgentState state);
    void agentTurnAborted();
    void agentStreamStarted();
    void agentStreamContentDelta(const QString& text);
    void agentStreamReasoningDelta(const QString& text);
    void agentStreamFinished();
    void agentStreamAborted();
    void agentStreamFallback(const QString& reason);
    void agentStreamTruncated();

DEEPLUX_TEST_VISIBILITY slots : void onLLMResponse(const struct AgentResponse& resp);
    void onLLMError(const QString& error);
    void onLLMRequestFailed(const QString& requestId, const QString& error);
    void onLLMErrorClassified(const struct LLMErrorInfo& info);
    void onLLMErrorClassifiedForRequest(const QString& requestId, const struct LLMErrorInfo& info);
    QString finalizeTextReply(const QString& content) const;
    void doConfirmPendingTools(QJsonArray calls, const QString& confirmationId);
    void onStreamStarted();
    void onStreamContentDelta(const QString& text);
    void onStreamReasoningDelta(const QString& text);
    void onStreamFinished();
    void onStreamFailed();
    void onStreamFallback(const QString& reason);

    DEEPLUX_TEST_VISIBILITY : explicit AgentController(QObject* parent = nullptr);
    ~AgentController() override;

    QString buildContext();
    void extendAgentLoop(const QJsonArray& toolCalls, const QString& confirmationId = QString());
    void pollPendingQueries(int generation);
    void cancelPendingQueries(const QString& reason);
    void continueAgentLoop();
    void sendLLMRequest(AgentConversation ctx);
    void trimHistoryIfNeeded();
    void attachToolImages(const QJsonArray& results);
    bool stripImagesFromLastUserMessage();
    void flushStreamPending();
    void resetStreamBuffer();

    // 阶段1 §4/§5：确认批次绑定工程与配置版本快照；执行前重校验，陈旧确认零副作用拒绝
    void beginPendingConfirmation(const QJsonArray& calls, const QString& content, bool continueAfter);
    bool confirmationContextStillValid(const QJsonArray& calls, QString* reason) const;
    QJsonObject unresolvedFileTargetPreflight(const QString& toolName) const;
    QJsonObject cameraCyclePreflight(const QString& toolName, const QJsonObject& params) const;
    QJsonObject cameraSessionPreflight(const QString& toolName) const;
    QJsonObject cameraCycleBatchPreflight(const QJsonArray& calls) const;
    // 未执行/被拒绝的确认批次：为每个 call 补一条 tool 结果，保持 OpenAI 对话合法性
    void appendToolResultsForCalls(const QJsonArray& calls, const QJsonObject& result);

    // ---- 上下文治理（设计 §7/§8）：唯一配置读取点 + 预算/阈值访问器 ----
    int contextBudgetTokens() const;     // agent/contextBudgetTokens 默认 128000，clamp [16000,1048576]
    int toolResultMaxChars() const;      // agent/toolResultMaxChars 默认 6000，clamp [1000,64000]
    int flowSummaryThreshold() const;    // agent/flowSummaryThreshold 默认 24，clamp [1,1000]
    int flowSummaryPreviewCount() const; // agent/flowSummaryPreviewCount 默认 24，clamp [1,200]
    /// 测试用固定上限（-1 = 沿用配置；避免依赖 ConfigManager 初始化时序）
#ifdef DEEPLUX_BUILD_TESTS
    DEEPLUX_TEST_VISIBILITY : void setContextLimitsForTest(int budgetTokens, int toolResultMaxChars,
                                                           int summaryThreshold, int summaryPreviewCount);
#endif

    bool sendPreparedContext();
    void failContextSend(const QString& code, const QString& detail);
    void bindProjectSignals(Project* project);
    void onProjectStructureChanged();
    void snapshotConfirmation();
    bool confirmationExpired() const;
    void expirePendingConfirmation(const QString& reason);
    QSet<int> protectedHistoryIndices() const;
    bool historyPairsValid(QString* error) const;
    void repairOrphanToolGroups();
    bool dropOldestUserTurn();
    bool dropOldestToolGroup();
    qint64 estimateHistoryTokens();
    qint64 estimateTokensFor(const QList<AgentMessage>& messages, const QString& systemPrompt,
                             bool countReasoning) const;
    bool echoesReasoning() const;

public:
    /// 保首尾的 tool 结果截断（设计 §7.4；maxChars 会 clamp 到 [1000,64000]）
    static QString truncateToolResult(const QString& json, int maxChars);

    // ---- R1/L9：非流式 reasoning 只读访问器 + 单轮全局 deadline ----
    int turnDeadlineMs() const; // agent/turnDeadlineMs 默认 600000，clamp [10000,3600000]
    // 绕过 clamp，仅用于触发超时。
#ifdef DEEPLUX_BUILD_TESTS
    DEEPLUX_TEST_VISIBILITY : void setTurnDeadlineMsForTest(int ms);
    DEEPLUX_TEST_VISIBILITY : void setTurnDeadlineConfigForTest(int ms); // 模拟配置值（走 clamp）
#endif
    DEEPLUX_TEST_VISIBILITY : bool turnDeadlineExceeded() const;
    DEEPLUX_TEST_VISIBILITY : void endTurnWithReason(const QString& responseText, const QString& errorText,
                                                     bool suppressClientError);

    DEEPLUX_TEST_VISIBILITY : bool m_initialized = false;
    PermissionLevel m_permissionLevel = PermissionLevel::Advisor;

    AgentObserver* m_observer = nullptr;
    AgentActor* m_actor = nullptr;
    QPointer<ILLMClient> m_llmClient;

    QString m_systemPrompt;
    QList<AgentMessage> m_conversationHistory;
    QJsonArray m_pendingToolCalls;
    bool m_continueAfterPendingTools = true;

    // 待确认批次上下文快照（阶段1 §4）
    QString m_pendingConfirmationId;
    bool m_pendingHasProject = false;
    QString m_pendingProjectPath;
    QString m_pendingProjectName;
    QByteArray m_pendingProjectFingerprint; // 工程内容 sha1，检出任何来源的窗口期修改
    bool m_pendingDataSourceEpochCaptured = false;
    quint64 m_pendingDataSourceProjectIdentity = 0;
    quint64 m_pendingDataSourceGeneration = 0;
    bool m_pendingDataSourceViewOwnerCaptured = false;
    QPointer<QObject> m_pendingDataSourceViewOwner;
    quintptr m_pendingDataSourceViewOwnerIdentity = 0;
    QJsonObject m_pendingFileTargetPaths; // module:<instance ID>/call:<tool call ID> -> resolved output target
    bool m_pendingFileTargetsCaptured = false;
    int m_pendingUndoIndex = -1;
    int m_pendingUndoCount = -1;
    bool m_pendingResumeSnapshotValid = false;
    QString m_pendingResumeRunId;
    QString m_pendingResumeSessionId;
    int m_pendingResumeState = 0;
    int m_pendingResumeLifecycleGeneration = 0;

    QPointer<QObject> m_dataSourceViewOwner;
    DataSourceViewHandler m_dataSourceViewHandler;
    mutable QJsonObject m_lastQuickMeasurementStatus;

    quint64 m_turnGeneration = 0;
    bool m_dispatchQueued = false;
    bool m_queuedContinueAfterPending = true;
    bool m_visionRetryDone = false;
    bool m_visionDisabledForSession = false;
    bool m_suppressNextLlmError = false;
    QString m_lastErrorCategory;
    QTimer* m_streamFlushTimer = nullptr;
    QString m_pendingContent;
    QString m_pendingReasoning;
    bool m_streamActive = false;
    QString m_lastResponseReasoning;
    QElapsedTimer m_turnTimer;
#ifdef DEEPLUX_BUILD_TESTS
    int m_turnDeadlineOverride = -1;       // 测试触发值（绕过 clamp）
    int m_turnDeadlineConfigOverride = -1; // 模拟配置值（走 clamp）
#endif

    // ---- 工程会话身份/版本（B2：确认有效期；内部递增，不以指针/路径为唯一身份）----
    quint64 m_projectSessionId = 0;
    quint64 m_projectRevision = 0;
    quint64 m_confirmationSessionId = 0;
    quint64 m_confirmationRevision = 0;
    QList<QMetaObject::Connection> m_projectConnections;

    // ---- 上下文治理（设计 §7）：测试覆盖值（-1 = 沿用配置）----
#ifdef DEEPLUX_BUILD_TESTS
    int m_contextBudgetOverride = -1;
    int m_toolResultMaxCharsOverride = -1;
    int m_summaryThresholdOverride = -1;
    int m_summaryPreviewOverride = -1;
#endif

    int m_agentTurnCount = 0;
    int m_parameterCorrectionRetries = 0;
    QSet<QString> m_parameterFailureSignatures;
    struct PendingQuery {
        QString callId;
        QString name;
        QJsonObject params;
    };
    QList<PendingQuery> m_pendingQueries;
    QDateTime m_pendingQueryDeadline;
    int m_waitGeneration = 0;
    QString m_activeLLMRequestId;
    AgentState m_state = AgentState::Idle;
    mutable QMutex m_stateMutex;

    void transitionTo(AgentState newState);
    bool tryTransitionTo(AgentState expected, AgentState newState);
    static QString stateName(AgentState state);
    static QString defaultSystemPrompt();
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CONTROLLER_H
