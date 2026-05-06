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
#include <QTimer>

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

void AgentController::clearConversation()
{
    m_conversationHistory.clear();
    m_pendingToolCalls = QJsonArray();
    m_agentTurnCount = 0;
}

// ========== Agentic Loop ==========

QString AgentController::buildContext()
{
    QString ctx = m_systemPrompt;

    // 实时 GUI 状态摘要 — 不存对话历史，每次请求前即时查询
    Project* proj = ProjectManager::instance().currentProject();
    if (proj) {
        ctx += QString(
            "\n\n## Current State\n"
            "- Project: %1\n"
            "- Modules (%2): ")
            .arg(proj->name())
            .arg(proj->modules().size());
        QStringList modNames;
        for (const ModuleInstance& m : proj->modules()) {
            modNames.append(QString("%1(id=%2)").arg(m.moduleId).arg(m.id));
        }
        ctx += modNames.join(", ");
        ctx += QString("\n- Connections: %1\n- RunEngine: %2\n")
            .arg(proj->connections().size())
            .arg(RunEngine::instance().isRunning() ? "Running" : "Idle");
    } else {
        ctx += "\n\n## Current State\n- No project opened.\n";
    }

    // 最近 GUI 事件（Observer 记录）
    QList<GuiEvent> recent = m_observer->recentEvents(5);
    if (!recent.isEmpty()) {
        ctx += "\n## Recent Events\n";
        for (const GuiEvent& e : recent) {
            ctx += QString("[%1] %2\n")
                .arg(e.timestamp.toString("hh:mm:ss"))
                .arg(e.typeString());
        }
    }

    return ctx;
}

void AgentController::sendUserMessage(const QString& message)
{
    if (!m_llmClient) {
        emit llmErrorOccurred("LLM client not configured");
        return;
    }

    // 追加用户消息到对话历史
    AgentMessage userMsg;
    userMsg.role = "user";
    userMsg.content = message;
    m_conversationHistory.append(userMsg);
    trimHistory();

    // 重置轮数计数
    m_agentTurnCount = 0;

    // 构建完整上下文（system prompt + 实时状态）
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();

    // 延迟到下一轮事件循环发送，避免卡 UI
    QTimer::singleShot(0, this, [this, ctx]() {
        if (m_llmClient) {
            m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
        }
    });
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

    m_conversationHistory.append(msg);
    trimHistory();
    m_agentTurnCount = 0;

    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();

    QTimer::singleShot(0, this, [this, ctx]() {
        if (m_llmClient) {
            m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
        }
    });
}

void AgentController::onLLMResponse(const AgentResponse& resp)
{
    // 追加 assistant 回应到对话历史
    AgentMessage assistantMsg;
    assistantMsg.role = "assistant";
    assistantMsg.content = resp.content;
    assistantMsg.toolCalls = resp.toolCalls;  // QJsonArray, direct OpenAI format
    m_conversationHistory.append(assistantMsg);
    trimHistory();

    // 有工具调用 → 执行 + 闭环
    if (!resp.toolCalls.isEmpty()) {
        if (m_permissionLevel == PermissionLevel::Observer) {
            emit llmResponseReceived(resp.content, resp.toolCalls);
            return;
        }
        if (m_permissionLevel == PermissionLevel::Advisor) {
            m_pendingToolCalls = resp.toolCalls;
            emit toolsPendingConfirmation(resp.toolCalls);
            // 先展示 LLM 的文本内容和工具预览
            emit llmResponseReceived(resp.content, resp.toolCalls);
            return;
        }
        // Autopilot: 直接执行 + 闭环
        extendAgentLoop(resp.toolCalls);
        return;
    }

    // 纯文本回复 — 终态
    emit llmResponseReceived(resp.content, QJsonArray());
}

void AgentController::extendAgentLoop(const QJsonArray& toolCalls)
{
    if (m_agentTurnCount++ >= MAX_AGENT_TURNS) {
        emit llmResponseReceived(
            QString("Agent reached maximum reasoning turns (%1).").arg(MAX_AGENT_TURNS),
            QJsonArray());
        return;
    }

    // 解析 tool_calls，保留 id 用于结果回传
    QList<QPair<QString, QJsonObject>> tools;
    QList<QString> toolCallIds;
    for (const QJsonValue& v : toolCalls) {
        QJsonObject tc = v.toObject();
        QString toolCallId = tc["id"].toString();
        QString name = tc["name"].toString();
        QJsonObject params = tc["arguments"].toObject();
        if (name.isEmpty()) {
            name = tc["function"].toObject()["name"].toString();
            QString argsStr = tc["function"].toObject()["arguments"].toString();
            params = QJsonDocument::fromJson(argsStr.toUtf8()).object();
        }
        if (!name.isEmpty()) {
            tools.append({name, params});
            toolCallIds.append(toolCallId);
        }
    }

    if (tools.isEmpty()) {
        emit llmErrorOccurred("No valid tool calls to execute");
        return;
    }

    // 批量执行（macro undo）
    QJsonObject batchResult = m_actor->executeTools(tools,
        QString("Agent turn %1").arg(m_agentTurnCount));

    // 拆分 tool 结果，每条 tool call 对应一条独立的 tool role message（OpenAI 要求）
    QJsonArray resultsArray = batchResult["results"].toArray();
    for (int i = 0; i < resultsArray.size() && i < toolCallIds.size(); ++i) {
        QJsonObject entry = resultsArray[i].toObject();
        QJsonObject singleResult = entry["result"].toObject();

        AgentMessage toolMsg;
        toolMsg.role = "tool";
        toolMsg.toolCallId = toolCallIds[i];
        toolMsg.content = QString(QJsonDocument(singleResult).toJson(QJsonDocument::Compact));
        m_conversationHistory.append(toolMsg);
    }
    trimHistory();

    // 记录中间结果（不 emit llmResponseReceived，LLM 下一轮推理会给出最终回复）
    Logger::instance().addLog(
        QString("[AgentLoop] Executed %1 tool(s), continuing...").arg(resultsArray.size()),
        LogLevel::Debug, "Agent");

    // 🔁 闭环：用 QTimer::singleShot 将 LLM 请求推迟到下一轮事件循环
    // 避免在信号处理链中同步发送请求导致 UI 冻结
    emit agentLoopIterating();
    QTimer::singleShot(0, this, [this]() {
        if (!m_llmClient) {
            emit llmErrorOccurred("LLM client disconnected during agent loop");
            return;
        }
        AgentConversation ctx;
        ctx.messages = m_conversationHistory;
        ctx.systemPrompt = buildContext();
        m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
    });
}

void AgentController::confirmPendingTools()
{
    if (m_pendingToolCalls.isEmpty()) return;

    QJsonArray calls = m_pendingToolCalls;
    m_pendingToolCalls = QJsonArray();

    // 延迟到下一轮事件循环执行，避免阻塞 Confirm 按钮的信号链
    QTimer::singleShot(0, this, [this, calls]() {
        extendAgentLoop(calls);
    });
}

void AgentController::rejectPendingTools()
{
    m_pendingToolCalls = QJsonArray();
    emit llmResponseReceived("Tool execution cancelled by user.", QJsonArray());
}

void AgentController::trimHistory()
{
    while (m_conversationHistory.size() > MAX_HISTORY_SIZE) {
        m_conversationHistory.removeFirst();
    }
}

// ========== AgentBridge / Observer ==========

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
        "Knowledge Base:\n"
        "- Use read_documentation(topic) to learn about modules, parameters, and workflows.\n"
        "- Topics: module names (FindCircle, GrabImage...), 'workflow', 'params', 'modules', 'all'.\n\n"
        "Rules:\n"
        "1. Always use tools to make changes, never describe pseudo-code.\n"
        "2. Before modifying, check current state with get_flow_state if needed.\n"
        "3. When unsure about a module's parameters, call read_documentation first.\n"
        "4. Be concise. Industrial users prefer direct answers.\n"
        "5. When user pastes an image, analyze it visually if possible.\n"
        "6. Dangerous operations (remove_module, save_project) require user confirmation in Advisor mode.\n"
    );
}

} // namespace DeepLux
