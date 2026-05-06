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
    void extendAgentLoop(const QJsonArray& toolCalls);  // 内部实现
    void trimHistory();

    bool m_initialized = false;
    PermissionLevel m_permissionLevel = PermissionLevel::Advisor;

    AgentObserver* m_observer = nullptr;
    AgentActor* m_actor = nullptr;
    ILLMClient* m_llmClient = nullptr;

    QString m_systemPrompt;
    QList<AgentMessage> m_conversationHistory;
    QJsonArray m_pendingToolCalls;

    int m_agentTurnCount = 0;
    bool m_agentBusy = false;  // 防止并发 loop

    static constexpr int MAX_AGENT_TURNS = 5;
    static constexpr int MAX_HISTORY_SIZE = 15;

    static QString defaultSystemPrompt();
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CONTROLLER_H
