#ifndef DEEPLUX_AGENT_CONTROLLER_H
#define DEEPLUX_AGENT_CONTROLLER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <memory>

namespace DeepLux {

class AgentObserver;
class AgentActor;
class ILLMClient;
struct GuiEvent;
struct AgentActionLogEntry;

/**
 * @brief Agent 核心协调器 —— 进程内单例
 *
 * 职责：
 * - 持有 AgentObserver、AgentActor、ILLMClient
 * - 接收 GUI 事件流，转发给 LLM Client
 * - 接收外部 tool_call（通过 AgentBridge），路由给 AgentActor
 * - 管理权限级别
 *
 * 设计约束：
 * - 只能操作本软件，不能执行 bash/系统命令
 * - 所有 Agent 操作必须通过日志可观察
 */
class AgentController : public QObject
{
    Q_OBJECT

public:
    static AgentController& instance();

    bool initialize();
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    enum class PermissionLevel
    {
        Observer,
        Advisor,
        Autopilot
    };
    PermissionLevel permissionLevel() const { return m_permissionLevel; }
    void setPermissionLevel(PermissionLevel level);

    void onGuiEvent(const GuiEvent& event);
    QJsonObject handleToolCall(const QString& toolName, const QJsonObject& params);

    // LLM Client 管理
    void setLLMClient(ILLMClient* client);
    ILLMClient* llmClient() const { return m_llmClient; }

    // 发送用户消息到 LLM（由 ChatPanel 调用）
    void sendUserMessage(const QString& message);
    void sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images);

    void logAction(const AgentActionLogEntry& entry);
    AgentActor* actor() const { return m_actor; }

    // Advisor mode: pending tool confirmation
    bool hasPendingTools() const { return !m_pendingToolCalls.isEmpty(); }
    QJsonArray pendingToolCalls() const { return m_pendingToolCalls; }
    void confirmPendingTools();
    void rejectPendingTools();

private:
    void executePendingTools(const QJsonArray& toolCalls);

signals:
    void actionLogEntryAdded(const AgentActionLogEntry& entry);
    void permissionLevelChanged(PermissionLevel level);
    void agentActionReceived(const QString& action, const QJsonObject& params);
    void agentActionCompleted(const QString& action, const QJsonObject& result);
    void llmResponseReceived(const QString& content, const QJsonArray& toolCalls);
    void llmErrorOccurred(const QString& error);
    void toolsPendingConfirmation(const QJsonArray& toolCalls);

private slots:
    void onLLMResponse(const struct AgentResponse& resp);
    void onLLMError(const QString& error);

private:
    explicit AgentController(QObject* parent = nullptr);
    ~AgentController() override;

    bool m_initialized = false;
    PermissionLevel m_permissionLevel = PermissionLevel::Advisor;

    AgentObserver* m_observer = nullptr;
    AgentActor* m_actor = nullptr;
    ILLMClient* m_llmClient = nullptr;

    QString m_systemPrompt;
    QList<struct AgentMessage> m_conversationHistory;
    QJsonArray m_pendingToolCalls;

    static QString defaultSystemPrompt();
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CONTROLLER_H
