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

#include <QFile>
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
    transitionTo(AgentState::Idle);
}

// ========== State Machine ==========

void AgentController::transitionTo(AgentState newState) {
    if (m_state == newState) return;
    qDebug() << "[AgentState]" << stateName(m_state) << "->" << stateName(newState);
    m_state = newState;
}

QString AgentController::stateName(AgentState state) {
    switch (state) {
    case AgentState::Idle:       return "Idle";
    case AgentState::Thinking:   return "Thinking";
    case AgentState::Confirming: return "Confirming";
    case AgentState::Executing:  return "Executing";
    }
    return "Unknown";
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
    if (m_state != AgentState::Idle) {
        emit llmResponseReceived("Agent is busy processing a previous request. Please wait.", {});
        return;
    }
    transitionTo(AgentState::Thinking);
    AgentMessage m; m.role = "user"; m.content = message;
    m_conversationHistory.append(m); trimHistoryIfNeeded(); m_agentTurnCount = 0;
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

void AgentController::sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images) {
    if (!m_llmClient) { emit llmErrorOccurred("LLM client not configured"); return; }
    if (m_state != AgentState::Idle) {
        emit llmResponseReceived("Agent is busy processing a previous request. Please wait.", {});
        return;
    }
    transitionTo(AgentState::Thinking);
    AgentMessage msg; msg.role = "user"; msg.content = message;
    for (const QPixmap& pm : images) {
        QByteArray d; QBuffer b(&d); b.open(QIODevice::WriteOnly); pm.save(&b, "PNG"); b.close();
        msg.images.append({d, "image/png", "User image"});
    }
    m_conversationHistory.append(msg); trimHistoryIfNeeded(); m_agentTurnCount = 0;
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

// ========== LLM Response ==========

void AgentController::onLLMResponse(const AgentResponse& resp) {
    if (m_state != AgentState::Thinking) {
        qWarning() << "[AgentController] Unexpected onLLMResponse in state" << stateName(m_state);
        return;
    }

    AgentMessage am; am.role = "assistant"; am.content = resp.content;
    am.toolCalls = resp.toolCalls;
    am.reasoningContent = resp.reasoningContent;
    m_conversationHistory.append(am); trimHistoryIfNeeded();

    if (!resp.toolCalls.isEmpty()) {
        if (m_permissionLevel == PermissionLevel::Observer) {
            transitionTo(AgentState::Idle);
            emit llmResponseReceived(resp.content, resp.toolCalls);
            return;
        }
        if (m_permissionLevel == PermissionLevel::Advisor) {
            m_pendingToolCalls = resp.toolCalls;
            transitionTo(AgentState::Confirming);
            emit llmResponseReceived(resp.content, resp.toolCalls);
            emit toolsPendingConfirmation(resp.toolCalls);
            return;
        }
        // Autopilot: 直接执行 + 闭环
        transitionTo(AgentState::Executing);
        extendAgentLoop(resp.toolCalls);
        return;
    }

    // 纯文本回复 — 终态
    transitionTo(AgentState::Idle);
    emit llmResponseReceived(resp.content, {});
}

// ========== Tool Confirm (通过 QueuedConnection 入队，避免阻塞按钮信号链) ==========

void AgentController::confirmPendingTools() {
    QFile logFile("/tmp/deeplux_agent_diag.log");
    logFile.open(QIODevice::WriteOnly | QIODevice::Append);
    logFile.write(QString("[DIAG] confirmPendingTools called, state=%1\n").arg(stateName(m_state)).toUtf8());
    logFile.close();
    qDebug() << "[DIAG] confirmPendingTools called, state=" << stateName(m_state);
    if (m_state != AgentState::Confirming) {
        qWarning() << "[AgentController] confirmPendingTools called in state" << stateName(m_state);
        return;
    }
    if (m_pendingToolCalls.isEmpty()) {
        transitionTo(AgentState::Idle);
        return;
    }
    // 直接同步执行，避免 QueuedConnection 入队延迟导致 UI 无响应
    QJsonArray calls = m_pendingToolCalls;
    m_pendingToolCalls = QJsonArray();
    doConfirmPendingTools(calls);
}

void AgentController::doConfirmPendingTools(QJsonArray calls) {
    QFile logFile("/tmp/deeplux_agent_diag.log");
    logFile.open(QIODevice::WriteOnly | QIODevice::Append);
    logFile.write(QString("[DIAG] doConfirmPendingTools called, state=%1 calls=%2\n").arg(stateName(m_state)).arg(calls.size()).toUtf8());
    logFile.close();
    qDebug() << "[DIAG] doConfirmPendingTools called, state=" << stateName(m_state) << "calls=" << calls.size();
    if (m_state != AgentState::Confirming) {
        qWarning() << "[AgentController] doConfirmPendingTools: state changed to" << stateName(m_state);
        return;
    }
    transitionTo(AgentState::Executing);
    extendAgentLoop(calls);
}

void AgentController::rejectPendingTools() {
    if (m_state != AgentState::Confirming) {
        qWarning() << "[AgentController] rejectPendingTools called in state" << stateName(m_state);
        return;
    }
    // 直接同步执行，避免 QueuedConnection 入队延迟
    m_pendingToolCalls = QJsonArray();
    transitionTo(AgentState::Idle);
    emit llmResponseReceived("Tool execution cancelled by user.", {});
}

// ========== Agent Loop Core ==========

void AgentController::extendAgentLoop(const QJsonArray& toolCalls) {
    if (m_state != AgentState::Executing) {
        qWarning() << "[AgentController] Unexpected extendAgentLoop in state" << stateName(m_state);
        return;
    }
    m_agentTurnCount++;

    if (m_agentTurnCount > MAX_AGENT_TURNS) {
        transitionTo(AgentState::Idle);
        emit llmResponseReceived(QString("Agent reached maximum reasoning turns (%1).").arg(MAX_AGENT_TURNS), {});
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
    if (tools.isEmpty()) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("No valid tool calls to execute");
        return;
    }

    // 批量执行
    qDebug() << "[DIAG] extendAgentLoop: executing" << tools.size() << "tool(s)";
    {
        QFile logFile("/tmp/deeplux_agent_diag.log");
        logFile.open(QIODevice::WriteOnly | QIODevice::Append);
        logFile.write(QString("[DIAG] extendAgentLoop: executing %1 tool(s)\n").arg(tools.size()).toUtf8());
        logFile.close();
    }
    QJsonObject batchResult = m_actor->executeTools(tools,
        QString("Agent turn %1").arg(m_agentTurnCount));
    qDebug() << "[DIAG] extendAgentLoop: executeTools returned" << batchResult;
    {
        QFile logFile("/tmp/deeplux_agent_diag.log");
        logFile.open(QIODevice::WriteOnly | QIODevice::Append);
        logFile.write(QString("[DIAG] extendAgentLoop: executeTools returned\n").toUtf8());
        logFile.close();
    }

    // 拆分每个 tool 结果，每条 tool call 对应一条独立的 tool role message（OpenAI 要求）
    QJsonArray resultsArray = batchResult["results"].toArray();
    for (int i = 0; i < resultsArray.size() && i < ids.size(); ++i) {
        AgentMessage tm; tm.role = "tool"; tm.toolCallId = ids[i];
        tm.content = QString(QJsonDocument(resultsArray[i].toObject()["result"].toObject())
            .toJson(QJsonDocument::Compact));
        m_conversationHistory.append(tm);
    }
    trimHistoryIfNeeded();

    Logger::instance().addLog(
        QString("[AgentLoop] Executed %1 tool(s)").arg(resultsArray.size()), LogLevel::Debug, "Agent");

    // 🔁 继续 LLM 请求
    qDebug() << "[DIAG] extendAgentLoop: sending LLM request";
    {
        QFile logFile("/tmp/deeplux_agent_diag.log");
        logFile.open(QIODevice::WriteOnly | QIODevice::Append);
        logFile.write(QString("[DIAG] extendAgentLoop: sending LLM request\n").toUtf8());
        logFile.close();
    }
    emit agentLoopIterating();
    if (!m_llmClient) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("LLM disconnected during agent loop");
        return;
    }
    transitionTo(AgentState::Thinking);
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

// ========== Helpers ==========

void AgentController::trimHistoryIfNeeded() {
    // 粗略估计: 每字符 ≈ 0.3 token (中英文混合)，每消息 overhead ≈ 20 tokens
    // 目标: 不超过模型 context window 的 80% (128K * 0.8 ≈ 100K)
    constexpr int maxEstTokens = 256000;
    int estimated = 0;
    for (const auto& m : m_conversationHistory) {
        estimated += m.content.length() * 0.3 + 20;
        if (!m.reasoningContent.isEmpty()) estimated += m.reasoningContent.length() * 0.3;
    }
    // 加上 system prompt
    estimated += m_systemPrompt.length() * 0.3;

    // 以完整轮次为单位从头部裁剪
    while (estimated > maxEstTokens) {
        int secondUserIdx = -1;
        int userCount = 0;
        for (int i = 0; i < m_conversationHistory.size(); ++i) {
            if (m_conversationHistory[i].role == "user" && ++userCount == 2) {
                secondUserIdx = i; break;
            }
        }
        if (secondUserIdx > 1) {
            int removed = 0;
            for (int i = 0; i < secondUserIdx; ++i)
                removed += m_conversationHistory[i].content.length() * 0.3 + 20;
            m_conversationHistory = m_conversationHistory.mid(secondUserIdx);
            estimated -= removed;
        } else {
            break;  // 仅剩一轮, 无法裁剪
        }
    }
}

void AgentController::onGuiEvent(const GuiEvent& event) {
    Logger::instance().addLog(
        QString("[AgentObserver] %1 from %2").arg(event.typeString()).arg(event.source),
        LogLevel::Debug, "Agent");
}

QJsonObject AgentController::handleToolCall(const QString& toolName, const QJsonObject& params) {
    if (m_state != AgentState::Idle)
        return {{"error", "Agent is busy processing another request"}};
    if (m_permissionLevel == PermissionLevel::Observer)
        return {{"error", "Permission denied: Observer mode"}};
    Logger::instance().addLog(
        QString("[ToolCall] %1").arg(toolName), LogLevel::Info, "Agent");
    emit agentActionReceived(toolName, params);
    QJsonObject result = m_actor->executeTool(toolName, params);
    AgentActionLogEntry e; e.timestamp = QDateTime::currentDateTime(); e.actor = "Agent";
    e.action = toolName; e.result = result.contains("error") ? "error" : "success";
    emit actionLogEntryAdded(e);
    return result;
}

void AgentController::onLLMError(const QString& error) {
    m_pendingToolCalls = QJsonArray();
    transitionTo(AgentState::Idle);
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
        "Critical rules:\n"
        "1. If get_flow_state returns \"No project opened\", you MUST call create_project first.\n"
        "   Never call get_flow_state again without creating a project — it will keep returning the same error.\n"
        "2. Always use tools, never describe pseudo-code.\n"
        "3. When you have completed all the user's requests, respond in plain text (no more tool calls).\n"
        "4. Use read_documentation when unsure about module parameters.\n"
        "5. Be concise. Industrial users prefer direct answers.\n"
    );
}

} // namespace DeepLux
