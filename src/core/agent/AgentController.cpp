#include "AgentController.h"

#include "AgentActor.h"
#include "AgentBridge.h"
#include "AgentObserver.h"
#include "GuiEvent.h"
#include "ILLMClient.h"
#include "ToolSchema.h"
#include "common/Logger.h"
#include "engine/RunEngine.h"
#include "manager/ConfigManager.h"
#include "manager/ProjectManager.h"
#include "model/Project.h"

#include <QBuffer>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPixmap>

namespace DeepLux {

namespace {
constexpr int kMaxAgentToolTurns = 20;
constexpr int kMaxToolCallsPerTurn = 10;

bool isReadOnlyToolName(const QString& toolName) {
    static const QStringList readOnlyTools = {
        "get_flow_state",
        "get_available_plugins",
        "get_module_params_schema",
        "get_run_results",
        "read_documentation",
    };
    return readOnlyTools.contains(toolName);
}

QString toolCallName(const QJsonObject& tc) {
    QString name = tc["name"].toString();
    if (name.isEmpty())
        name = tc["function"].toObject()["name"].toString();
    return name;
}

QJsonObject toolCallParams(const QJsonObject& tc) {
    QJsonObject params = tc["arguments"].toObject();
    if (params.isEmpty()) {
        QJsonValue av = tc["function"].toObject()["arguments"];
        if (av.isString())
            params = QJsonDocument::fromJson(av.toString().toUtf8()).object();
        else if (av.isObject())
            params = av.toObject();
    }
    return params;
}

bool isDangerousToolName(const QString& toolName) {
    return ToolSchema::instance().findTool(toolName).dangerous;
}

QJsonObject makeToolCall(const QString& toolName, const QJsonObject& params) {
    return QJsonObject{
        {"id", QString("manual_%1").arg(toolName)},
        {"type", "function"},
        {"name", toolName},
        {"arguments", params},
    };
}
} // namespace

AgentController::AgentController(QObject* parent)
    : QObject(parent), m_observer(new AgentObserver(this)), m_actor(new AgentActor(this)) {}

AgentController::~AgentController() = default;

AgentController& AgentController::instance() {
    static AgentController instance;
    return instance;
}

bool AgentController::initialize() {
    if (m_initialized)
        return true;
    qRegisterMetaType<AgentResponse>("AgentResponse");
    qRegisterMetaType<AgentResponse>("DeepLux::AgentResponse");
    ToolSchema::instance().registerDefaultTools();
    if (!m_observer->initialize()) {
        qWarning() << "AgentController: Failed to initialize AgentObserver";
        return false;
    }
    connect(m_observer, &AgentObserver::guiEventOccurred, this, &AgentController::onGuiEvent);
    AgentBridge::instance().setToolCallCallback(
        [this](const QString& n, const QJsonObject& p) { return handleToolCall(n, p); });
    connect(m_actor, &AgentActor::toolExecuted, this,
            [this](const QString& t, const QJsonObject& r) { emit agentActionCompleted(t, r); });
    connect(m_actor, &AgentActor::toolError, this, [this](const QString& t, const QString& e) {
        emit agentActionCompleted(t, {{"error", e}});
    });
    m_systemPrompt = ConfigManager::instance().groupString("agent", "systemPrompt", defaultSystemPrompt());
    m_initialized = true;
    return true;
}

void AgentController::shutdown() {
    if (!m_initialized)
        return;
    AgentBridge::instance().setToolCallCallback(nullptr);
    if (m_observer)
        m_observer->shutdown();
    if (m_llmClient)
        disconnect(m_llmClient, nullptr, this, nullptr);
    m_initialized = false;
}

void AgentController::setPermissionLevel(PermissionLevel level) {
    if (m_permissionLevel == level)
        return;
    m_permissionLevel = level;
    emit permissionLevelChanged(level);
}

void AgentController::setLLMClient(ILLMClient* client) {
    if (m_llmClient)
        disconnect(m_llmClient, nullptr, this, nullptr);
    m_llmClient = client;
    if (m_llmClient) {
        connect(m_llmClient, &ILLMClient::responseReceived, this, &AgentController::onLLMResponse,
                Qt::QueuedConnection);
        connect(m_llmClient, &ILLMClient::errorOccurred, this, &AgentController::onLLMError, Qt::QueuedConnection);
    }
}

void AgentController::logAction(const AgentActionLogEntry& entry) {
    emit actionLogEntryAdded(entry);
}

void AgentController::clearConversation() {
    m_conversationHistory.clear();
    m_pendingToolCalls = QJsonArray();
    m_continueAfterPendingTools = true;
    m_agentTurnCount = 0;
    transitionTo(AgentState::Idle);
}

bool AgentController::undoLastAgentAction() {
    if (!m_actor || !m_actor->undoStack() || !m_actor->undoStack()->canUndo())
        return false;
    if (RunEngine::instance().isBusy())
        return false;
    m_actor->undoStack()->undo();
    AgentActionLogEntry e;
    e.timestamp = QDateTime::currentDateTime();
    e.actor = "User";
    e.action = "undo";
    e.result = "success";
    e.undoable = false;
    emit actionLogEntryAdded(e);
    return true;
}

// ========== State Machine ==========

void AgentController::transitionTo(AgentState newState) {
    QMutexLocker locker(&m_stateMutex);
    if (m_state == newState)
        return;
    qDebug() << "[AgentState]" << stateName(m_state) << "->" << stateName(newState);
    m_state = newState;
}

bool AgentController::tryTransitionTo(AgentState expected, AgentState newState) {
    QMutexLocker locker(&m_stateMutex);
    if (m_state != expected)
        return false;
    if (m_state == newState)
        return true;
    qDebug() << "[AgentState]" << stateName(m_state) << "->" << stateName(newState);
    m_state = newState;
    return true;
}

QString AgentController::stateName(AgentState state) {
    switch (state) {
    case AgentState::Idle:
        return "Idle";
    case AgentState::Thinking:
        return "Thinking";
    case AgentState::Confirming:
        return "Confirming";
    case AgentState::Executing:
        return "Executing";
    }
    return "Unknown";
}

// ========== Context ==========

QString AgentController::buildContext() {
    QString ctx = m_systemPrompt;
    Project* proj = ProjectManager::instance().currentProject();
    if (proj) {
        ctx += QString("\n\n## Current State\n- Project: %1\n- Modules (%2): ")
                   .arg(proj->name())
                   .arg(proj->modules().size());
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
    if (!m_llmClient) {
        emit llmErrorOccurred("LLM client not configured");
        return;
    }
    if (!tryTransitionTo(AgentState::Idle, AgentState::Thinking)) {
        emit llmResponseReceived("Agent is busy processing a previous request. Please wait.", {});
        return;
    }
    AgentMessage m;
    m.role = "user";
    m.content = message;
    m_conversationHistory.append(m);
    trimHistoryIfNeeded();
    m_agentTurnCount = 0;
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

void AgentController::sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images) {
    if (!m_llmClient) {
        emit llmErrorOccurred("LLM client not configured");
        return;
    }
    if (!tryTransitionTo(AgentState::Idle, AgentState::Thinking)) {
        emit llmResponseReceived("Agent is busy processing a previous request. Please wait.", {});
        return;
    }

    // DeepSeek 不支持 vision API (image_url 格式), 图片转为文本描述
    QString actualMessage = message;
    if (!message.isEmpty())
        actualMessage += "\n\n";
    actualMessage += QString("[User attached %1 image(s). Describe what you see or ask the user to describe them. "
                             "If you need vision capabilities, switch to a multimodal model like gpt-4o or claude.]")
                         .arg(images.size());

    AgentMessage msg;
    msg.role = "user";
    msg.content = actualMessage;
    // 不填充 msg.images — DeepSeek 不支持 image_url
    m_conversationHistory.append(msg);
    trimHistoryIfNeeded();
    m_agentTurnCount = 0;
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

    AgentMessage am;
    am.role = "assistant";
    am.content = resp.content;
    am.toolCalls = resp.toolCalls;
    am.reasoningContent = resp.reasoningContent;
    m_conversationHistory.append(am);
    trimHistoryIfNeeded();

    if (!resp.toolCalls.isEmpty()) {
        if (m_permissionLevel == PermissionLevel::Observer) {
            transitionTo(AgentState::Idle);
            emit llmResponseReceived(resp.content, resp.toolCalls);
            return;
        }

        bool requiresConfirmation = false;
        for (const QJsonValue& v : resp.toolCalls) {
            const QString name = toolCallName(v.toObject());
            if ((m_permissionLevel == PermissionLevel::Advisor && !isReadOnlyToolName(name)) ||
                (m_permissionLevel == PermissionLevel::Autopilot && isDangerousToolName(name))) {
                requiresConfirmation = true;
                break;
            }
        }
        if (requiresConfirmation) {
            m_pendingToolCalls = resp.toolCalls;
            m_continueAfterPendingTools = true;
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
    m_continueAfterPendingTools = true;
}

void AgentController::doConfirmPendingTools(QJsonArray calls) {
    if (m_state != AgentState::Confirming) {
        qWarning() << "[AgentController] doConfirmPendingTools: state changed to" << stateName(m_state);
        return;
    }
    transitionTo(AgentState::Executing);
    if (m_continueAfterPendingTools) {
        extendAgentLoop(calls);
        return;
    }

    QList<QPair<QString, QJsonObject>> tools;
    for (const QJsonValue& v : calls) {
        QJsonObject tc = v.toObject();
        const QString name = toolCallName(tc);
        if (!name.isEmpty())
            tools.append({name, toolCallParams(tc)});
    }
    QJsonObject batchResult = m_actor->executeTools(tools, QString("Confirmed Agent tools"));
    QJsonArray resultsArray = batchResult["results"].toArray();
    for (int i = 0; i < resultsArray.size() && i < tools.size(); ++i) {
        QJsonObject result = resultsArray[i].toObject()["result"].toObject();
        AgentActionLogEntry e;
        e.timestamp = QDateTime::currentDateTime();
        e.actor = "Agent";
        e.action = tools[i].first;
        e.params = QString(QJsonDocument(tools[i].second).toJson(QJsonDocument::Compact));
        e.result = result.contains("error") ? "error" : "success";
        e.undoable = !isReadOnlyToolName(tools[i].first);
        emit actionLogEntryAdded(e);
    }
    transitionTo(AgentState::Idle);
}

void AgentController::rejectPendingTools() {
    if (m_state != AgentState::Confirming) {
        qWarning() << "[AgentController] rejectPendingTools called in state" << stateName(m_state);
        return;
    }
    // 直接同步执行，避免 QueuedConnection 入队延迟
    m_pendingToolCalls = QJsonArray();
    m_continueAfterPendingTools = true;
    transitionTo(AgentState::Idle);
    emit llmResponseReceived("Tool execution cancelled by user.", {});
}

// ========== Agent Loop Core ==========

void AgentController::extendAgentLoop(const QJsonArray& toolCalls) {
    if (m_state != AgentState::Executing) {
        qWarning() << "[AgentController] Unexpected extendAgentLoop in state" << stateName(m_state);
        return;
    }
    if (toolCalls.size() > kMaxToolCallsPerTurn) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("已达到单轮工具数量上限");
        return;
    }
    if (m_agentTurnCount >= kMaxAgentToolTurns) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("已达到自动执行轮数上限");
        return;
    }
    ++m_agentTurnCount;

    // 解析 tool_calls
    QList<QPair<QString, QJsonObject>> tools;
    QList<QString> ids;
    for (const QJsonValue& v : toolCalls) {
        QJsonObject tc = v.toObject();
        QString tid = tc["id"].toString();
        QString name = toolCallName(tc);
        QJsonObject params = toolCallParams(tc);
        if (!name.isEmpty()) {
            tools.append({name, params});
            ids.append(tid);
        }
    }
    if (tools.isEmpty()) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("No valid tool calls to execute");
        return;
    }

    // 批量执行
    QJsonObject batchResult = m_actor->executeTools(tools, QString("Agent turn %1").arg(m_agentTurnCount));

    // 拆分每个 tool 结果，每条 tool call 对应一条独立的 tool role message（OpenAI 要求）
    QJsonArray resultsArray = batchResult["results"].toArray();
    for (int i = 0; i < resultsArray.size() && i < ids.size(); ++i) {
        QJsonObject result = resultsArray[i].toObject()["result"].toObject();
        AgentActionLogEntry e;
        e.timestamp = QDateTime::currentDateTime();
        e.actor = "Agent";
        e.action = tools[i].first;
        e.params = QString(QJsonDocument(tools[i].second).toJson(QJsonDocument::Compact));
        e.result = result.contains("error") ? "error" : "success";
        e.undoable = !isReadOnlyToolName(tools[i].first);
        emit actionLogEntryAdded(e);

        AgentMessage tm;
        tm.role = "tool";
        tm.toolCallId = ids[i];
        tm.content = QString(QJsonDocument(result).toJson(QJsonDocument::Compact));
        m_conversationHistory.append(tm);
    }
    trimHistoryIfNeeded();

    if (batchResult["status"].toString() == "partial_failed") {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred(batchResult["error"].toString("Agent tool execution failed"));
        return;
    }

    Logger::instance().addLog(QString("[AgentLoop] Executed %1 tool(s)").arg(resultsArray.size()), LogLevel::Debug,
                              "Agent");

    // 🔁 继续 LLM 请求
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
    // 仅靠 token 估计裁剪，不设硬消息数上限
    // 每字符 ≈ 0.3 token，每消息 overhead ≈ 20 tokens
    constexpr int maxEstTokens = 256000;

    auto estimateHistoryTokens = [this]() {
        int estimated = m_systemPrompt.length() * 0.3;
        for (const auto& m : m_conversationHistory) {
            estimated += m.content.length() * 0.3 + 20;
            if (!m.reasoningContent.isEmpty())
                estimated += m.reasoningContent.length() * 0.3;
        }
        return estimated;
    };

    auto trimLeadingPartialTurn = [this]() {
        while (!m_conversationHistory.isEmpty() && m_conversationHistory.first().role != "user")
            m_conversationHistory.removeFirst();
    };

    trimLeadingPartialTurn();
    int estimated = estimateHistoryTokens();

    while (estimated > maxEstTokens) {
        int secondUserIdx = -1, userCount = 0;
        for (int i = 0; i < m_conversationHistory.size(); ++i)
            if (m_conversationHistory[i].role == "user" && ++userCount == 2) { secondUserIdx = i; break; }
        if (secondUserIdx > 1) {
            m_conversationHistory = m_conversationHistory.mid(secondUserIdx);
            trimLeadingPartialTurn();
            estimated = estimateHistoryTokens();
        } else {
            break;
        }
    }
}

void AgentController::onGuiEvent(const GuiEvent& event) {
    // AgentObserver DEBUG 镜像日志过于冗余，降级为不输出
    Q_UNUSED(event)
}

QJsonObject AgentController::handleToolCall(const QString& toolName, const QJsonObject& params) {
    if (m_state != AgentState::Idle)
        return {{"error", "Agent is busy processing another request"}};
    if (!ToolSchema::instance().hasTool(toolName))
        return m_actor->executeTool(toolName, params);

    const bool readOnly = isReadOnlyToolName(toolName);
    const bool dangerous = isDangerousToolName(toolName);
    if (m_permissionLevel == PermissionLevel::Observer && !readOnly)
        return {{"error", "Permission denied: Observer mode"}};
    if ((m_permissionLevel == PermissionLevel::Advisor && !readOnly) ||
        (m_permissionLevel == PermissionLevel::Autopilot && dangerous)) {
        QJsonArray calls;
        calls.append(makeToolCall(toolName, params));
        m_pendingToolCalls = calls;
        m_continueAfterPendingTools = false;
        transitionTo(AgentState::Confirming);
        emit toolsPendingConfirmation(calls);
        return QJsonObject{{"status", "pending_confirmation"}, {"tools", calls}};
    }

    Logger::instance().addLog(QString("[ToolCall] %1").arg(toolName), LogLevel::Info, "Agent");
    emit agentActionReceived(toolName, params);
    QJsonObject result = m_actor->executeTool(toolName, params);
    AgentActionLogEntry e;
    e.timestamp = QDateTime::currentDateTime();
    e.actor = "Agent";
    e.action = toolName;
    e.params = QString(QJsonDocument(params).toJson(QJsonDocument::Compact));
    e.result = result.contains("error") ? "error" : "success";
    e.undoable = !readOnly && !result.contains("error");
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
        "4. If the user only asked a question or gave a simple instruction that is already fulfilled, do NOT call "
        "additional tools just to verify — respond directly.\n"
        "5. Use read_documentation when unsure about module parameters.\n"
        "6. Be concise. Industrial users prefer direct answers.\n");
}

} // namespace DeepLux
