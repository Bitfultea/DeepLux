#ifndef DEEPLUX_AGENT_CONTROLLER_H
#define DEEPLUX_AGENT_CONTROLLER_H

#include "ILLMClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <memory>

namespace DeepLux {

class AgentObserver;
class AgentActor;
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

DEEPLUX_TEST_VISIBILITY slots : void onLLMResponse(const struct AgentResponse& resp);
    void onLLMError(const QString& error);
    // 分类错误（最终失败）：只落审计日志，状态机仍由 onLLMError 收口
    void onLLMErrorClassified(const struct LLMErrorInfo& info);
    void doConfirmPendingTools(QJsonArray calls);

    DEEPLUX_TEST_VISIBILITY : explicit AgentController(QObject* parent = nullptr);
    ~AgentController() override;

    QString buildContext();
    void extendAgentLoop(const QJsonArray& toolCalls);
    void trimHistoryIfNeeded();

    /// 把本批工具结果里的 imagePath 转成下一轮多模态输入或文本降级说明（设计 §6.4）
    void attachToolImages(const QJsonArray& results);
    /// 视觉降级重发：去掉最近一条带图 user 消息的 images（无带图消息返回 false）
    bool stripImagesFromLastUserMessage();

    // 权限矩阵的测试入口：转调唯一的 requiresConfirmation 实现（设计 §3.5 第 10 条）
    static bool toolRequiresConfirmation(PermissionLevel level, const QString& toolName);

    bool m_initialized = false;
    PermissionLevel m_permissionLevel = PermissionLevel::Advisor;

    AgentObserver* m_observer = nullptr;
    AgentActor* m_actor = nullptr;
    ILLMClient* m_llmClient = nullptr;

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

    void transitionTo(AgentState newState);
    bool tryTransitionTo(AgentState expected, AgentState newState);
    static QString stateName(AgentState state);
    static QString defaultSystemPrompt();
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CONTROLLER_H
