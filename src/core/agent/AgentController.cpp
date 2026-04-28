#include "AgentController.h"
#include "AgentObserver.h"
#include "AgentActor.h"
#include "GuiEvent.h"
#include "AgentBridge.h"
#include "ToolSchema.h"
#include "ILLMClient.h"
#include "common/Logger.h"
#include "manager/ConfigManager.h"

#include <QJsonDocument>
#include <QPixmap>
#include <QBuffer>

#include <QDebug>

namespace DeepLux {

AgentController::AgentController(QObject* parent)
    : QObject(parent)
    , m_observer(new AgentObserver(this))
    , m_actor(new AgentActor(this))
{
}

AgentController::~AgentController() = default;

AgentController& AgentController::instance()
{
    static AgentController instance;
    return instance;
}

bool AgentController::initialize()
{
    if (m_initialized) return true;

    ToolSchema::instance().registerDefaultTools();

    if (!m_observer->initialize()) {
        qWarning() << "AgentController: Failed to initialize AgentObserver";
        return false;
    }

    connect(m_observer, &AgentObserver::guiEventOccurred,
            this, &AgentController::onGuiEvent);

    AgentBridge::instance().setToolCallCallback(
        [this](const QString& toolName, const QJsonObject& params) -> QJsonObject {
            return this->handleToolCall(toolName, params);
        });

    connect(m_actor, &AgentActor::toolExecuted,
            this, [this](const QString& tool, const QJsonObject& result) {
                emit agentActionCompleted(tool, result);
            });
    connect(m_actor, &AgentActor::toolError,
            this, [this](const QString& tool, const QString& error) {
                QJsonObject r;
                r["error"] = error;
                emit agentActionCompleted(tool, r);
            });

    // Load system prompt from config or use default
    ConfigManager& cfg = ConfigManager::instance();
    m_systemPrompt = cfg.groupString("agent", "systemPrompt", defaultSystemPrompt());

    m_initialized = true;
    return true;
}

void AgentController::shutdown()
{
    if (!m_initialized) return;

    AgentBridge::instance().setToolCallCallback(nullptr);

    if (m_observer) m_observer->shutdown();
    if (m_llmClient) {
        disconnect(m_llmClient, nullptr, this, nullptr);
    }

    Logger::instance().addLog("AgentController shutdown", LogLevel::Info, "Agent");
    m_initialized = false;
}

void AgentController::setPermissionLevel(PermissionLevel level)
{
    if (m_permissionLevel == level) return;
    m_permissionLevel = level;
    emit permissionLevelChanged(level);

    QString levelStr;
    switch (level) {
    case PermissionLevel::Observer:   levelStr = "Observer"; break;
    case PermissionLevel::Advisor:    levelStr = "Advisor"; break;
    case PermissionLevel::Autopilot:  levelStr = "Autopilot"; break;
    }
    qDebug() << "Agent permission changed to:" << levelStr;
}

void AgentController::setLLMClient(ILLMClient* client)
{
    if (m_llmClient) {
        disconnect(m_llmClient, nullptr, this, nullptr);
    }
    m_llmClient = client;
    if (m_llmClient) {
        connect(m_llmClient, &ILLMClient::responseReceived,
                this, &AgentController::onLLMResponse);
        connect(m_llmClient, &ILLMClient::errorOccurred,
                this, &AgentController::onLLMError);
    }
}

void AgentController::logAction(const AgentActionLogEntry& entry)
{
    emit actionLogEntryAdded(entry);
}

void AgentController::onGuiEvent(const GuiEvent& event)
{
    Logger::instance().addLog(
        QString("[AgentObserver] %1 from %2").arg(event.typeString()).arg(event.source),
        LogLevel::Debug, "Agent");
}

QJsonObject AgentController::handleToolCall(const QString& toolName, const QJsonObject& params)
{
    Logger::instance().addLog(
        QString("[ToolCall] %1 (permission=%2)").arg(toolName).arg(static_cast<int>(m_permissionLevel)),
        LogLevel::Info, "Agent");

    emit agentActionReceived(toolName, params);

    if (m_permissionLevel == PermissionLevel::Observer) {
        return QJsonObject{{"error", "Permission denied: current level is Observer (read-only)"}};
    }

    if (m_permissionLevel == PermissionLevel::Advisor) {
        Logger::instance().addLog(
            QString("[Advisor] Executing %1 with user confirmation").arg(toolName),
            LogLevel::Warning, "Agent");
    }

    QJsonObject result = m_actor->executeTool(toolName, params);

    AgentActionLogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.actor = "Agent";
    entry.action = toolName;
    entry.params = QString(QJsonDocument(params).toJson(QJsonDocument::Compact));
    entry.result = result.contains("error") ? "error" : "success";
    emit actionLogEntryAdded(entry);

    return result;
}

void AgentController::sendUserMessage(const QString& message)
{
    if (!m_llmClient) {
        emit llmErrorOccurred("LLM client not configured");
        return;
    }

    AgentConversation ctx;
    ctx.messages.append({"user", message, QJsonObject(), QString()});
    ctx.systemPrompt = m_systemPrompt;

    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

void AgentController::sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images)
{
    if (!m_llmClient) {
        emit llmErrorOccurred("LLM client not configured");
        return;
    }

    AgentMessage msg;
    msg.role = "user";
    msg.content = message;

    for (const QPixmap& pixmap : images) {
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        pixmap.save(&buffer, "PNG");
        buffer.close();

        AgentImageAttachment attach;
        attach.data = data;
        attach.mimeType = "image/png";
        attach.description = "User uploaded image";
        msg.images.append(attach);
    }

    AgentConversation ctx;
    ctx.messages.append(msg);
    ctx.systemPrompt = m_systemPrompt;

    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

void AgentController::onLLMResponse(const AgentResponse& resp)
{
    if (!resp.toolCalls.isEmpty()) {
        if (m_permissionLevel == PermissionLevel::Observer) {
            emit llmResponseReceived("Permission denied: Observer mode cannot execute tools.", QJsonArray());
            return;
        }

        if (m_permissionLevel == PermissionLevel::Advisor) {
            m_pendingToolCalls = resp.toolCalls;
            emit toolsPendingConfirmation(resp.toolCalls);
            // Don't emit llmResponseReceived yet; wait for user confirmation
            return;
        }

        // Autopilot: execute directly
        executePendingTools(resp.toolCalls);
        return;
    }

    emit llmResponseReceived(resp.content, resp.toolCalls);
}

void AgentController::executePendingTools(const QJsonArray& toolCalls)
{
    QList<QPair<QString, QJsonObject>> tools;
    for (const QJsonValue& v : toolCalls) {
        QJsonObject tc = v.toObject();
        QString name = tc["name"].toString();
        QJsonObject params = tc["arguments"].toObject();
        if (name.isEmpty()) {
            name = tc["function"].toObject()["name"].toString();
            params = QJsonDocument::fromJson(
                tc["function"].toObject()["arguments"].toString().toUtf8()).object();
        }
        tools.append(qMakePair(name, params));
    }

    if (!tools.isEmpty()) {
        QJsonObject result = m_actor->executeTools(tools, "Agent batch execution");
        emit llmResponseReceived("Tools executed successfully.", QJsonArray());
    }
}

void AgentController::confirmPendingTools()
{
    if (m_pendingToolCalls.isEmpty()) return;

    executePendingTools(m_pendingToolCalls);
    m_pendingToolCalls = QJsonArray();
}

void AgentController::rejectPendingTools()
{
    m_pendingToolCalls = QJsonArray();
    emit llmResponseReceived("Tool execution cancelled by user.", QJsonArray());
}

void AgentController::onLLMError(const QString& error)
{
    emit llmErrorOccurred(error);
}

QString AgentController::defaultSystemPrompt()
{
    return QString(
        "You are DeepLux Agent, an AI assistant embedded in DeepLux Vision "
        "(an industrial machine vision software).\n\n"
        "Your capabilities:\n"
        "- Create and manage vision inspection projects\n"
        "- Add/remove/configure image processing modules (e.g., GrabImage, FindCircle, MeasureLine)\n"
        "- Connect modules into execution flows\n"
        "- Run flows and interpret results\n"
        "- Answer questions about the current project state\n\n"
        "Available tools: create_project, add_module, remove_module, set_param, "
        "connect_modules, disconnect_modules, run_flow, stop_flow, "
        "get_flow_state, get_available_plugins, save_project.\n\n"
        "Rules:\n"
        "1. Always use tools to make changes, never describe pseudo-code.\n"
        "2. Before modifying, check current state with get_flow_state if needed.\n"
        "3. Be concise. Industrial users prefer direct answers.\n"
        "4. When user pastes an image, analyze it visually if possible.\n"
        "5. Dangerous operations (remove_module, save_project) require user confirmation in Advisor mode.\n"
    );
}

} // namespace DeepLux
