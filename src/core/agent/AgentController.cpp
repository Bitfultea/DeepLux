#include "AgentController.h"

#include "AgentActor.h"
#include "AgentAuditLog.h"
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
#include <QTimer>

namespace DeepLux {

namespace {
constexpr int kMaxAgentToolTurns = 20;
constexpr int kMaxToolCallsPerTurn = 10;
constexpr int kMaxImageAttachments = 4;
constexpr qint64 kMaxImageAttachmentBytes = 8LL * 1024 * 1024;
// 流式增量合批间隔（设计 §5.2：唯一合批点在控制器）
constexpr int kStreamFlushIntervalMs = 50;

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

// 审计结果单源：跳过/拒绝与成功/失败可区分，且绝不用 contains("error") 猜测。
QString auditOutcome(const QJsonObject& result) {
    if (result.value(QStringLiteral("skipped")).toBool())
        return QStringLiteral("skipped");
    const QString code = result.value(QStringLiteral("code")).toString();
    if (code == QStringLiteral("PERMISSION_DENIED") || code == QStringLiteral("USER_REJECTED") ||
        code == QStringLiteral("USER_CANCELLED") || code == QStringLiteral("confirmation_expired"))
        return QStringLiteral("rejected");
    if (code == QStringLiteral("cancelled") || code == QStringLiteral("project_changed"))
        return QStringLiteral("skipped");
    return AgentActor::resultSucceeded(result) ? QStringLiteral("success") : QStringLiteral("error");
}

AgentActionLogEntry makeAuditEntry(const AgentActor* actor, const QString& toolName, const QJsonObject& params,
                                   const QString& toolCallId, QJsonObject& result) {
    AgentActionLogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.actor = "Agent";
    entry.action = toolName;
    entry.params = QString(QJsonDocument(params).toJson(QJsonDocument::Compact));
    entry.durationMs = takeElapsedMs(result);
    entry.result = auditOutcome(result);
    // R1：失败/跳过的工具不会压入 QUndoStack，审计 undoable 必须同步为 false
    entry.undoable = isAgentUndoable(actor, toolName) && AgentActor::resultSucceeded(result);
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

void appendToolResultsForCalls(QList<AgentMessage>& history, const QJsonArray& calls, const QString& code) {
    for (const QJsonValue& value : calls) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        AgentMessage result;
        result.role = QStringLiteral("tool");
        result.toolCallId = id;
        result.content = QString::fromUtf8(QJsonDocument(QJsonObject{{"error", code}}).toJson(QJsonDocument::Compact));
        history.append(result);
    }
}
} // namespace

AgentController::AgentController(QObject* parent)
    : QObject(parent), m_observer(new AgentObserver(this)), m_actor(new AgentActor(this)),
      m_streamFlushTimer(new QTimer(this)) {
    // 唯一的流式合批点（设计 §5.2）：约 50ms 一批，禁止 UI 层再叠一层定时器
    m_streamFlushTimer->setSingleShot(true);
    m_streamFlushTimer->setTimerType(Qt::CoarseTimer);
    m_streamFlushTimer->setInterval(kStreamFlushIntervalMs);
    connect(m_streamFlushTimer, &QTimer::timeout, this, &AgentController::flushStreamPending);

    // B2：工程会话身份/版本。任何切换/关闭都使旧确认失效；结构变化递增版本。
    ProjectManager& projects = ProjectManager::instance();
    connect(&projects, &ProjectManager::projectCreated, this, [this](Project* project) {
        ++m_projectSessionId;
        ++m_projectRevision;
        bindProjectSignals(project);
        onProjectStructureChanged();
    });
    connect(&projects, &ProjectManager::projectOpened, this, [this](Project* project) {
        ++m_projectSessionId;
        ++m_projectRevision;
        bindProjectSignals(project);
        onProjectStructureChanged();
    });
    connect(&projects, &ProjectManager::projectClosed, this, [this]() {
        ++m_projectSessionId;
        ++m_projectRevision;
        bindProjectSignals(nullptr);
        onProjectStructureChanged();
    });
    bindProjectSignals(projects.currentProject());
}

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
    connect(m_actor, &AgentActor::toolError, this, [this](const QString& t, const QString& e) {
        emit agentActionCompleted(t, {{"error", e}});
    });
    m_systemPrompt = ConfigManager::instance().groupString("agent", "systemPrompt", defaultSystemPrompt());
    m_initialized = true;
    // 第二迭代：审计 JSONL 落盘（设计 §3.3）——通过信号接线，覆盖所有审计发射点
    AgentAuditLog::instance().initialize(this);
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
    AgentAuditLog::instance().shutdown();
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
        // 配置级流式开关（agent/streamEnabled，默认 true；设计 §4.1）
        if (ConfigManager::instance().isInitialized())
            m_llmClient->setStreamEnabled(ConfigManager::instance().groupBool("agent", "streamEnabled", true));
        connect(m_llmClient, &ILLMClient::responseReceived, this, &AgentController::onLLMResponse,
                Qt::QueuedConnection);
        connect(m_llmClient, &ILLMClient::errorOccurred, this, &AgentController::onLLMError, Qt::QueuedConnection);
        connect(m_llmClient, &ILLMClient::errorOccurredClassified, this, &AgentController::onLLMErrorClassified,
                Qt::QueuedConnection);
        // 流式信号（设计 §5.1）：UI 只与控制器交互
        connect(m_llmClient, &ILLMClient::streamStarted, this, &AgentController::onStreamStarted, Qt::QueuedConnection);
        connect(m_llmClient, &ILLMClient::streamContentDelta, this, &AgentController::onStreamContentDelta,
                Qt::QueuedConnection);
        connect(m_llmClient, &ILLMClient::streamReasoningDelta, this, &AgentController::onStreamReasoningDelta,
                Qt::QueuedConnection);
        connect(m_llmClient, &ILLMClient::streamFinished, this, &AgentController::onStreamFinished,
                Qt::QueuedConnection);
        connect(m_llmClient, &ILLMClient::streamFailed, this, &AgentController::onStreamFailed, Qt::QueuedConnection);
        connect(m_llmClient, &ILLMClient::streamFallback, this, &AgentController::onStreamFallback,
                Qt::QueuedConnection);
        // 客户端可能先于控制器单例析构（测试/切换设置）：清空悬空指针，setLLMClient(nullptr) 安全
        connect(m_llmClient, &QObject::destroyed, this, [this]() { m_llmClient = nullptr; });
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
    m_lastResponseReasoning.clear();
    m_turnTimer.invalidate();
    resetStreamBuffer();
    transitionTo(AgentState::Idle);
}

void AgentController::endTurnWithReason(const QString& responseText, const QString& errorText,
                                        bool suppressClientError) {
    if (state() == AgentState::Idle)
        return;
    const bool chatConfirmation = state() == AgentState::Confirming &&
                                  (m_dispatchQueued ? m_queuedContinueAfterPending : m_continueAfterPendingTools);
    if (chatConfirmation) {
        QJsonArray calls = m_pendingToolCalls;
        if (calls.isEmpty() && !m_conversationHistory.isEmpty() &&
            m_conversationHistory.constLast().role == QLatin1String("assistant"))
            calls = m_conversationHistory.constLast().toolCalls;
        appendToolResultsForCalls(m_conversationHistory, calls, QStringLiteral("USER_CANCELLED"));
    }
    // 作废排队任务与索引：turnGeneration 让已入队的 dispatch/续跑全部失效
    ++m_turnGeneration;
    m_pendingToolCalls = QJsonArray();
    m_continueAfterPendingTools = true;
    m_dispatchQueued = false;
    m_suppressNextLlmError = suppressClientError;
    // 先把已收到的流式内容 flush 给 UI（设计 §6.1：保留已收内容）
    flushStreamPending();
    if (m_llmClient)
        m_llmClient->abortRequest();
    if (m_actor)
        m_actor->abortRunningTool();
    transitionTo(AgentState::Idle);
    // 流式中止信号必须先于 agentTurnAborted（UI 先标记气泡，再复位状态条）
    if (m_streamActive) {
        m_streamActive = false;
        emit agentStreamAborted();
    }
    emit llmResponseReceived(responseText, {});
    if (!errorText.isEmpty()) {
        m_lastErrorCategory = llmErrorCategoryName(LLMErrorCategory::Timeout);
        emit llmErrorOccurred(errorText);
    }
    m_turnTimer.invalidate();
    emit agentTurnAborted();
}

void AgentController::abortCurrentTurn() {
    if (state() == AgentState::Idle) {
        // 幂等：Idle 下不做任何事（不报错、不发信号）
        qWarning() << "[AgentController] abortCurrentTurn ignored in Idle state";
        return;
    }
    endTurnWithReason(QStringLiteral("Stopped by user."), QString(), true);
}

int AgentController::turnDeadlineMs() const {
    const int configured = m_turnDeadlineConfigOverride >= 0
                               ? m_turnDeadlineConfigOverride
                               : (ConfigManager::instance().isInitialized()
                                      ? ConfigManager::instance().groupInt("agent", "turnDeadlineMs", 600000)
                                      : 600000);
    return qBound(10000, configured, 3600000);
}

void AgentController::setTurnDeadlineMsForTest(int ms) {
    m_turnDeadlineOverride = ms;
}

void AgentController::setTurnDeadlineConfigForTest(int ms) {
    m_turnDeadlineConfigOverride = ms;
}

bool AgentController::turnDeadlineExceeded() const {
    if (!m_turnTimer.isValid())
        return false;
    const int deadline = m_turnDeadlineOverride >= 0 ? m_turnDeadlineOverride : turnDeadlineMs();
    return deadline > 0 && m_turnTimer.elapsed() >= deadline;
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
    constexpr int kMaxBuildContextChars = 16384;
    Project* proj = ProjectManager::instance().currentProject();
    const int threshold = flowSummaryThreshold();
    const int previewCount = flowSummaryPreviewCount();

    auto moduleLine = [proj, threshold, previewCount](bool forceSummary) -> QString {
        const int count = proj->modules().size();
        const bool summarize = forceSummary || count > threshold;
        if (!summarize) {
            QStringList modNames;
            for (const ModuleInstance& m : proj->modules())
                modNames.append(QString("%1(id=%2)").arg(m.moduleId).arg(m.id));
            return QStringLiteral("- Modules (%1): %2").arg(count).arg(modNames.join(", "));
        }
        QStringList preview;
        int listed = 0;
        for (const ModuleInstance& m : proj->modules()) {
            if (listed++ >= previewCount)
                break;
            preview.append(QString("%1(id=%2)").arg(m.moduleId).arg(m.id));
        }
        return QStringLiteral("- Modules (%1): %2, … (+%3 more; call get_flow_state for the full list)")
            .arg(count)
            .arg(preview.join(", "))
            .arg(qMax(0, count - previewCount));
    };

    auto build = [this, proj, &moduleLine](bool forceSummary) -> QString {
        QString text = m_systemPrompt;
        if (proj) {
            text += QStringLiteral("\n\n## Current State\n- Project: %1\n").arg(proj->name());
            text += moduleLine(forceSummary);
            text += QStringLiteral("\n- Connections: %1\n- RunEngine: %2\n")
                        .arg(proj->connections().size())
                        .arg(RunEngine::instance().isRunning() ? QStringLiteral("Running") : QStringLiteral("Idle"));
        } else {
            text += QStringLiteral("\n\n## Current State\n- No project opened.\n");
        }
        const QList<GuiEvent> recent = m_observer->recentEvents(5); // 保持 ≤5 条（设计 §8.1）
        if (!recent.isEmpty()) {
            text += QStringLiteral("\n## Recent Events\n");
            for (const GuiEvent& e : recent)
                text += QStringLiteral("[%1] %2\n").arg(e.timestamp.toString("hh:mm:ss")).arg(e.typeString());
        }
        return text;
    };

    QString ctx = build(false);
    if (proj && ctx.size() > kMaxBuildContextChars) {
        // 超长工程：先把模块行替换为摘要（即使未过阈值），并标注上下文已被裁剪
        ctx = build(true);
        ctx += QStringLiteral("\n…[context truncated]\n");
        if (ctx.size() > kMaxBuildContextChars) {
            const QString marker = QStringLiteral("…[context truncated]");
            ctx = ctx.left(kMaxBuildContextChars - marker.size() - 1) + QLatin1Char('\n') + marker;
        }
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
    m_turnTimer.start(); // L9：单轮 deadline 起点
    ++m_turnGeneration;  // 新的用户轮次：作废上一轮残留的排队/续跑
    m_visionRetryDone = false;
    m_suppressNextLlmError = false;
    m_lastErrorCategory.clear(); // R6：每轮开始清空，避免 category 串用旧错误
    sendPreparedContext();
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
    m_turnTimer.start(); // L9：单轮 deadline 起点
    ++m_turnGeneration;
    m_visionRetryDone = false;
    m_suppressNextLlmError = false;
    m_lastErrorCategory.clear(); // R6：每轮开始清空，避免 category 串用旧错误
    sendPreparedContext();
}

// ========== 工程会话身份 / 确认有效期（B2） ==========

void AgentController::bindProjectSignals(Project* project) {
    for (const QMetaObject::Connection& connection : m_projectConnections)
        QObject::disconnect(connection);
    m_projectConnections.clear();
    if (!project)
        return;

    const auto bump = [this]() {
        ++m_projectRevision;
        onProjectStructureChanged();
    };
    m_projectConnections << connect(project, &Project::nameChanged, this, [bump](const QString&) { bump(); })
                         << connect(project, &Project::moduleAdded, this, [bump](const ModuleInstance&) { bump(); })
                         << connect(project, &Project::moduleUpdated, this, [bump](const ModuleInstance&) { bump(); })
                         << connect(project, &Project::moduleRemoved, this, [bump](const QString&) { bump(); })
                         << connect(project, &Project::connectionAdded, this,
                                    [bump](const ModuleConnection&) { bump(); })
                         << connect(project, &Project::connectionRemoved, this,
                                    [bump](const QString&, const QString&) { bump(); })
                         << connect(project, &Project::connectionRemovedWithPorts, this,
                                    [bump](const QString&, const QString&, const QString&, const QString&) { bump(); })
                         << connect(project, &Project::dataSourceAdded, this, [bump](const DataSource&) { bump(); })
                         << connect(project, &Project::dataSourceRemoved, this, [bump](const QString&) { bump(); });
}

void AgentController::onProjectStructureChanged() {
    // 确认期间工程被切换/关闭/外部修改：旧确认立即失效，且不执行任何工程编辑。
    if (state() == AgentState::Confirming)
        expirePendingConfirmation(QStringLiteral("project_changed"));
}

void AgentController::snapshotConfirmation() {
    m_confirmationSessionId = m_projectSessionId;
    m_confirmationRevision = m_projectRevision;
}

bool AgentController::confirmationExpired() const {
    return m_confirmationSessionId != m_projectSessionId || m_confirmationRevision != m_projectRevision;
}

void AgentController::expirePendingConfirmation(const QString& reason) {
    QJsonArray calls = m_pendingToolCalls;
    if (calls.isEmpty() && !m_conversationHistory.isEmpty() &&
        m_conversationHistory.constLast().role == QLatin1String("assistant"))
        calls = m_conversationHistory.constLast().toolCalls;

    const bool continueAfter = m_dispatchQueued ? m_queuedContinueAfterPending : m_continueAfterPendingTools;
    if (continueAfter) {
        for (const QJsonValue& value : calls) {
            const QJsonObject call = value.toObject();
            const QString id = call.value(QStringLiteral("id")).toString();
            if (id.isEmpty())
                continue;
            const QJsonObject denied{{QStringLiteral("success"), false},
                                     {QStringLiteral("error"), QStringLiteral("Confirmation expired: %1").arg(reason)},
                                     {QStringLiteral("code"), QStringLiteral("confirmation_expired")}};
            AgentMessage message;
            message.role = QStringLiteral("tool");
            message.toolCallId = id;
            message.content = QString(QJsonDocument(denied).toJson(QJsonDocument::Compact));
            m_conversationHistory.append(message);
        }
    }

    ++m_turnGeneration; // 作废已排队的确认/续跑
    m_pendingToolCalls = QJsonArray();
    m_dispatchQueued = false;
    m_continueAfterPendingTools = true;
    transitionTo(AgentState::Idle);
    emit llmErrorOccurred(QStringLiteral("Confirmation expired: %1; no project changes were made").arg(reason));
}

bool AgentController::executeObserverCalls(const QJsonArray& calls, bool continueAfter) {
    bool denied = false;
    for (const QJsonValue& value : calls) {
        const QJsonObject call = value.toObject();
        const QString name = toolCallName(call);
        if (name.isEmpty())
            continue;
        const QString id = call.value(QStringLiteral("id")).toString();
        const QJsonObject params = toolCallParams(call);

        QJsonObject result;
        if (!toolAccess(name).readOnly) {
            denied = true;
            result = QJsonObject{{QStringLiteral("success"), false},
                                 {QStringLiteral("error"), QStringLiteral("Permission denied: Observer mode")},
                                 {QStringLiteral("code"), QStringLiteral("PERMISSION_DENIED")}};
        } else {
            result = m_actor->executeTool(name, params);
        }

        emit actionLogEntryAdded(makeAuditEntry(m_actor, name, params, id, result));
        if (continueAfter && !id.isEmpty()) {
            AgentMessage message;
            message.role = QStringLiteral("tool");
            message.toolCallId = id;
            message.content =
                truncateToolResult(QString(QJsonDocument(result).toJson(QJsonDocument::Compact)), toolResultMaxChars());
            m_conversationHistory.append(message);
        }
    }
    return denied;
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
    // 流式：先 flush 残留缓冲，保证 UI 收到的文本 = 写入历史的文本（§5.1 顺序约定）
    flushStreamPending();
    m_streamActive = false;
    // R1：暴露最近一次响应 reasoning（只读访问器），供非流式/回退分支展示
    m_lastResponseReasoning = resp.reasoningContent;
    // R6：成功响应后复位最近错误类别，保证每轮审计只反映本轮错误
    m_lastErrorCategory.clear();

    // 输出被 max_tokens 截断：状态条提示（§3.4 / §5.4）+ 审计 result="truncated"（R2）
    if (resp.truncatedByMaxTokens) {
        Logger::instance().addLog(QStringLiteral("[LLM] response truncated by max_tokens"), LogLevel::Warning, "Agent");
        AgentActionLogEntry truncatedEntry;
        truncatedEntry.timestamp = QDateTime::currentDateTime();
        truncatedEntry.actor = QStringLiteral("Agent");
        truncatedEntry.action = QStringLiteral("LLM response truncated");
        truncatedEntry.result = QStringLiteral("truncated");
        truncatedEntry.undoable = false;
        truncatedEntry.errorCategory = llmErrorCategoryName(LLMErrorCategory::None);
        emit actionLogEntryAdded(truncatedEntry);
        emit agentStreamTruncated();
    }

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
            bool hasWrite = false;
            for (const QJsonValue& v : resp.toolCalls) {
                if (!toolAccess(toolCallName(v.toObject())).readOnly) {
                    hasWrite = true;
                    break;
                }
            }
            if (!hasWrite) {
                // 纯只读批次：正常执行并续跑，Observer 仍可完成查询型任务
                transitionTo(AgentState::Executing);
                extendAgentLoop(resp.toolCalls);
                return;
            }
            // 混合批次：允许的只读调用逐项执行，写入项生成结构化拒绝结果；
            // 任何拒绝都结束本轮（不续跑）。
            transitionTo(AgentState::Executing);
            emit llmResponseReceived(resp.content, resp.toolCalls);
            executeObserverCalls(resp.toolCalls, true);
            trimHistoryIfNeeded();
            transitionTo(AgentState::Idle);
            emit llmErrorOccurred(QStringLiteral("Permission denied: Observer mode"));
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
            snapshotConfirmation();
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
            if (confirmationExpired()) {
                expirePendingConfirmation(QStringLiteral("project_changed"));
                return;
            }
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
    // 确认按钮最终复核：工程会话/版本变化则整批失效，零工程修改
    if (confirmationExpired()) {
        expirePendingConfirmation(QStringLiteral("project_changed"));
        return;
    }
    if (m_permissionLevel == PermissionLevel::Observer) {
        // 等待期间降权：只读项仍可执行，写入项全部拒绝（不因批次含读工具放行写工具）
        transitionTo(AgentState::Executing);
        const bool denied = executeObserverCalls(calls, m_continueAfterPendingTools);
        m_continueAfterPendingTools = true;
        transitionTo(AgentState::Idle);
        if (denied)
            emit llmErrorOccurred(QStringLiteral("Permission denied: Observer mode"));
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
    if (m_continueAfterPendingTools)
        appendToolResultsForCalls(m_conversationHistory, m_pendingToolCalls, QStringLiteral("USER_REJECTED"));
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
    // L9 检查点 1：执行工具批之前
    if (turnDeadlineExceeded()) {
        const int deadline = m_turnDeadlineOverride >= 0 ? m_turnDeadlineOverride : turnDeadlineMs();
        const QString message = QStringLiteral("已超过单轮总时长上限（%1 ms），本轮已中止").arg(deadline);
        endTurnWithReason(message, message, false);
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

    // 批量执行（等待类工具可能进入嵌套事件循环；期间用户可能按停止/切换工程/降权）。
    // 每项执行前复核取消、工程会话与权限，旧批次不得继续写入。
    const quint64 session = m_projectSessionId;
    const PermissionLevel batchPermission = m_permissionLevel;
    const auto gate = [this, turn, session, batchPermission](int, const QString& name, const QJsonObject&) -> QString {
        if (turn != m_turnGeneration)
            return QStringLiteral("cancelled");
        if (session != m_projectSessionId)
            return QStringLiteral("project_changed");
        const ToolAccess access = toolAccess(name);
        if (m_permissionLevel == PermissionLevel::Observer && !access.readOnly)
            return QStringLiteral("PERMISSION_DENIED");
        if (m_permissionLevel != batchPermission && requiresConfirmation(m_permissionLevel, access) &&
            !requiresConfirmation(batchPermission, access))
            return QStringLiteral("PERMISSION_DENIED");
        return QString();
    };
    QJsonObject batchResult = m_actor->executeTools(tools, QString("Agent turn %1").arg(m_agentTurnCount), gate);

    // 拆分每个 tool 结果，每条 tool call 对应一条独立的 tool role message（OpenAI 要求）。
    // executeTools 保证 results 与 tools 一一对应；未执行项以 skipped_after_failure 补齐。
    QJsonArray resultsArray = batchResult["results"].toArray();
    for (int i = 0; i < resultsArray.size() && i < ids.size(); ++i) {
        QJsonObject result = resultsArray[i].toObject()["result"].toObject();
        // makeAuditEntry 读取并剥离内部字段 _elapsedMs（不得进入模型上下文/IPC）
        emit actionLogEntryAdded(makeAuditEntry(m_actor, tools[i].first, tools[i].second, ids[i], result));

        AgentMessage tm;
        tm.role = "tool";
        tm.toolCallId = ids[i];
        // 设计 §7.4：唯一截断点——只影响回传模型的上下文，审计/UI 仍用完整结果
        tm.content =
            truncateToolResult(QString(QJsonDocument(result).toJson(QJsonDocument::Compact)), toolResultMaxChars());
        m_conversationHistory.append(tm);
    }

    // 中止后仍保留审计与 tool 消息（保持 assistant.tool_calls 配对），但不续跑 LLM
    if (turn != m_turnGeneration || state() != AgentState::Executing) {
        qWarning() << "[AgentController] tool batch finished after abort; dropping continuation";
        return;
    }
    // L9 检查点 2：等待类工具可能跨过 deadline
    if (turnDeadlineExceeded()) {
        const int deadline = m_turnDeadlineOverride >= 0 ? m_turnDeadlineOverride : turnDeadlineMs();
        const QString message = QStringLiteral("已超过单轮总时长上限（%1 ms），本轮已中止").arg(deadline);
        endTurnWithReason(message, message, false);
        return;
    }
    trimHistoryIfNeeded();

    // B1/B2：run_failed 允许重新规划；create/open 成功切换工程后保守终止旧批次，
    // 同样回灌结果让模型读取新工程再规划。其余失败结束本轮。
    const bool batchFailed = batchResult.value(QStringLiteral("status")).toString() == QLatin1String("partial_failed");
    const QString failedCode = batchResult.value(QStringLiteral("failedCode")).toString();
    const bool canReplan =
        batchFailed && (failedCode == QLatin1String("run_failed") || failedCode == QLatin1String("project_switched"));
    if (batchFailed && !canReplan) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred(
            batchResult.value(QStringLiteral("error")).toString(QStringLiteral("Agent tool execution failed")));
        return;
    }

    // 图像回灌（设计 §6.4）：正常续跑与 run_failed 重规划前都保留可诊断产物
    attachToolImages(resultsArray);

    Logger::instance().addLog(QString("[AgentLoop] Executed %1 tool(s)").arg(resultsArray.size()), LogLevel::Debug,
                              "Agent");

    // L9 检查点 3：续跑 LLM 请求之前
    if (turnDeadlineExceeded()) {
        const int deadline = m_turnDeadlineOverride >= 0 ? m_turnDeadlineOverride : turnDeadlineMs();
        const QString message = QStringLiteral("已超过单轮总时长上限（%1 ms），本轮已中止").arg(deadline);
        endTurnWithReason(message, message, false);
        return;
    }

    // 🔁 继续 LLM 请求
    emit agentLoopIterating();
    if (!m_llmClient) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("LLM disconnected during agent loop");
        return;
    }
    transitionTo(AgentState::Thinking);
    sendPreparedContext();
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
    message.internalAutoInjected = true; // 属于原用户任务，不产生新的任务边界
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

// ========== 上下文治理（设计 §7/§8） ==========

int AgentController::contextBudgetTokens() const {
    if (m_contextBudgetOverride >= 0)
        return m_contextBudgetOverride;
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "contextBudgetTokens", 128000)
                               : 128000;
    return qBound(16000, configured, 1000000);
}

int AgentController::toolResultMaxChars() const {
    if (m_toolResultMaxCharsOverride >= 0)
        return m_toolResultMaxCharsOverride;
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "toolResultMaxChars", 6000)
                               : 6000;
    return qBound(1000, configured, 64000);
}

int AgentController::flowSummaryThreshold() const {
    if (m_summaryThresholdOverride >= 0)
        return m_summaryThresholdOverride;
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "flowSummaryThreshold", 24)
                               : 24;
    return qBound(1, configured, 1000);
}

int AgentController::flowSummaryPreviewCount() const {
    if (m_summaryPreviewOverride >= 0)
        return m_summaryPreviewOverride;
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "flowSummaryPreviewCount", 24)
                               : 24;
    return qBound(1, configured, 200);
}

void AgentController::setContextLimitsForTest(int budgetTokens, int toolResultMaxChars, int summaryThreshold,
                                              int summaryPreviewCount) {
    if (budgetTokens >= 0)
        m_contextBudgetOverride = qBound(16000, budgetTokens, 1000000);
    if (toolResultMaxChars >= 0)
        m_toolResultMaxCharsOverride = qBound(1000, toolResultMaxChars, 64000);
    if (summaryThreshold >= 0)
        m_summaryThresholdOverride = qBound(1, summaryThreshold, 1000);
    if (summaryPreviewCount >= 0)
        m_summaryPreviewOverride = qBound(1, summaryPreviewCount, 200);
}

QString AgentController::truncateToolResult(const QString& json, int maxChars) {
    maxChars = qBound(1000, maxChars, 64000);
    if (json.size() <= maxChars)
        return json;
    const int head = maxChars * 2 / 3;
    const int tail = maxChars - head;
    return json.left(head) + QStringLiteral("\n…[truncated %1 chars]…\n").arg(json.size() - maxChars) +
           json.right(tail);
}

bool AgentController::echoesReasoning() const {
    if (!ConfigManager::instance().isInitialized())
        return false;
    return ConfigManager::instance().groupString("agent", "reasoningEcho", "none").trimmed().toLower() !=
           QStringLiteral("none");
}

qint64 AgentController::estimateTokensFor(const QList<AgentMessage>& messages, const QString& systemPrompt,
                                          bool countReasoning) const {
    // 每字符 ≈0.3 token，每消息 +20；reasoning 仅在回传时计入；图片按字节估算
    qint64 estimated = static_cast<qint64>(systemPrompt.length()) * 3 / 10;
    for (const AgentMessage& message : messages) {
        estimated += static_cast<qint64>(message.content.length()) * 3 / 10 + 20;
        if (!message.toolCalls.isEmpty()) {
            // tool_calls 的参数同样回传模型，必须计入预算
            estimated +=
                static_cast<qint64>(QJsonDocument(message.toolCalls).toJson(QJsonDocument::Compact).size()) * 3 / 10;
        }
        if (countReasoning && !message.reasoningContent.isEmpty())
            estimated += static_cast<qint64>(message.reasoningContent.length()) * 3 / 10;
        for (const AgentImageAttachment& image : message.images)
            estimated += (image.data.size() + 2) / 3;
    }
    return estimated;
}

qint64 AgentController::estimateHistoryTokens() {
    // 真实发送的系统提示是 buildContext()（包含 m_systemPrompt 与当前工程摘要）
    return estimateTokensFor(m_conversationHistory, buildContext(), echoesReasoning());
}

void AgentController::repairOrphanToolGroups() {
    QList<AgentMessage> repaired;
    repaired.reserve(m_conversationHistory.size());
    for (int i = 0; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role == QStringLiteral("tool")) {
            // I1：tool 必须属于前一条 assistant(tool_calls) 的某个 id
            int assistantIndex = repaired.size() - 1;
            while (assistantIndex >= 0 && repaired.at(assistantIndex).role == QLatin1String("tool"))
                --assistantIndex;
            if (assistantIndex < 0 || repaired.at(assistantIndex).role != QLatin1String("assistant") ||
                repaired.at(assistantIndex).toolCalls.isEmpty()) {
                qWarning() << "[AgentController] dropping orphan tool message" << message.toolCallId;
                continue;
            }
            QSet<QString> ids;
            for (const QJsonValue& call : repaired.at(assistantIndex).toolCalls)
                ids.insert(call.toObject().value(QStringLiteral("id")).toString());
            bool duplicate = false;
            for (int j = assistantIndex + 1; j < repaired.size(); ++j)
                duplicate = duplicate || repaired.at(j).toolCallId == message.toolCallId;
            if (!ids.contains(message.toolCallId) || duplicate) {
                qWarning() << "[AgentController] dropping unmatched tool message" << message.toolCallId;
                continue;
            }
            repaired.append(message);
            continue;
        }
        if (message.role == QStringLiteral("assistant") && !message.toolCalls.isEmpty()) {
            // 末尾的 assistant(tool_calls) 是"待执行"的合法状态（onLLMResponse 在工具
            // 执行前会 trim）：保留，等工具结果写入后下一次裁剪再校验完整性。
            if (i + 1 >= m_conversationHistory.size()) {
                repaired.append(message);
                continue;
            }
            // I2：assistant(tool_calls) 的每个 id 都必须有紧邻的 tool 响应，否则整组移除
            QSet<QString> ids;
            for (const QJsonValue& call : message.toolCalls) {
                const QString id = call.toObject().value(QStringLiteral("id")).toString();
                if (!id.isEmpty())
                    ids.insert(id);
            }
            QSet<QString> found;
            int j = i + 1;
            for (; j < m_conversationHistory.size() && m_conversationHistory.at(j).role == QStringLiteral("tool");
                 ++j) {
                found.insert(m_conversationHistory.at(j).toolCallId);
            }
            if (!ids.isEmpty() && !(ids - found).isEmpty()) {
                qWarning() << "[AgentController] dropping incomplete tool group at index" << i;
                i = j - 1; // 跳过该 assistant 及其全部 tool 响应
                continue;
            }
            repaired.append(message);
            continue;
        }
        repaired.append(message);
    }
    m_conversationHistory = repaired;
}

QSet<int> AgentController::protectedHistoryIndices() const {
    QSet<int> protectedIndices;
    int lastRealUser = -1;
    for (int i = 0; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role == QStringLiteral("user") && !message.internalAutoInjected)
            lastRealUser = i;
    }
    if (lastRealUser >= 0)
        protectedIndices.insert(lastRealUser);

    // 从尾部向前：保护在途组（缺结果）与最近一个完整工具组；后者连同其后的自动回灌消息
    bool completeGroupProtected = false;
    for (int i = m_conversationHistory.size() - 1; i >= 0; --i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role != QStringLiteral("assistant") || message.toolCalls.isEmpty())
            continue;
        QSet<QString> ids;
        for (const QJsonValue& call : message.toolCalls) {
            const QString id = call.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty())
                ids.insert(id);
        }
        QSet<QString> found;
        int end = i + 1;
        while (end < m_conversationHistory.size() && m_conversationHistory.at(end).role == QStringLiteral("tool")) {
            found.insert(m_conversationHistory.at(end).toolCallId);
            ++end;
        }
        if (!(ids - found).isEmpty()) {
            // 在途/不完整组：不得当作历史损坏删除
            for (int k = i; k < end; ++k)
                protectedIndices.insert(k);
            continue;
        }
        if (completeGroupProtected)
            continue;
        for (int k = i; k < end; ++k)
            protectedIndices.insert(k);
        while (end < m_conversationHistory.size() && m_conversationHistory.at(end).role == QStringLiteral("user") &&
               m_conversationHistory.at(end).internalAutoInjected) {
            protectedIndices.insert(end);
            ++end;
        }
        completeGroupProtected = true;
    }
    return protectedIndices;
}

bool AgentController::dropOldestUserTurn() {
    int firstRealUser = -1;
    int secondRealUser = -1;
    for (int i = 0; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role != QStringLiteral("user") || message.internalAutoInjected)
            continue;
        if (firstRealUser < 0) {
            firstRealUser = i;
        } else {
            secondRealUser = i;
            break;
        }
    }
    if (secondRealUser < 0)
        return false;

    const QSet<int> protectedIndices = protectedHistoryIndices();
    for (int i = 0; i < secondRealUser; ++i) {
        if (protectedIndices.contains(i))
            return false;
    }
    m_conversationHistory = m_conversationHistory.mid(secondRealUser);
    while (!m_conversationHistory.isEmpty() && (m_conversationHistory.constFirst().role != QStringLiteral("user") ||
                                                m_conversationHistory.constFirst().internalAutoInjected))
        m_conversationHistory.removeFirst();
    return true;
}

bool AgentController::dropOldestToolGroup() {
    // 从 index=1 起，只删保护集合之外的完整工具组（含其后的自动回灌消息）
    const QSet<int> protectedIndices = protectedHistoryIndices();
    for (int i = 1; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role != QStringLiteral("assistant") || message.toolCalls.isEmpty())
            continue;
        int end = i + 1;
        QSet<QString> ids;
        for (const QJsonValue& call : message.toolCalls) {
            const QString id = call.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty())
                ids.insert(id);
        }
        while (end < m_conversationHistory.size() && m_conversationHistory.at(end).role == QStringLiteral("tool")) {
            if (!ids.contains(m_conversationHistory.at(end).toolCallId))
                qWarning() << "[AgentController] tool group contains unmatched tool while dropping group";
            ++end;
        }
        while (end < m_conversationHistory.size() && m_conversationHistory.at(end).role == QStringLiteral("user") &&
               m_conversationHistory.at(end).internalAutoInjected)
            ++end;

        bool overlapsProtected = false;
        for (int k = i; k < end; ++k) {
            if (protectedIndices.contains(k)) {
                overlapsProtected = true;
                break;
            }
        }
        if (overlapsProtected)
            continue;

        for (int k = end - 1; k >= i; --k)
            m_conversationHistory.removeAt(k);
        return true;
    }
    return false;
}

void AgentController::trimHistoryIfNeeded() {
    constexpr int kMaxHistoryMessages = 64; // 既有硬上限（设计 §7.6）
    if (m_conversationHistory.isEmpty())
        return;

    repairOrphanToolGroups();

    // I5：首条必须为真实 user（system 由 systemPrompt 单独承载；自动回灌不构成边界）
    while (!m_conversationHistory.isEmpty() && (m_conversationHistory.constFirst().role != QStringLiteral("user") ||
                                                m_conversationHistory.constFirst().internalAutoInjected))
        m_conversationHistory.removeFirst();

    const int budget = contextBudgetTokens();
    int guard = 0;
    while ((estimateHistoryTokens() > budget || m_conversationHistory.size() > kMaxHistoryMessages) &&
           guard++ < 100000) {
        if (dropOldestUserTurn())
            continue;
        if (dropOldestToolGroup())
            continue;
        break; // 只剩保护集合：由发送前校验失败关闭（context_budget_exceeded）
    }

    while (!m_conversationHistory.isEmpty() && (m_conversationHistory.constFirst().role != QStringLiteral("user") ||
                                                m_conversationHistory.constFirst().internalAutoInjected))
        m_conversationHistory.removeFirst();
}

bool AgentController::historyPairsValid(QString* error) const {
    QSet<QString> pending;
    bool inGroup = false;
    for (int i = 0; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role == QStringLiteral("assistant") && !message.toolCalls.isEmpty()) {
            if (inGroup) {
                if (error)
                    *error = QStringLiteral("tool group at message %1 is missing results").arg(i - 1);
                return false;
            }
            pending.clear();
            for (const QJsonValue& call : message.toolCalls) {
                const QString id = call.toObject().value(QStringLiteral("id")).toString();
                if (id.isEmpty()) {
                    if (error)
                        *error = QStringLiteral("assistant tool_call without id at message %1").arg(i);
                    return false;
                }
                pending.insert(id);
            }
            if (pending.isEmpty()) {
                if (error)
                    *error = QStringLiteral("empty tool_calls at message %1").arg(i);
                return false;
            }
            inGroup = true;
            continue;
        }
        if (message.role == QStringLiteral("tool")) {
            if (!inGroup || !pending.contains(message.toolCallId)) {
                if (error)
                    *error = QStringLiteral("unexpected tool result '%1' at message %2").arg(message.toolCallId).arg(i);
                return false;
            }
            pending.remove(message.toolCallId);
            if (pending.isEmpty())
                inGroup = false;
            continue;
        }
        if (inGroup) {
            if (error)
                *error = QStringLiteral("tool group interrupted at message %1").arg(i);
            return false;
        }
    }
    if (inGroup) {
        if (error)
            *error = QStringLiteral("in-flight tool group is missing results");
        return false;
    }
    return true;
}

void AgentController::failContextSend(const QString& code, const QString& detail) {
    const QString message = QStringLiteral("Context send refused (%1); no request was sent: %2").arg(code, detail);
    Logger::instance().addLog(QStringLiteral("[Context] %1").arg(message), LogLevel::Warning, "Agent");
    AgentActionLogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.actor = QStringLiteral("Agent");
    entry.action = code;
    entry.result = QStringLiteral("error");
    entry.undoable = false;
    emit actionLogEntryAdded(entry);
    transitionTo(AgentState::Idle);
    emit llmErrorOccurred(message);
}

bool AgentController::sendPreparedContext() {
    trimHistoryIfNeeded();

    QString pairError;
    if (!historyPairsValid(&pairError)) {
        failContextSend(QStringLiteral("context_invalid_history"), pairError);
        return false;
    }

    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();

    // buildContext 会把超长系统提示截断到 16KB；预算校验仍以"用户配置的完整系统提示"
    // 为准，避免截断掩盖系统提示本身超限（B0/B3 契约）。
    const QString budgetPrompt = m_systemPrompt.size() > ctx.systemPrompt.size() ? m_systemPrompt : ctx.systemPrompt;
    const qint64 estimated = estimateTokensFor(ctx.messages, budgetPrompt, echoesReasoning());
    constexpr int kMaxHistoryMessages = 64;
    if (estimated > contextBudgetTokens() || ctx.messages.size() > kMaxHistoryMessages) {
        failContextSend(QStringLiteral("context_budget_exceeded"),
                        QStringLiteral("estimated %1 tokens / %2 messages, budget %3 tokens")
                            .arg(estimated)
                            .arg(ctx.messages.size())
                            .arg(contextBudgetTokens()));
        return false;
    }

    if (!m_llmClient) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred(QStringLiteral("LLM client not configured"));
        return false;
    }
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
    return true;
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
        return {{"success", false}, {"error", "Permission denied: Observer mode"}, {"code", "PERMISSION_DENIED"}};
    if (requiresConfirmation(m_permissionLevel, access)) {
        QJsonArray calls;
        calls.append(makeToolCall(toolName, params));
        m_pendingToolCalls = calls;
        m_continueAfterPendingTools = false;
        snapshotConfirmation();
        transitionTo(AgentState::Confirming);
        emit toolsPendingConfirmation(calls);
        return QJsonObject{{"status", "pending_confirmation"}, {"tools", calls}};
    }

    Logger::instance().addLog(QString("[ToolCall] %1").arg(toolName), LogLevel::Info, "Agent");
    emit agentActionReceived(toolName, params);
    QJsonObject result = m_actor->executeTool(toolName, params);
    // makeAuditEntry 读取并剥离 _elapsedMs，返回给 IPC/调用方的 result 不再含内部字段
    AgentActionLogEntry entry = makeAuditEntry(m_actor, toolName, params, QString(), result);
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
    sendPreparedContext();
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

// ========== Streaming (设计 §5.1/§5.2/§6.1) ==========

void AgentController::onStreamStarted() {
    m_streamActive = true;
    m_pendingContent.clear();
    m_pendingReasoning.clear();
    emit agentStreamStarted();
}

void AgentController::onStreamContentDelta(const QString& text) {
    if (text.isEmpty())
        return;
    m_pendingContent += text;
    if (!m_streamFlushTimer->isActive())
        m_streamFlushTimer->start();
}

void AgentController::onStreamReasoningDelta(const QString& text) {
    if (text.isEmpty())
        return;
    m_pendingReasoning += text;
    if (!m_streamFlushTimer->isActive())
        m_streamFlushTimer->start();
}

void AgentController::flushStreamPending() {
    if (m_streamFlushTimer)
        m_streamFlushTimer->stop();
    if (!m_pendingContent.isEmpty()) {
        const QString text = m_pendingContent;
        m_pendingContent.clear();
        emit agentStreamContentDelta(text);
    }
    if (!m_pendingReasoning.isEmpty()) {
        const QString text = m_pendingReasoning;
        m_pendingReasoning.clear();
        emit agentStreamReasoningDelta(text);
    }
}

void AgentController::resetStreamBuffer() {
    if (m_streamFlushTimer)
        m_streamFlushTimer->stop();
    m_pendingContent.clear();
    m_pendingReasoning.clear();
    m_streamActive = false;
}

void AgentController::onStreamFinished() {
    flushStreamPending();
    // 双终态去重（§5.6）：重复的 streamFinished 直接忽略
    if (!m_streamActive)
        return;
    m_streamActive = false;
    emit agentStreamFinished();
}

void AgentController::onStreamFailed() {
    flushStreamPending();
    m_streamActive = false;
    // 收口由随后 errorOccurred/Classified 完成；UI 用 llmErrorOccurred 标记已中断气泡
}

void AgentController::onStreamFallback(const QString& reason) {
    flushStreamPending();
    m_streamActive = false;
    Logger::instance().addLog(QString("[AgentStream] fallback: %1").arg(reason), LogLevel::Info, "Agent");
    AgentActionLogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.actor = "System";
    entry.action = "LLM stream fallback";
    entry.params = reason;
    entry.result = "success";
    entry.undoable = false;
    emit actionLogEntryAdded(entry);
    emit agentStreamFallback(reason);
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
        "call.\n"
        "12. If the project summary says \"(+M more; call get_flow_state for the full list)\", the module list is "
        "partial — call get_flow_state before assuming a module does not exist.\n");
}

} // namespace DeepLux
