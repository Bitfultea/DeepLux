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
struct AgentMessage;

class AgentController : public QObject
{
    Q_OBJECT

public:
    static AgentController& instance();

    bool initialize();
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    enum class PermissionLevel { Observer, Advisor, Autopilot };
    PermissionLevel permissionLevel() const { return m_permissionLevel; }
    void setPermissionLevel(PermissionLevel level);

    enum class AgentState { Idle, Thinking, Confirming, Executing };
    AgentState state() const { return m_state; }

    void onGuiEvent(const GuiEvent& event);
    QJsonObject handleToolCall(const QString& toolName, const QJsonObject& params);

    void setLLMClient(ILLMClient* client);
    ILLMClient* llmClient() const { return m_llmClient; }

    void sendUserMessage(const QString& message);
    void sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images);
    void clearConversation();

    void logAction(const AgentActionLogEntry& entry);
    AgentActor* actor() const { return m_actor; }

    bool hasPendingTools() const { return !m_pendingToolCalls.isEmpty(); }
    QJsonArray pendingToolCalls() const { return m_pendingToolCalls; }
    void confirmPendingTools();
    void rejectPendingTools();

    // 测试/调试支持：只读访问对话历史大小
    int conversationHistorySize() const { return m_conversationHistory.size(); }

signals:
    void actionLogEntryAdded(const AgentActionLogEntry& entry);
    void permissionLevelChanged(PermissionLevel level);
    void agentActionReceived(const QString& action, const QJsonObject& params);
    void agentActionCompleted(const QString& action, const QJsonObject& result);
    void llmResponseReceived(const QString& content, const QJsonArray& toolCalls);
    void llmErrorOccurred(const QString& error);
    void toolsPendingConfirmation(const QJsonArray& toolCalls);
    void agentLoopIterating();

private slots:
    void onLLMResponse(const struct AgentResponse& resp);
    void onLLMError(const QString& error);
    void doConfirmPendingTools(QJsonArray calls);

private:
    explicit AgentController(QObject* parent = nullptr);
    ~AgentController() override;

    QString buildContext();
    void extendAgentLoop(const QJsonArray& toolCalls);
    void trimHistoryIfNeeded();

    bool m_initialized = false;
    PermissionLevel m_permissionLevel = PermissionLevel::Advisor;

    AgentObserver* m_observer = nullptr;
    AgentActor* m_actor = nullptr;
    ILLMClient* m_llmClient = nullptr;

    QString m_systemPrompt;
    QList<AgentMessage> m_conversationHistory;
    QJsonArray m_pendingToolCalls;

    int m_agentTurnCount = 0;
    AgentState m_state = AgentState::Idle;

    void transitionTo(AgentState newState);
    static QString stateName(AgentState state);
    static QString defaultSystemPrompt();
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CONTROLLER_H
