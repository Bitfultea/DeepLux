#include "AgentController.h"
#include "AgentObserver.h"
#include "AgentActor.h"
#include "GuiEvent.h"
#include "AgentBridge.h"
#include "ToolSchema.h"
#include "ILLMClient.h"
#include "common/Logger.h"
#include "manager/ConfigManager.h"
#include "manager/ProjectManager.h"
#include "engine/RunEngine.h"
#include "model/Project.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QPixmap>
#include <QBuffer>

#include <QDebug>

namespace DeepLux {

AgentController::AgentController(QObject* parent)
    : QObject(parent)
    , m_observer(new AgentObserver(this))
    , m_actor(new AgentActor(this))
{}

AgentController::~AgentController() = default;

AgentController& AgentController::instance() {
    static AgentController instance;
    return instance;
}

bool AgentController::initialize() {
    if (m_initialized) return true;
    ToolSchema::instance().registerDefaultTools();
    if (!m_observer->initialize()) {
        qWarning() << "AgentController: Failed to initialize AgentObserver";
        return false;
    }
    connect(m_observer, &AgentObserver::guiEventOccurred, this, &AgentController::onGuiEvent);
    AgentBridge::instance().setToolCallCallback(
        [this](const QString& n, const QJsonObject& p) { return handleToolCall(n, p); });
    connect(m_actor, &AgentActor::toolExecuted, this, [this](const QString& t, const QJsonObject& r) {
        emit agentActionCompleted(t, r); });
    connect(m_actor, &AgentActor::toolError, this, [this](const QString& t, const QString& e) {
        emit agentActionCompleted(t, {{"error", e}}); });
    m_systemPrompt = ConfigManager::instance().groupString("agent", "systemPrompt", defaultSystemPrompt());
    m_initialized = true;
    return true;
}

void AgentController::shutdown() {
    if (!m_initialized) return;
    AgentBridge::instance().setToolCallCallback(nullptr);
    if (m_observer) m_observer->shutdown();
    if (m_llmClient) disconnect(m_llmClient, nullptr, this, nullptr);
    m_initialized = false;
}

void AgentController::setPermissionLevel(PermissionLevel level) {
    if (m_permissionLevel == level) return;
    m_permissionLevel = level;
    emit permissionLevelChanged(level);
}

void AgentController::setLLMClient(ILLMClient* client) {
    if (m_llmClient) disconnect(m_llmClient, nullptr, this, nullptr);
    m_llmClient = client;
    if (m_llmClient) {
        connect(m_llmClient, &ILLMClient::responseReceived, this, &AgentController::onLLMResponse);
        connect(m_llmClient, &ILLMClient::errorOccurred, this, &AgentController::onLLMError);
    }
}

void AgentController::logAction(const AgentActionLogEntry& entry) { emit actionLogEntryAdded(entry); }
void AgentController::clearConversation() {
    m_conversationHistory.clear();
    m_pendingToolCalls = QJsonArray();
    m_agentTurnCount = 0;
}

// ========== Context ==========

QString AgentController::buildContext() {
    QString ctx = m_systemPrompt;
    Project* proj = ProjectManager::instance().currentProject();
    if (proj) {
        ctx += QString("\n\n## Current State\n- Project: %1\n- Modules (%2): ")
            .arg(proj->name()).arg(proj->modules().size());
        QStringList modNames;
        for (const ModuleInstance& m : proj->modules())
            modNames.append(QString("%1(id=%2)").arg(m.moduleId).arg(m.id));
        ctx += modNames.join(", ");
        ctx += QString("\n- Connections: %1\n- RunEngine: %2\n")
            .arg(proj->connections().size())
            .arg(RunEngine::instance().isRunning() ? "Running" : "Idle");
    } else {
        ctx += "\n\n## Current State\n- No project opened.\n";
    }
    QList<GuiEvent> recent = m_observer->recentEvents(5);
    if (!recent.isEmpty()) {
        ctx += "\n## Recent Events\n";
        for (const GuiEvent& e : recent)
            ctx += QString("[%1] %2\n").arg(e.timestamp.toString("hh:mm:ss")).arg(e.typeString());
    }
    return ctx;
}

// ========== User Input ==========

void AgentController::sendUserMessage(const QString& message) {
    if (!m_llmClient) { emit llmErrorOccurred("LLM client not configured"); return; }
    if (m_agentBusy) {
        emit llmResponseReceived("Agent is busy processing a previous request. Please wait.", {});
        return;
    }
    m_agentBusy = true;
    AgentMessage m; m.role = "user"; m.content = message;
    m_conversationHistory.append(m); trimHistory(); m_agentTurnCount = 0;
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

void AgentController::sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images) {
    if (!m_llmClient) { emit llmErrorOccurred("LLM client not configured"); return; }
    if (m_agentBusy) {
        emit llmResponseReceived("Agent is busy processing a previous request. Please wait.", {});
        return;
    }
    m_agentBusy = true;
    AgentMessage msg; msg.role = "user"; msg.content = message;
    for (const QPixmap& pm : images) {
        QByteArray d; QBuffer b(&d); b.open(QIODevice::WriteOnly); pm.save(&b, "PNG"); b.close();
        msg.images.append({d, "image/png", "User image"});
    }
    m_conversationHistory.append(msg); trimHistory(); m_agentTurnCount = 0;
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

// ========== LLM Response ==========

void AgentController::onLLMResponse(const AgentResponse& resp) {
    AgentMessage am; am.role = "assistant"; am.content = resp.content;
    am.toolCalls = resp.toolCalls;
    m_conversationHistory.append(am); trimHistory();

    if (!resp.toolCalls.isEmpty()) {
        if (m_permissionLevel == PermissionLevel::Observer) {
            m_agentBusy = false;
            emit llmResponseReceived(resp.content, resp.toolCalls);
            return;
        }
        if (m_permissionLevel == PermissionLevel::Advisor) {
            m_pendingToolCalls = resp.toolCalls;
            emit llmResponseReceived(resp.content, resp.toolCalls);
            emit toolsPendingConfirmation(resp.toolCalls);
            return;  // 等用户确认，m_agentBusy 保持 true
        }
        extendAgentLoop(resp.toolCalls);  // Autopilot
        return;
    }
    m_agentBusy = false;  // 终态：无更多工具调用
    emit llmResponseReceived(resp.content, {});
}

// ========== Tool Confirm (通过 QueuedConnection 入队，避免阻塞) ==========

void AgentController::confirmPendingTools() {
    if (m_pendingToolCalls.isEmpty()) return;
    // ⚠️ 拷贝数据后入队，防止执行时数据被修改
    QMetaObject::invokeMethod(this, [this, calls = m_pendingToolCalls]() {
        doConfirmPendingTools(calls);
    }, Qt::QueuedConnection);
    m_pendingToolCalls = QJsonArray();
}

void AgentController::doConfirmPendingTools(QJsonArray calls) {
    extendAgentLoop(calls);
}

void AgentController::rejectPendingTools() {
    QMetaObject::invokeMethod(this, [this]() {
        m_agentBusy = false;
        m_pendingToolCalls = QJsonArray();
        emit llmResponseReceived("Tool execution cancelled by user.", {});
    }, Qt::QueuedConnection);
}

// ========== Agent Loop Core ==========

void AgentController::extendAgentLoop(const QJsonArray& toolCalls) {
    if (m_agentTurnCount++ >= MAX_AGENT_TURNS) {
        m_agentBusy = false;
        emit llmResponseReceived(
            QString("Agent reached maximum reasoning turns (%1).").arg(MAX_AGENT_TURNS), {});
        return;
    }

    // 解析 tool_calls
    QList<QPair<QString, QJsonObject>> tools; QList<QString> ids;
    for (const QJsonValue& v : toolCalls) {
        QJsonObject tc = v.toObject(); QString tid = tc["id"].toString();
        QString name = tc["name"].toString(); QJsonObject params = tc["arguments"].toObject();
        if (name.isEmpty()) {
            QJsonObject func = tc["function"].toObject(); name = func["name"].toString();
            QJsonValue av = func["arguments"];
            if (av.isString()) params = QJsonDocument::fromJson(av.toString().toUtf8()).object();
            else if (av.isObject()) params = av.toObject();
        }
        if (!name.isEmpty()) { tools.append({name, params}); ids.append(tid); }
    }
    if (tools.isEmpty()) { emit llmErrorOccurred("No valid tool calls"); return; }

    // 批量执行
    QJsonObject batchResult = m_actor->executeTools(tools,
        QString("Agent turn %1").arg(m_agentTurnCount));

    // 拆分每个 tool 结果
    QJsonArray resultsArray = batchResult["results"].toArray();
    for (int i = 0; i < resultsArray.size() && i < ids.size(); ++i) {
        AgentMessage tm; tm.role = "tool"; tm.toolCallId = ids[i];
        tm.content = QString(QJsonDocument(resultsArray[i].toObject()["result"].toObject())
            .toJson(QJsonDocument::Compact));
        m_conversationHistory.append(tm);
    }
    trimHistory();

    Logger::instance().addLog(
        QString("[AgentLoop] Executed %1 tool(s)").arg(resultsArray.size()), LogLevel::Debug, "Agent");

    // 🔁 继续 LLM 请求
    emit agentLoopIterating();
    if (!m_llmClient) { emit llmErrorOccurred("LLM disconnected"); return; }
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

// ========== Helpers ==========

void AgentController::trimHistory() {
    while (m_conversationHistory.size() > MAX_HISTORY_SIZE)
        m_conversationHistory.removeFirst();
}

void AgentController::onGuiEvent(const GuiEvent& event) {
    Logger::instance().addLog(
        QString("[AgentObserver] %1 from %2").arg(event.typeString()).arg(event.source),
        LogLevel::Debug, "Agent");
}

QJsonObject AgentController::handleToolCall(const QString& toolName, const QJsonObject& params) {
    Logger::instance().addLog(
        QString("[ToolCall] %1").arg(toolName), LogLevel::Info, "Agent");
    emit agentActionReceived(toolName, params);
    if (m_permissionLevel == PermissionLevel::Observer)
        return {{"error", "Permission denied: Observer mode"}};
    QJsonObject result = m_actor->executeTool(toolName, params);
    AgentActionLogEntry e; e.timestamp = QDateTime::currentDateTime(); e.actor = "Agent";
    e.action = toolName; e.result = result.contains("error") ? "error" : "success";
    emit actionLogEntryAdded(e);
    return result;
}

void AgentController::onLLMError(const QString& error) {
    m_agentBusy = false;
    emit llmErrorOccurred(error);
}

QString AgentController::defaultSystemPrompt() {
    return QString(
        "You are DeepLux Agent, an AI assistant embedded in DeepLux Vision "
        "(an industrial machine vision software).\n\n"
        "Your capabilities:\n"
        "- Create and manage vision inspection projects\n"
        "- Add/remove/configure image processing modules\n"
        "- Connect modules into execution flows, run flows, interpret results\n\n"
        "Knowledge Base: Use read_documentation(topic) to learn about modules and workflows.\n"
        "Topics: module names, 'workflow', 'params', 'all'.\n\n"
        "Rules:\n"
        "1. Always use tools, never pseudo-code.\n"
        "2. Check state with get_flow_state before modifying.\n"
        "3. Use read_documentation when unsure about parameters.\n"
        "4. Be concise. Industrial users prefer direct answers.\n"
    );
}

} // namespace DeepLux
