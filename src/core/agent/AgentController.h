#ifndef DEEPLUX_AGENT_CONTROLLER_H
#define DEEPLUX_AGENT_CONTROLLER_H

#include "ILLMClient.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QSet>
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

    void setLLMClient(ILLMClient* client);
    ILLMClient* llmClient() const {
        return m_llmClient;
    }
    void setSystemPrompt(const QString& prompt);
    QString systemPrompt() const {
        return m_systemPrompt;
    }

    void sendUserMessage(const QString& message);
    void sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images);
    void clearConversation();
    bool undoLastAgentAction();

    /**
     * @brief 中止当前轮（UI 停止按钮 / 外部 IPC）
     *
     * 幂等：Idle 下只记 warning；非 Idle 时作废排队任务与后续续跑、
     * 中止 LLM 请求与运行等待，回到 Idle 并发出 agentTurnAborted()。
     */
    void abortCurrentTurn();
    /// 中止代号（每次用户消息/中止 +1；测试断言用）
    quint64 turnGeneration() const {
        return m_turnGeneration;
    }
    /// 最近一次 LLM 分类错误的类别名（"Auth"/"RateLimit"/...；空 = 无）；UI 审计用
    QString lastLLMErrorCategory() const {
        return m_lastErrorCategory;
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
    void confirmPendingTools();
    void rejectPendingTools();

    // 测试/调试支持：只读访问对话历史大小
    int conversationHistorySize() const {
        return m_conversationHistory.size();
    }

signals:
    void actionLogEntryAdded(const AgentActionLogEntry& entry);
    void permissionLevelChanged(PermissionLevel level);
    void agentActionReceived(const QString& action, const QJsonObject& params);
    void agentActionCompleted(const QString& action, const QJsonObject& result);
    void llmResponseReceived(const QString& content, const QJsonArray& toolCalls);
    void llmErrorOccurred(const QString& error);
    void llmUsageReported(int promptTokens, int completionTokens);
    void toolsPendingConfirmation(const QJsonArray& toolCalls);
    void agentLoopIterating();
    /// 状态机迁移（UI：状态条文案 / 停止按钮可见性 / 面板执行态）
    void agentStateChanged(AgentState state);
    /// 本轮被用户中止（UI 复位状态条与思考态）
    void agentTurnAborted();

    // ---- 流式（设计 §5.1；只增不改）----
    void agentStreamStarted();
    void agentStreamContentDelta(const QString& text);   // 已按 50ms 合批
    void agentStreamReasoningDelta(const QString& text); // 已按 50ms 合批
    void agentStreamFinished();
    void agentStreamAborted();
    void agentStreamFallback(const QString& reason);
    void agentStreamTruncated(); // finish_reason=length（UI 状态条提示）

DEEPLUX_TEST_VISIBILITY slots : void onLLMResponse(const struct AgentResponse& resp);
    void onLLMError(const QString& error);
    // 分类错误（最终失败）：只落审计日志，状态机仍由 onLLMError 收口
    void onLLMErrorClassified(const struct LLMErrorInfo& info);
    void doConfirmPendingTools(QJsonArray calls);
    void onStreamStarted();
    void onStreamContentDelta(const QString& text);
    void onStreamReasoningDelta(const QString& text);
    void onStreamFinished();
    void onStreamFailed();
    void onStreamFallback(const QString& reason);

    DEEPLUX_TEST_VISIBILITY : explicit AgentController(QObject* parent = nullptr);
    ~AgentController() override;

    QString buildContext();
    void extendAgentLoop(const QJsonArray& toolCalls);
    void trimHistoryIfNeeded();

    /// 把本批工具结果里的 imagePath 转成下一轮多模态输入或文本降级说明（设计 §6.4）
    void attachToolImages(const QJsonArray& results);
    /// 视觉降级重发：去掉最近一条带图 user 消息的 images（无带图消息返回 false）
    bool stripImagesFromLastUserMessage();

    // ---- 流式合批（设计 §5.2；唯一合批点）----
    void flushStreamPending();
    void resetStreamBuffer();

    // 权限矩阵的测试入口：转调唯一的 requiresConfirmation 实现（设计 §3.5 第 10 条）
    static bool toolRequiresConfirmation(PermissionLevel level, const QString& toolName);

    // ---- 上下文治理（设计 §7/§8）：唯一配置读取点 + 预算/阈值访问器 ----
    int contextBudgetTokens() const;     // agent/contextBudgetTokens 默认 128000，clamp [16000,1048576]
    int toolResultMaxChars() const;      // agent/toolResultMaxChars 默认 6000，clamp [1000,64000]
    int flowSummaryThreshold() const;    // agent/flowSummaryThreshold 默认 24，clamp [1,1000]
    int flowSummaryPreviewCount() const; // agent/flowSummaryPreviewCount 默认 24，clamp [1,200]
    /// 测试用固定上限（-1 = 沿用配置；避免依赖 ConfigManager 初始化时序）
    DEEPLUX_TEST_VISIBILITY : void setContextLimitsForTest(int budgetTokens, int toolResultMaxChars,
                                                           int summaryThreshold, int summaryPreviewCount);

    /// 保首尾的 tool 结果截断（设计 §7.4；maxChars 会 clamp 到 [1000,64000]）
    static QString truncateToolResult(const QString& json, int maxChars);

    // ---- R1/L9：非流式 reasoning 只读访问器 + 单轮全局 deadline ----
    QString lastResponseReasoning() const {
        return m_lastResponseReasoning;
    }
    int turnDeadlineMs() const; // agent/turnDeadlineMs 默认 600000，clamp [10000,3600000]
    DEEPLUX_TEST_VISIBILITY : void setTurnDeadlineMsForTest(int ms); // 绕过 clamp，仅用于触发超时
    void setTurnDeadlineConfigForTest(int ms);                       // 模拟配置值（走 clamp）
    bool turnDeadlineExceeded() const;
    void endTurnWithReason(const QString& responseText, const QString& errorText, bool suppressClientError);

    bool m_initialized = false;
    PermissionLevel m_permissionLevel = PermissionLevel::Advisor;

    AgentObserver* m_observer = nullptr;
    AgentActor* m_actor = nullptr;
    QPointer<ILLMClient> m_llmClient;

    QString m_systemPrompt;
    QList<AgentMessage> m_conversationHistory;
    QJsonArray m_pendingToolCalls;
    bool m_continueAfterPendingTools = true;

    int m_agentTurnCount = 0;
    AgentState m_state = AgentState::Idle;
    mutable QMutex m_stateMutex;

    // 异步执行与取消（设计 §5）：中止代号 + 排队连点保护 + 视觉降级状态
    quint64 m_turnGeneration = 0;
    bool m_dispatchQueued = false;
    bool m_queuedContinueAfterPending = true;
    bool m_visionRetryDone = false;
    bool m_visionDisabledForSession = false;
    bool m_suppressNextLlmError = false;
    QString m_lastErrorCategory;

    // ---- 流式合批状态（设计 §5.2/§5.6）：唯一合批点在控制器
    QTimer* m_streamFlushTimer = nullptr;
    QString m_pendingContent;
    QString m_pendingReasoning;
    bool m_streamActive = false;

    // ---- R1/L9：最近一次响应 reasoning + 单轮 deadline 状态 ----
    QString m_lastResponseReasoning;
    QElapsedTimer m_turnTimer;
    int m_turnDeadlineOverride = -1;       // 测试触发值（绕过 clamp）
    int m_turnDeadlineConfigOverride = -1; // 模拟配置值（走 clamp）

    // ---- 工程会话身份/版本（B2：确认有效期；内部递增，不以指针/路径为唯一身份）----
    quint64 m_projectSessionId = 0;
    quint64 m_projectRevision = 0;
    quint64 m_confirmationSessionId = 0;
    quint64 m_confirmationRevision = 0;
    QList<QMetaObject::Connection> m_projectConnections;

    // ---- 上下文治理（设计 §7）：测试覆盖值（-1 = 沿用配置）----
    int m_contextBudgetOverride = -1;
    int m_toolResultMaxCharsOverride = -1;
    int m_summaryThresholdOverride = -1;
    int m_summaryPreviewOverride = -1;

    // ---- 权限/确认有效期（集成收口 B2）----
    void bindProjectSignals(Project* project);
    void onProjectStructureChanged();
    void snapshotConfirmation();
    bool confirmationExpired() const;
    void expirePendingConfirmation(const QString& reason);
    /// Observer 混合批次：只读逐项执行、写入拒绝；返回 true 表示存在拒绝项
    bool executeObserverCalls(const QJsonArray& calls, bool continueAfter);

    // ---- 上下文保护与发送前校验（集成收口 B3）----
    /// 保护集合：当前真实用户请求、在途调用组、最近完整工具组（含其后的自动回灌消息）
    QSet<int> protectedHistoryIndices() const;
    /// 发送前配对校验：每个 assistant(tool_calls) 与 tool 结果恰好一一配对
    bool historyPairsValid(QString* error) const;
    /// 唯一的发送入口：裁剪→配对校验→预算校验→sendRequest
    bool sendPreparedContext();
    void failContextSend(const QString& code, const QString& detail);
    qint64 estimateTokensFor(const QList<AgentMessage>& messages, const QString& systemPrompt,
                             bool countReasoning) const;

    // 工具组原子裁剪辅助（设计 §7.3；全部只在 trimHistoryIfNeeded 内使用）
    void repairOrphanToolGroups();
    bool dropOldestUserTurn();
    bool dropOldestToolGroup();
    qint64 estimateHistoryTokens();
    bool echoesReasoning() const; // reasoning 仅在回传时计入 token 预算（§7.1）

    void transitionTo(AgentState newState);
    bool tryTransitionTo(AgentState expected, AgentState newState);
    static QString stateName(AgentState state);
    static QString defaultSystemPrompt();
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CONTROLLER_H
