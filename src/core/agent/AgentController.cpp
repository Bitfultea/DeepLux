#include "AgentController.h"

#include "AgentActor.h"
#include "AgentBridge.h"
#include "AgentObserver.h"
#include "AgentToolImage.h"
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
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPixmap>
#include <QSet>

namespace DeepLux {

namespace {
constexpr int kMaxAgentToolTurns = 20;
constexpr int kMaxToolCallsPerTurn = 10;
constexpr int kMaxImageAttachments = 4;
constexpr qint64 kMaxImageAttachmentBytes = 8LL * 1024 * 1024;

// 图像回灌预算（设计 §6.3）：每轮 ≤2 张、长边 ≤1280、单张 ≤4MB、单轮总量复用 8MB 预算
constexpr int kMaxToolImagesPerTurn = 2;
constexpr int kMaxToolImageLongEdge = 1280;
constexpr qint64 kMaxToolImageBytes = 4LL * 1024 * 1024;

/// 只有这些工具的结果 imagePath 会被回灌（设计 §6.1 白名单）
bool isImageProducingTool(const QString& toolName) {
    static const QSet<QString> tools = {QStringLiteral("capture_viewport_image"), QStringLiteral("export_node_output")};
    return tools.contains(toolName);
}

// ---- 权限派生（唯一事实来源：ToolSchema 元数据，设计 §3.2.1）----
struct ToolAccess {
    bool readOnly = false;
    bool dangerous = false;
};

// 未注册的工具按"写工具"处理（fail closed），与基线硬编码名单的行为一致。
ToolAccess toolAccess(const QString& toolName) {
    const ToolDefinition def = ToolSchema::instance().findTool(toolName);
    if (def.name.isEmpty())
        return {false, false};
    return {def.readOnly, def.dangerous};
}

// 唯一的确认判定；onLLMResponse 与 handleToolCall 必须都调用它。
// Observer 的"拒绝"由调用点显式返回，不由本函数决定（保持既有错误文案）。
bool requiresConfirmation(AgentController::PermissionLevel level, const ToolAccess& access) {
    using Level = AgentController::PermissionLevel;
    switch (level) {
    case Level::Observer:
        return !access.readOnly;
    case Level::Advisor:
        return !access.readOnly;
    case Level::Autopilot:
        return access.dangerous;
    }
    return true;
}

// D2：审计 undoable 必须与"是否真的压入了 QUndoStack"一致，事实来源是
// AgentActor 的派发注册表（create/open/save 三个工程级工具不入撤销栈）。
bool isAgentUndoable(const AgentActor* actor, const QString& toolName) {
    return actor && actor->toolUndoable(toolName);
}

// D4：工具耗时由 AgentActor 以内部字段 "_elapsedMs" 附带；消费方必须立刻剥离，
// 保证内部字段不进入模型上下文、审计 params/result 之外的任何 payload。
int takeElapsedMs(QJsonObject& result) {
    const QJsonValue value = result.value(QStringLiteral("_elapsedMs"));
    if (value.isUndefined())
        return -1;
    const int elapsedMs = value.toInt(-1);
    result.remove(QStringLiteral("_elapsedMs"));
    return elapsedMs;
}

AgentActionLogEntry makeAuditEntry(const AgentActor* actor, const QString& toolName, const QJsonObject& params,
                                   const QString& toolCallId, QJsonObject& result) {
    AgentActionLogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.actor = "Agent";
    entry.action = toolName;
    entry.params = QString(QJsonDocument(params).toJson(QJsonDocument::Compact));
    entry.durationMs = takeElapsedMs(result);
    entry.result = result.contains("error") ? "error" : "success";
    // R1：失败的工具不会压入 QUndoStack，审计 undoable 必须同步为 false
    entry.undoable = isAgentUndoable(actor, toolName) && !result.contains("error");
    entry.toolCallId = toolCallId;
    return entry;
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
    qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
    qRegisterMetaType<AgentActionLogEntry>("DeepLux::AgentActionLogEntry");
    qRegisterMetaType<LLMErrorInfo>("LLMErrorInfo");
    qRegisterMetaType<LLMErrorInfo>("DeepLux::LLMErrorInfo");
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
    connect(m_actor, &AgentActor::toolError, this,
            [this](const QString& t, const QString& e) { emit agentActionCompleted(t, {{"error", e}}); });
    m_systemPrompt = ConfigManager::instance().groupString("agent", "systemPrompt", defaultSystemPrompt());
    m_initialized = true;
    // R4（设计 §6.8）：启动时清理 7 天前的工具图像产物；best-effort，失败不阻塞对话
    const int removedImages = AgentToolImage::cleanupOlderThanDays(7);
    qDebug() << "[AgentController] tool image cleanup removed" << removedImages << "old file(s)";
    return true;
}

void AgentController::shutdown() {
    if (!m_initialized)
        return;
    // 先中止未完成的轮次：避免 shutdown 后仍有排队 tick / 等待中的运行回写状态
    abortCurrentTurn();
    AgentBridge::instance().setToolCallCallback(nullptr);
    if (m_observer)
        m_observer->shutdown();
    if (m_llmClient)
        disconnect(m_llmClient, nullptr, this, nullptr);
    // 阶7 Agent 评审修复（P3-10）：清空客户端与待确认工具，避免 shutdown 后
    // sendUserMessage 仍向已断开客户端发请求或残留待确认状态
    m_llmClient = nullptr;
    m_pendingToolCalls = QJsonArray();
    m_initialized = false;
    transitionTo(AgentState::Idle);
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
        connect(m_llmClient, &ILLMClient::errorOccurredClassified, this, &AgentController::onLLMErrorClassified,
                Qt::QueuedConnection);
    }
}

void AgentController::setSystemPrompt(const QString& prompt) {
    m_systemPrompt = prompt.trimmed();
    if (m_systemPrompt.isEmpty())
        m_systemPrompt = defaultSystemPrompt();
}

void AgentController::logAction(const AgentActionLogEntry& entry) {
    emit actionLogEntryAdded(entry);
}

void AgentController::clearConversation() {
    m_conversationHistory.clear();
    m_pendingToolCalls = QJsonArray();
    m_continueAfterPendingTools = true;
    m_agentTurnCount = 0;
    m_dispatchQueued = false;
    m_visionRetryDone = false;
    m_suppressNextLlmError = false;
    transitionTo(AgentState::Idle);
}

void AgentController::abortCurrentTurn() {
    const AgentState currentState = state();
    if (currentState == AgentState::Idle) {
        // 幂等：Idle 下不做任何事（不报错、不发信号）
        qWarning() << "[AgentController] abortCurrentTurn ignored in Idle state";
        return;
    }
    // 1) 作废排队任务与索引：turnGeneration 让已入队的 dispatch/续跑全部失效
    ++m_turnGeneration;
    m_pendingToolCalls = QJsonArray();
    m_continueAfterPendingTools = true;
    m_dispatchQueued = false;
    m_suppressNextLlmError = false;
    // 2) 中止 LLM 请求（客户端保证被主动 abort 的错误不分类、不重试）
    if (m_llmClient)
        m_llmClient->abortRequest();
    // 3) 中止等待中的运行（run_flow(wait)/tune 的嵌套事件循环）
    if (m_actor)
        m_actor->abortRunningTool();
    transitionTo(AgentState::Idle);
    emit llmResponseReceived(QStringLiteral("Stopped by user."), {});
    emit agentTurnAborted();
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
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state == newState)
            return;
        qDebug() << "[AgentState]" << stateName(m_state) << "->" << stateName(newState);
        m_state = newState;
    }
    // 锁外发信号，避免槽函数回调控制器时自锁
    emit agentStateChanged(newState);
}

bool AgentController::tryTransitionTo(AgentState expected, AgentState newState) {
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_state != expected)
            return false;
        if (m_state == newState)
            return true;
        qDebug() << "[AgentState]" << stateName(m_state) << "->" << stateName(newState);
        m_state = newState;
    }
    emit agentStateChanged(newState);
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
    if (!m_initialized) {
        emit llmErrorOccurred("Agent not initialized");
        return;
    }
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
    ++m_turnGeneration; // 新的用户轮次：作废上一轮残留的排队/续跑
    m_visionRetryDone = false;
    m_suppressNextLlmError = false;
    m_lastErrorCategory.clear(); // R6：每轮开始清空，避免 category 串用旧错误
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

void AgentController::sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images) {
    // 批3 复核五轮（P2-4）：附件上限（与 AgentChatPanel::kMaxImageAttachments
    // 同一契约，测试断言两者相等）——参数校验先于状态/客户端校验，超限在
    // 任何控制器状态下都得到明确反馈；UI 侧另有源头限额
    if (images.size() > kMaxImageAttachments) {
        emit llmErrorOccurred(QString("Too many image attachments (maximum %1)").arg(kMaxImageAttachments));
        return;
    }
    if (!m_initialized) {
        emit llmErrorOccurred("Agent not initialized");
        return;
    }
    if (!m_llmClient) {
        emit llmErrorOccurred("LLM client not configured");
        return;
    }
    if (!m_llmClient->supportsImageInput()) {
        emit llmErrorOccurred("The configured model does not support image input");
        return;
    }
    if (images.size() > kMaxImageAttachments) {
        emit llmErrorOccurred(QString("Too many image attachments (maximum %1)").arg(kMaxImageAttachments));
        return;
    }
    if (!tryTransitionTo(AgentState::Idle, AgentState::Thinking)) {
        emit llmResponseReceived("Agent is busy processing a previous request. Please wait.", {});
        return;
    }

    AgentMessage msg;
    msg.role = "user";
    msg.content = message.isEmpty() ? QStringLiteral("Analyze the attached image.") : message;
    qint64 totalBytes = 0;
    for (const QPixmap& image : images) {
        if (image.isNull())
            continue;

        QByteArray data;
        QBuffer buffer(&data);
        if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG")) {
            totalBytes += data.size();
            if (totalBytes > kMaxImageAttachmentBytes) {
                transitionTo(AgentState::Idle);
                emit llmErrorOccurred("Image attachments exceed the 8 MB request limit");
                return;
            }
            msg.images.append({data, QStringLiteral("image/png"), QString()});
        }
    }
    if (msg.images.isEmpty()) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("No valid image attachment");
        return;
    }

    m_conversationHistory.append(msg);
    trimHistoryIfNeeded();
    m_agentTurnCount = 0;
    ++m_turnGeneration;
    m_visionRetryDone = false;
    m_suppressNextLlmError = false;
    m_lastErrorCategory.clear(); // R6：每轮开始清空，避免 category 串用旧错误
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

// ========== LLM Response ==========

void AgentController::onLLMResponse(const AgentResponse& resp) {
    // 阶7 Agent 评审修复（P3-12）：统一经 state() 访问器读状态（带锁），
    // 避免未来跨线程引入后直读成员造成回归
    const AgentState currentState = state();
    if (currentState != AgentState::Thinking) {
        qWarning() << "[AgentController] Unexpected onLLMResponse in state" << stateName(currentState);
        return;
    }
    // R6：成功响应后复位最近错误类别，保证每轮审计只反映本轮错误
    m_lastErrorCategory.clear();

    AgentMessage am;
    am.role = "assistant";
    am.content = resp.content;
    am.toolCalls = resp.toolCalls;
    am.reasoningContent = resp.reasoningContent;
    m_conversationHistory.append(am);
    trimHistoryIfNeeded();

    // D4：token 用量上报（UI 由 T4 接线）
    if (resp.promptTokens > 0 || resp.completionTokens > 0)
        emit llmUsageReported(resp.promptTokens, resp.completionTokens);

    if (!resp.toolCalls.isEmpty()) {
        if (m_permissionLevel == PermissionLevel::Observer) {
            transitionTo(AgentState::Idle);
            emit llmResponseReceived(resp.content, resp.toolCalls);
            return;
        }

        // 权限判定完全由 ToolSchema 元数据驱动（唯一实现 requiresConfirmation）
        bool needsConfirmation = false;
        for (const QJsonValue& v : resp.toolCalls) {
            if (requiresConfirmation(m_permissionLevel, toolAccess(toolCallName(v.toObject())))) {
                needsConfirmation = true;
                break;
            }
        }
        if (needsConfirmation) {
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
    const AgentState currentState = state();
    if (currentState != AgentState::Confirming) {
        qWarning() << "[AgentController] confirmPendingTools called in state" << stateName(currentState);
        return;
    }
    // 连点保护：必须在清空 pending 之前判断
    if (m_dispatchQueued)
        return;
    if (m_pendingToolCalls.isEmpty()) {
        transitionTo(AgentState::Idle);
        return;
    }
    // 排队执行（设计 §5.1）：脱离按钮信号链，避免同步执行阻塞 UI/重入。
    // 生产者的续跑意图必须在此快照；复位后交给排队 lambda 回填。
    const QJsonArray calls = m_pendingToolCalls;
    m_pendingToolCalls = QJsonArray();
    m_dispatchQueued = true;
    m_queuedContinueAfterPending = m_continueAfterPendingTools;
    m_continueAfterPendingTools = true;
    const quint64 turn = m_turnGeneration;
    QMetaObject::invokeMethod(
        this,
        [this, calls, turn] {
            m_dispatchQueued = false;
            if (turn != m_turnGeneration || state() != AgentState::Confirming) {
                // 中止或状态已变化：丢弃本批，不执行、不续跑
                qWarning() << "[AgentController] queued dispatch dropped (turn aborted or state changed)";
                if (state() == AgentState::Confirming)
                    transitionTo(AgentState::Idle);
                return;
            }
            m_continueAfterPendingTools = m_queuedContinueAfterPending;
            doConfirmPendingTools(calls);
        },
        Qt::QueuedConnection);
}

void AgentController::doConfirmPendingTools(QJsonArray calls) {
    const AgentState currentState = state();
    if (currentState != AgentState::Confirming) {
        qWarning() << "[AgentController] doConfirmPendingTools: state changed to" << stateName(currentState);
        return;
    }
    transitionTo(AgentState::Executing);
    if (m_continueAfterPendingTools) {
        extendAgentLoop(calls);
        return;
    }

    QList<QPair<QString, QJsonObject>> tools;
    QList<QString> ids;
    for (const QJsonValue& v : calls) {
        QJsonObject tc = v.toObject();
        const QString name = toolCallName(tc);
        if (!name.isEmpty()) {
            tools.append({name, toolCallParams(tc)});
            ids.append(tc["id"].toString());
        }
    }
    QJsonObject batchResult = m_actor->executeTools(tools, QString("Confirmed Agent tools"));
    QJsonArray resultsArray = batchResult["results"].toArray();
    for (int i = 0; i < resultsArray.size() && i < tools.size(); ++i) {
        QJsonObject result = resultsArray[i].toObject()["result"].toObject();
        emit actionLogEntryAdded(makeAuditEntry(m_actor, tools[i].first, tools[i].second, ids[i], result));
    }
    transitionTo(AgentState::Idle);
}

void AgentController::rejectPendingTools() {
    const AgentState currentState = state();
    if (currentState != AgentState::Confirming) {
        qWarning() << "[AgentController] rejectPendingTools called in state" << stateName(currentState);
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
    const AgentState currentState = state();
    if (currentState != AgentState::Executing) {
        qWarning() << "[AgentController] Unexpected extendAgentLoop in state" << stateName(currentState);
        return;
    }
    // 中止代号：工具执行（可能含嵌套运行等待）返回后据此判定本轮是否已被用户中止
    const quint64 turn = m_turnGeneration;
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

    // 批量执行（等待类工具可能进入嵌套事件循环；期间用户可能按停止）
    QJsonObject batchResult = m_actor->executeTools(tools, QString("Agent turn %1").arg(m_agentTurnCount));

    // 拆分每个 tool 结果，每条 tool call 对应一条独立的 tool role message（OpenAI 要求）
    QJsonArray resultsArray = batchResult["results"].toArray();
    for (int i = 0; i < resultsArray.size() && i < ids.size(); ++i) {
        QJsonObject result = resultsArray[i].toObject()["result"].toObject();
        // makeAuditEntry 读取并剥离内部字段 _elapsedMs（不得进入模型上下文/IPC）
        emit actionLogEntryAdded(makeAuditEntry(m_actor, tools[i].first, tools[i].second, ids[i], result));

        AgentMessage tm;
        tm.role = "tool";
        tm.toolCallId = ids[i];
        tm.content = QString(QJsonDocument(result).toJson(QJsonDocument::Compact));
        m_conversationHistory.append(tm);
    }

    // 中止后仍保留审计与 tool 消息（保持 assistant.tool_calls 配对），但不续跑 LLM
    if (turn != m_turnGeneration || state() != AgentState::Executing) {
        qWarning() << "[AgentController] tool batch finished after abort; dropping continuation";
        return;
    }
    trimHistoryIfNeeded();

    if (batchResult["status"].toString() == "partial_failed") {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred(batchResult["error"].toString("Agent tool execution failed"));
        return;
    }

    // 图像回灌（设计 §6.4）：必须在 partial_failed 早退之后、续跑请求之前
    attachToolImages(resultsArray);

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

void AgentController::attachToolImages(const QJsonArray& results) {
    // 视觉可用性：模型能力 + 会话级降级记忆（设计 §6.5/§6.6）
    const bool visionAvailable = m_llmClient && m_llmClient->supportsImageInput() && !m_visionDisabledForSession;

    QStringList imageLines; // "<path> (<W>x<H>)"，磁盘原图尺寸
    QStringList notices;    // 超限/读取失败的文字说明
    QList<AgentImageAttachment> attachments;
    QSet<QString> seenPaths;
    qint64 totalBytes = 0;

    for (const QJsonValue& value : results) {
        const QJsonObject entry = value.toObject();
        const QString toolName = entry.value(QStringLiteral("tool")).toString();
        if (!isImageProducingTool(toolName))
            continue;
        const QJsonObject result = entry.value(QStringLiteral("result")).toObject();
        const QString path = result.value(QStringLiteral("imagePath")).toString();
        if (path.isEmpty() || seenPaths.contains(path))
            continue;
        if (!QFileInfo::exists(path)) {
            qWarning() << "[AgentController] tool image path does not exist:" << path;
            continue;
        }
        const QImage image(path);
        if (image.isNull()) {
            qWarning() << "[AgentController] failed to read tool image:" << path;
            continue;
        }
        seenPaths.insert(path);
        const int width = image.width();
        const int height = image.height();
        const QString imageLine = QStringLiteral("%1 (%2x%3)").arg(path).arg(width).arg(height);

        if (!visionAvailable) {
            // 视觉关闭：全部产物以文本降级说明（不占图片数量/字节预算，路径不丢）
            imageLines.append(imageLine);
            continue;
        }
        if (attachments.size() >= kMaxToolImagesPerTurn) {
            notices.append(QStringLiteral("- %1 skipped: per-turn image limit reached").arg(path));
            continue;
        }
        QImage scaled = image;
        if (qMax(width, height) > kMaxToolImageLongEdge) {
            scaled = image.scaled(kMaxToolImageLongEdge, kMaxToolImageLongEdge, Qt::KeepAspectRatio,
                                  Qt::SmoothTransformation);
        }
        QByteArray data;
        QBuffer pngBuffer(&data);
        QString mimeType = QStringLiteral("image/png");
        if (pngBuffer.open(QIODevice::WriteOnly) && scaled.save(&pngBuffer, "PNG")) {
            pngBuffer.close();
        } else {
            data.clear();
        }
        if (data.isEmpty() || data.size() > kMaxToolImageBytes) {
            // 降级 PNG→JPEG(q=85)，仍超限则丢弃并写文字说明
            data.clear();
            QBuffer jpegBuffer(&data);
            mimeType = QStringLiteral("image/jpeg");
            if (jpegBuffer.open(QIODevice::WriteOnly) && scaled.save(&jpegBuffer, "JPEG", 85))
                jpegBuffer.close();
            else
                data.clear();
        }
        if (data.isEmpty() || data.size() > kMaxToolImageBytes) {
            notices.append(QStringLiteral("- %1 skipped: image too large to attach").arg(path));
            continue;
        }
        if (totalBytes + data.size() > kMaxImageAttachmentBytes) {
            notices.append(QStringLiteral("- %1 skipped: per-turn image budget exhausted").arg(path));
            continue;
        }
        totalBytes += data.size();
        attachments.append({data, mimeType, path}); // description = 磁盘路径（非视觉链路兜底）
        imageLines.append(imageLine);
    }

    if (imageLines.isEmpty() && notices.isEmpty())
        return; // 无图：不产生空 user 消息，避免破坏 tool_calls 配对

    AgentMessage message;
    message.role = QStringLiteral("user");
    if (visionAvailable && !attachments.isEmpty()) {
        QStringList lines;
        for (const QString& line : imageLines)
            lines.append(QStringLiteral("- %1").arg(line));
        message.content = QStringLiteral("Images produced by the tool calls above (paths kept for reference):\n") +
                          lines.join(QLatin1Char('\n'));
        if (!notices.isEmpty())
            message.content += QLatin1Char('\n') + notices.join(QLatin1Char('\n'));
        message.images = attachments;
    } else {
        // 设计 §6.5：视觉关闭/非视觉模型的唯一文本降级口径（路径必须保留）
        QStringList blocks;
        for (const QString& line : imageLines) {
            blocks.append(QStringLiteral("Tool produced an image, but the configured model does not support image "
                                         "input.\nImage path: %1. If the user needs visual inspection, ask them to "
                                         "enable image input in Agent settings or switch to a vision-capable model.")
                              .arg(line));
        }
        message.content = blocks.join(QLatin1Char('\n'));
        if (!notices.isEmpty())
            message.content += QLatin1Char('\n') + notices.join(QLatin1Char('\n'));
    }
    m_conversationHistory.append(message);
    Logger::instance().addLog(QString("[AgentImages] attached %1 image(s)%2")
                                  .arg(attachments.size())
                                  .arg(visionAvailable ? QString() : QStringLiteral(" (text fallback)")),
                              LogLevel::Info, "Agent");
}

bool AgentController::stripImagesFromLastUserMessage() {
    for (int i = m_conversationHistory.size() - 1; i >= 0; --i) {
        AgentMessage& message = m_conversationHistory[i];
        if (message.role == QStringLiteral("user") && !message.images.isEmpty()) {
            message.images.clear();
            return true;
        }
    }
    return false;
}

void AgentController::trimHistoryIfNeeded() {
    // 仅靠 token 估计裁剪，不设硬消息数上限
    // 每字符 ≈ 0.3 token，每消息 overhead ≈ 20 tokens
    constexpr int maxEstTokens = 256000;

    auto estimateHistoryTokens = [this]() {
        qint64 estimated = m_systemPrompt.length() * 3 / 10;
        for (const auto& m : m_conversationHistory) {
            estimated += m.content.length() * 3 / 10 + 20;
            if (!m.reasoningContent.isEmpty())
                estimated += m.reasoningContent.length() * 3 / 10;
            for (const AgentImageAttachment& image : m.images)
                estimated += (image.data.size() + 2) / 3;
        }
        return estimated;
    };

    auto trimLeadingPartialTurn = [this]() {
        while (!m_conversationHistory.isEmpty() && m_conversationHistory.first().role != "user")
            m_conversationHistory.removeFirst();
    };

    trimLeadingPartialTurn();
    auto dropOldestTurn = [this, &trimLeadingPartialTurn]() {
        int secondUserIdx = -1, userCount = 0;
        for (int i = 0; i < m_conversationHistory.size(); ++i)
            if (m_conversationHistory[i].role == "user" && ++userCount == 2) {
                secondUserIdx = i;
                break;
            }
        if (secondUserIdx < 0)
            return false;
        m_conversationHistory = m_conversationHistory.mid(secondUserIdx);
        trimLeadingPartialTurn();
        return true;
    };

    while (estimateHistoryTokens() > maxEstTokens) {
        if (!dropOldestTurn())
            break;
    }

    constexpr int kMaxMessages = 64;
    while (m_conversationHistory.size() > kMaxMessages) {
        if (dropOldestTurn())
            continue;
        if (m_conversationHistory.size() <= 1)
            break;

        // A single user turn can contain many assistant/tool exchanges. Remove
        // the oldest complete exchange so tool results never become orphaned.
        m_conversationHistory.removeAt(1);
        while (m_conversationHistory.size() > 1 && m_conversationHistory[1].role == QStringLiteral("tool"))
            m_conversationHistory.removeAt(1);
    }
}

void AgentController::onGuiEvent(const GuiEvent& event) {
    // AgentObserver DEBUG 镜像日志过于冗余，降级为不输出
    Q_UNUSED(event)
}

QJsonObject AgentController::handleToolCall(const QString& toolName, const QJsonObject& params) {
    if (state() != AgentState::Idle)
        return {{"error", "Agent is busy processing another request"}};
    if (!ToolSchema::instance().hasTool(toolName)) {
        // R7：未注册工具的早退分支同样不得把内部字段 _elapsedMs 泄漏到 IPC 响应体
        QJsonObject result = m_actor->executeTool(toolName, params);
        takeElapsedMs(result);
        return result;
    }

    const ToolAccess access = toolAccess(toolName);
    if (m_permissionLevel == PermissionLevel::Observer && !access.readOnly)
        return {{"error", "Permission denied: Observer mode"}};
    if (requiresConfirmation(m_permissionLevel, access)) {
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
    // makeAuditEntry 读取并剥离 _elapsedMs，返回给 IPC/调用方的 result 不再含内部字段
    AgentActionLogEntry entry = makeAuditEntry(m_actor, toolName, params, QString(), result);
    entry.undoable = entry.undoable && !result.contains("error");
    emit actionLogEntryAdded(entry);
    return result;
}

bool AgentController::toolRequiresConfirmation(PermissionLevel level, const QString& toolName) {
    return requiresConfirmation(level, toolAccess(toolName));
}

void AgentController::onLLMErrorClassified(const LLMErrorInfo& info) {
    m_lastErrorCategory = llmErrorCategoryName(info.category);
    Logger::instance().addLog(QString("[LLMError] %1 http=%2 attempts=%3 %4")
                                  .arg(llmErrorCategoryName(info.category))
                                  .arg(info.httpStatus)
                                  .arg(info.attempts)
                                  .arg(info.message),
                              LogLevel::Warning, "Agent");

    // 视觉降级自动重试（设计 §6.6）：模型拒绝图片时去图重发一次，之后不再尝试
    if (!info.visionDowngradeHint || m_visionRetryDone || !m_llmClient)
        return;
    if (state() != AgentState::Thinking)
        return;
    if (!stripImagesFromLastUserMessage()) {
        qWarning() << "[AgentController] vision rejection without an image message; not retrying";
        return;
    }
    m_visionRetryDone = true;
    m_visionDisabledForSession = true;
    // 本次失败由我们重试，抑制紧随其后的 onLLMError（同一次失败只处理一次）
    m_suppressNextLlmError = true;
    Logger::instance().addLog("[AgentController] model rejected image input; retrying without images",
                              LogLevel::Warning, "Agent");
    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

void AgentController::onLLMError(const QString& error) {
    // 视觉降级重发路径：这一次 errorOccurred 是触发重试的同一次失败，不得收口状态机
    if (m_suppressNextLlmError) {
        m_suppressNextLlmError = false;
        m_lastErrorCategory.clear();
        return;
    }
    m_pendingToolCalls = QJsonArray();
    transitionTo(AgentState::Idle);
    // 先 emit 让 UI 审计读到本次分类（直连槽同步执行），随后复位避免串用旧类别
    emit llmErrorOccurred(error);
    m_lastErrorCategory.clear();
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
        "6. Be concise. Industrial users prefer direct answers.\n"
        "7. Before running a flow you just modified, call validate_workflow. If ok=false, fix the reported issues "
        "instead of running.\n"
        "8. run_flow returns only after the run finishes (wait defaults to true). Do not claim a run succeeded "
        "without checking its success/elapsedMs/failedModule fields.\n"
        "9. After a run, use get_node_runtime_outputs to read the actual numbers you need (never guess module "
        "outputs).\n"
        "10. When the user asks about image quality/content, call capture_viewport_image or export_node_output and "
        "look at the returned image. At most 2 tool images are attached per turn.\n"
        "11. If a tool returns an error with a \"code\", read the code and adapt; do not repeat the same failing "
        "call.\n");
}

} // namespace DeepLux
