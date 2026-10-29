#include "McpStdioServer.h"

#include "core/agent/ToolSchema.h"
#include "core/platform/Platform.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <exception>

namespace DeepLux {
namespace Mcp {

namespace {
// 与 AgentActor::resultSucceeded 同口径（本进程不链接 AgentActor 实现细节）
bool payloadIsError(const QJsonObject& payload) {
    const QJsonValue success = payload.value(QStringLiteral("success"));
    if (success.isBool())
        return !success.toBool();
    return payload.contains(QStringLiteral("error"));
}
} // namespace

McpStdioServer::McpStdioServer(QObject* parent) : QObject(parent), m_bridge(new AgentBridgeClient(this)) {
    // 工具清单单源：幂等注册（已注册时按名替换）
    ToolSchema::instance().registerDefaultTools();
    connect(m_bridge, &AgentBridgeClient::callFinished, this, &McpStdioServer::onBridgeFinished);
}

McpStdioServer::~McpStdioServer() = default;

bool McpStdioServer::isReadOnlyTool(const QString& name) {
    return ToolSchema::instance().findTool(name).readOnly;
}

bool McpStdioServer::hasInFlightCall() const {
    return m_inFlight;
}

void McpStdioServer::setQueueLimitsForTest(int maxCalls, qint64 maxBytes) {
    m_maxPendingCalls = qMax(1, maxCalls);
    m_maxPendingBytes = qMax<qint64>(1, maxBytes);
}

QString McpStdioServer::requestKey(const QJsonValue& id) {
    if (id.isString())
        return QStringLiteral("s:") + id.toString();
    if (id.isDouble())
        return QStringLiteral("n:") + QString::number(id.toDouble(), 'g', 17);
    if (id.isBool())
        return QStringLiteral("b:") + (id.toBool() ? QStringLiteral("1") : QStringLiteral("0"));
    return QStringLiteral("null");
}

QByteArray McpStdioServer::serialize(const QJsonObject& response) {
    return QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject McpStdioServer::successResponse(const QJsonValue& id, const QJsonObject& result) {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("result"), result}};
}

QJsonObject McpStdioServer::errorResponse(const QJsonValue& id, int code, const QString& message) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}

QJsonObject McpStdioServer::toolResult(const QJsonObject& payload, bool isError) {
    const QString text = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QJsonArray content;
    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}});
    return QJsonObject{{QStringLiteral("content"), content}, {QStringLiteral("isError"), isError}};
}

QJsonObject McpStdioServer::toolResultForOutcome(const ToolCallOutcome& outcome) {
    return toolResult(outcome.result, payloadIsError(outcome.result));
}

QJsonObject McpStdioServer::toolsListResult() const {
    QJsonArray tools;
    const QList<ToolDefinition> definitions = ToolSchema::instance().allTools();
    for (const ToolDefinition& definition : definitions) {
        const QJsonObject function = definition.toOpenAIFunction().value(QStringLiteral("function")).toObject();
        QJsonObject tool{
            {QStringLiteral("name"), definition.name},
            {QStringLiteral("description"), definition.description},
            {QStringLiteral("inputSchema"), function.value(QStringLiteral("parameters")).toObject()},
            {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), definition.readOnly},
                                                        {QStringLiteral("destructiveHint"), definition.dangerous}}}};
        tools.append(tool);
    }
    return QJsonObject{{QStringLiteral("tools"), tools}};
}

QJsonObject McpStdioServer::handleInitialize(const QJsonObject& params) {
    Q_UNUSED(params)
    return QJsonObject{{QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion)},
                       {QStringLiteral("capabilities"),
                        QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
                       {QStringLiteral("serverInfo"),
                        QJsonObject{{QStringLiteral("name"), QString::fromLatin1(kServerName)},
                                    {QStringLiteral("version"), QString::fromLatin1(DEEPLUX_VERSION_STRING)}}}};
}

McpStdioServer::ToolCallOutcome McpStdioServer::validateToolCall(const QJsonObject& params, PendingCall* call) {
    ToolCallOutcome outcome;
    const QJsonValue nameValue = params.value(QStringLiteral("name"));
    if (!nameValue.isString() || nameValue.toString().trimmed().isEmpty()) {
        outcome.rpcError = true;
        outcome.code = -32602;
        outcome.message = QStringLiteral("Invalid params: 'name' is required");
        return outcome;
    }
    const QString name = nameValue.toString();
    const QJsonValue arguments = params.value(QStringLiteral("arguments"));
    if (!arguments.isUndefined() && !arguments.isNull() && !arguments.isObject()) {
        outcome.rpcError = true;
        outcome.code = -32602;
        outcome.message = QStringLiteral("Invalid params: 'arguments' must be an object");
        return outcome;
    }

    const ToolDefinition definition = ToolSchema::instance().findTool(name);
    if (definition.name.isEmpty()) {
        outcome.rpcError = true;
        outcome.code = -32602;
        outcome.message = QStringLiteral("Invalid params: unknown tool '%1'").arg(name);
        return outcome;
    }

    // 权限：外部默认 Observer（只读）。写工具在桥内拒绝，且不建立 IPC 连接、不转发。
    if (!definition.readOnly) {
        qWarning() << "[mcp-bridge] mcp write tool denied:" << name;
        outcome.result =
            QJsonObject{{QStringLiteral("error"), QStringLiteral("Permission denied: MCP calls are read-only")},
                        {QStringLiteral("code"), QStringLiteral("permission_denied")},
                        {QStringLiteral("hint"), QStringLiteral("Write tools must be executed inside the DeepLux GUI "
                                                                "where the user can confirm.")}};
        return outcome;
    }

    call->name = name;
    call->arguments = arguments.toObject();
    call->bytes = QJsonDocument(call->arguments).toJson(QJsonDocument::Compact).size();
    return outcome;
}

McpStdioServer::ToolCallOutcome McpStdioServer::executeToolCallSync(const PendingCall& call) {
    ToolCallOutcome outcome;
    const AgentBridgeClient::CallResult result = m_bridge->callTool(call.name, call.arguments);
    switch (result.status) {
    case AgentBridgeClient::Status::Ok:
        outcome.result = result.payload;
        break;
    case AgentBridgeClient::Status::GuiNotRunning:
        outcome.rpcError = true;
        outcome.code = -32001;
        outcome.message = result.errorMessage;
        break;
    case AgentBridgeClient::Status::Timeout:
        outcome.rpcError = true;
        outcome.code = -32002;
        outcome.message = result.errorMessage;
        break;
    case AgentBridgeClient::Status::AuthFailed:
        outcome.rpcError = true;
        outcome.code = -32003;
        outcome.message = result.errorMessage;
        break;
    case AgentBridgeClient::Status::BridgeError:
        outcome.result = QJsonObject{{QStringLiteral("error"), result.errorMessage},
                                     {QStringLiteral("code"), QStringLiteral("bridge_error")}};
        break;
    }
    return outcome;
}

void McpStdioServer::handleCancellation(const QJsonObject& params) {
    const QJsonValue requestId = params.value(QStringLiteral("requestId"));
    if (requestId.isUndefined() || requestId.isNull()) {
        qWarning() << "[mcp-bridge] cancellation without requestId ignored";
        return;
    }
    const QString key = requestKey(requestId);

    // 已排队未执行：直接移除，不建立 IPC、不回包
    for (int i = 0; i < m_pendingCalls.size(); ++i) {
        if (m_pendingCalls.at(i).key == key) {
            m_pendingBytes -= m_pendingCalls.at(i).bytes;
            m_pendingCalls.removeAt(i);
            return;
        }
    }

    if (m_inFlight && key == m_inFlightKey) {
        // 只抑制本地迟到响应：断开会话并释放业务槽位，不声称中止 GUI 内操作
        m_cancelledKeys.insert(key);
        m_bridge->cancelCall();
        finishInFlight();
        m_cancelledKeys.remove(key); // 本地连接已断开，不会再有迟到回调
        pumpQueue();
        return;
    }

    // 未知/重复/过期取消：不影响其他请求
    qWarning() << "[mcp-bridge] cancellation for unknown/expired request ignored";
}

void McpStdioServer::enqueueCall(const PendingCall& call) {
    if (m_pendingCalls.size() >= m_maxPendingCalls || m_pendingBytes + call.bytes > m_maxPendingBytes) {
        const QJsonObject response = errorResponse(
            call.id, -32009, QStringLiteral("Bridge queue is full; retry after the in-flight call completes"));
        emit responseReady(serialize(response));
        return;
    }
    m_pendingCalls.enqueue(call);
    m_pendingBytes += call.bytes;
    pumpQueue();
}

void McpStdioServer::pumpQueue() {
    if (m_inFlight || m_pendingCalls.isEmpty())
        return;
    const PendingCall call = m_pendingCalls.dequeue();
    m_pendingBytes -= call.bytes;
    m_inFlight = true;
    m_inFlightKey = call.key;
    m_inFlightId = call.id;
    m_bridge->startCall(call.name, call.arguments);
}

void McpStdioServer::onBridgeFinished(const AgentBridgeClient::CallResult& result) {
    if (!m_inFlight)
        return;

    const QString key = m_inFlightKey;
    const QJsonValue id = m_inFlightId;
    const bool cancelled = m_cancelledKeys.remove(key);
    finishInFlight();

    if (!cancelled) {
        QJsonObject response;
        switch (result.status) {
        case AgentBridgeClient::Status::Ok:
            response = successResponse(id, toolResult(result.payload, payloadIsError(result.payload)));
            break;
        case AgentBridgeClient::Status::GuiNotRunning:
            response = errorResponse(id, -32001, result.errorMessage);
            break;
        case AgentBridgeClient::Status::Timeout:
            response = errorResponse(id, -32002, result.errorMessage);
            break;
        case AgentBridgeClient::Status::AuthFailed:
            response = errorResponse(id, -32003, result.errorMessage);
            break;
        case AgentBridgeClient::Status::BridgeError:
            response =
                successResponse(id, toolResult(QJsonObject{{QStringLiteral("error"), result.errorMessage},
                                                           {QStringLiteral("code"), QStringLiteral("bridge_error")}},
                                               true));
            break;
        }
        emit responseReady(serialize(response));
    }
    pumpQueue();
}

void McpStdioServer::finishInFlight() {
    m_inFlight = false;
    m_inFlightKey.clear();
    m_inFlightId = QJsonValue(QJsonValue::Undefined);
}

QJsonObject McpStdioServer::dispatch(const QJsonObject& request, bool* isNotification, bool asyncMode,
                                     bool* handledAsync) {
    *isNotification = false;
    if (handledAsync)
        *handledAsync = false;
    const QJsonValue id = request.contains(QStringLiteral("id")) ? request.value(QStringLiteral("id"))
                                                                 : QJsonValue(QJsonValue::Undefined);
    // 无 id 字段 = 通知；显式 null = 合法请求（既有兼容承诺）
    const bool hasId = request.contains(QStringLiteral("id")) && !id.isUndefined();

    if (request.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")) {
        return errorResponse(id.isUndefined() ? QJsonValue(QJsonValue::Null) : id, -32600,
                             QStringLiteral("Invalid Request: jsonrpc must be \"2.0\""));
    }
    if (!request.value(QStringLiteral("method")).isString()) {
        return errorResponse(id.isUndefined() ? QJsonValue(QJsonValue::Null) : id, -32600,
                             QStringLiteral("Invalid Request: 'method' is required"));
    }
    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonValue paramsValue = request.value(QStringLiteral("params"));
    const bool paramsValid = !paramsValue.isUndefined() && !paramsValue.isNull() ? paramsValue.isObject() : true;
    const QJsonObject params = paramsValue.toObject();

    // 取消通知/请求：始终消费，避免"未响应"被误判为协议违约
    if (method == QStringLiteral("notifications/cancelled")) {
        if (asyncMode)
            handleCancellation(params);
        if (!hasId) {
            *isNotification = true;
            return QJsonObject();
        }
        return successResponse(id, QJsonObject());
    }

    // 通知：JSON-RPC 规定不得回包（含未知通知）
    if (!hasId) {
        *isNotification = true;
        if (method == QStringLiteral("notifications/initialized")) {
            return QJsonObject();
        }
        qWarning() << "[mcp-bridge] ignoring unknown notification:" << method;
        return QJsonObject();
    }

    if (!paramsValid)
        return errorResponse(id, -32602, QStringLiteral("Invalid params: 'params' must be an object"));

    if (method == QStringLiteral("initialize"))
        return successResponse(id, handleInitialize(params));
    if (method == QStringLiteral("ping"))
        return successResponse(id, QJsonObject());
    if (method == QStringLiteral("tools/list"))
        return successResponse(id, toolsListResult());
    if (method == QStringLiteral("tools/call")) {
        PendingCall call;
        call.id = id;
        call.key = requestKey(id);
        const ToolCallOutcome outcome = validateToolCall(params, &call);
        if (outcome.rpcError)
            return errorResponse(id, outcome.code, outcome.message);
        if (!outcome.result.isEmpty())
            return successResponse(id, toolResultForOutcome(outcome));
        if (asyncMode) {
            if (handledAsync)
                *handledAsync = true;
            enqueueCall(call);
            return QJsonObject();
        }
        const ToolCallOutcome executed = executeToolCallSync(call);
        if (executed.rpcError)
            return errorResponse(id, executed.code, executed.message);
        return successResponse(id, toolResultForOutcome(executed));
    }

    return errorResponse(id, -32601, QStringLiteral("Method not found: %1").arg(method));
}

QByteArray McpStdioServer::processLine(const QByteArray& line) {
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return QByteArray();
    if (line.size() > kMaxFrameBytes) {
        return serialize(
            errorResponse(QJsonValue(QJsonValue::Null), -32700, QStringLiteral("Parse error: frame exceeds 4 MiB")));
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return serialize(errorResponse(QJsonValue(QJsonValue::Null), -32700,
                                       QStringLiteral("Parse error: %1").arg(parseError.errorString())));
    }
    if (!document.isObject()) {
        return serialize(
            errorResponse(QJsonValue(QJsonValue::Null), -32600, QStringLiteral("Invalid Request: expected an object")));
    }

    bool isNotification = false;
    bool handledAsync = false;
    QJsonObject response;
    try {
        response = dispatch(document.object(), &isNotification, false, &handledAsync);
    } catch (const std::exception& error) {
        return serialize(errorResponse(QJsonValue(QJsonValue::Null), -32603,
                                       QStringLiteral("Internal error: %1").arg(QString::fromUtf8(error.what()))));
    } catch (...) {
        return serialize(errorResponse(QJsonValue(QJsonValue::Null), -32603, QStringLiteral("Internal error")));
    }
    if (isNotification)
        return QByteArray();
    return serialize(response);
}

QList<McpStdioServer::Frame> McpStdioServer::takeFrames(const QByteArray& chunk) {
    QList<Frame> frames;
    QByteArray remaining = chunk;

    if (m_discardingOverlong) {
        const int newline = remaining.indexOf('\n');
        if (newline < 0)
            return frames; // 继续丢弃，不保留任何字节
        m_discardingOverlong = false;
        remaining.remove(0, newline + 1);
    }

    m_inputBuffer.append(remaining);
    while (true) {
        const int newline = m_inputBuffer.indexOf('\n');
        if (newline < 0) {
            if (m_inputBuffer.size() > kMaxFrameBytes) {
                frames.append(Frame{true, QByteArray()});
                m_inputBuffer.clear();
                m_discardingOverlong = true;
            }
            break;
        }
        QByteArray line = m_inputBuffer.left(newline);
        m_inputBuffer.remove(0, newline + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty())
            continue;
        if (line.size() > kMaxFrameBytes) {
            frames.append(Frame{true, QByteArray()});
            continue;
        }
        frames.append(Frame{false, line});
    }
    return frames;
}

void McpStdioServer::ingestChunk(const QByteArray& chunk) {
    const QList<Frame> frames = takeFrames(chunk);
    for (const Frame& frame : frames) {
        if (frame.tooLarge) {
            emit responseReady(serialize(errorResponse(QJsonValue(QJsonValue::Null), -32700,
                                                       QStringLiteral("Parse error: frame exceeds 4 MiB"))));
            continue;
        }
        ingestLine(frame.line);
    }
}

void McpStdioServer::ingestLine(const QByteArray& line) {
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;
    if (line.size() > kMaxFrameBytes) {
        emit responseReady(serialize(
            errorResponse(QJsonValue(QJsonValue::Null), -32700, QStringLiteral("Parse error: frame exceeds 4 MiB"))));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        emit responseReady(serialize(errorResponse(QJsonValue(QJsonValue::Null), -32700,
                                                   QStringLiteral("Parse error: %1").arg(parseError.errorString()))));
        return;
    }
    if (!document.isObject()) {
        emit responseReady(serialize(errorResponse(QJsonValue(QJsonValue::Null), -32600,
                                                   QStringLiteral("Invalid Request: expected an object"))));
        return;
    }

    bool isNotification = false;
    bool handledAsync = false;
    QJsonObject response;
    try {
        response = dispatch(document.object(), &isNotification, true, &handledAsync);
    } catch (const std::exception& error) {
        emit responseReady(
            serialize(errorResponse(QJsonValue(QJsonValue::Null), -32603,
                                    QStringLiteral("Internal error: %1").arg(QString::fromUtf8(error.what())))));
        return;
    } catch (...) {
        emit responseReady(
            serialize(errorResponse(QJsonValue(QJsonValue::Null), -32603, QStringLiteral("Internal error"))));
        return;
    }
    if (isNotification || handledAsync)
        return;
    emit responseReady(serialize(response));
}

void McpStdioServer::finishInput() {
    m_inputBuffer.clear();
    m_discardingOverlong = false;
}

} // namespace Mcp
} // namespace DeepLux
