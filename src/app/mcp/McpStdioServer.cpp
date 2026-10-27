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

McpStdioServer::McpStdioServer(QObject* parent) : QObject(parent), m_bridge(new AgentBridgeClient(this)) {
    // 工具清单单源：幂等注册（已注册时按名替换）
    ToolSchema::instance().registerDefaultTools();
}

McpStdioServer::~McpStdioServer() = default;

bool McpStdioServer::isReadOnlyTool(const QString& name) {
    return ToolSchema::instance().findTool(name).readOnly;
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

McpStdioServer::ToolCallOutcome McpStdioServer::handleToolsCall(const QJsonObject& params) {
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
        outcome.result = toolResult(
            QJsonObject{{QStringLiteral("error"), QStringLiteral("Permission denied: MCP calls are read-only")},
                        {QStringLiteral("code"), QStringLiteral("permission_denied")},
                        {QStringLiteral("hint"), QStringLiteral("Write tools must be executed inside the DeepLux GUI "
                                                                "where the user can confirm.")}},
            true);
        return outcome;
    }

    const AgentBridgeClient::CallResult call = m_bridge->callTool(name, arguments.toObject());
    switch (call.status) {
    case AgentBridgeClient::Status::Ok:
        outcome.result = toolResult(call.payload, call.payload.contains(QStringLiteral("error")));
        break;
    case AgentBridgeClient::Status::GuiNotRunning:
        outcome.rpcError = true;
        outcome.code = -32001;
        outcome.message = QStringLiteral("DeepLux GUI is not running; start DeepLux first");
        break;
    case AgentBridgeClient::Status::Timeout:
        outcome.rpcError = true;
        outcome.code = -32002;
        outcome.message = QStringLiteral("Bridge request timed out after %1 ms").arg(m_bridge->operationTimeoutMs());
        break;
    case AgentBridgeClient::Status::AuthFailed:
        outcome.rpcError = true;
        outcome.code = -32003;
        outcome.message = QStringLiteral("Bridge authentication failed");
        break;
    case AgentBridgeClient::Status::BridgeError:
        outcome.result = toolResult(QJsonObject{{QStringLiteral("error"), call.errorMessage},
                                                {QStringLiteral("code"), QStringLiteral("bridge_error")}},
                                    true);
        break;
    }
    return outcome;
}

QJsonObject McpStdioServer::dispatch(const QJsonObject& request, bool* isNotification) {
    *isNotification = false;
    const QJsonValue id =
        request.contains(QStringLiteral("id")) ? request.value(QStringLiteral("id")) : QJsonValue(QJsonValue::Null);
    const bool hasId = !id.isNull() && !id.isUndefined();

    if (request.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")) {
        return errorResponse(id, -32600, QStringLiteral("Invalid Request: jsonrpc must be \"2.0\""));
    }
    if (!request.value(QStringLiteral("method")).isString()) {
        return errorResponse(id, -32600, QStringLiteral("Invalid Request: 'method' is required"));
    }
    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonValue paramsValue = request.value(QStringLiteral("params"));
    const bool paramsValid = !paramsValue.isUndefined() && !paramsValue.isNull() ? paramsValue.isObject() : true;
    const QJsonObject params = paramsValue.toObject();

    // 通知：JSON-RPC 规定不得回包（含未知通知）
    if (!hasId) {
        *isNotification = true;
        if (method == QStringLiteral("notifications/initialized") ||
            method == QStringLiteral("notifications/cancelled")) {
            return QJsonObject();
        }
        qWarning() << "[mcp-bridge] ignoring unknown notification:" << method;
        return QJsonObject();
    }

    if (!paramsValid)
        return errorResponse(id, -32602, QStringLiteral("Invalid params: 'params' must be an object"));

    if (method == QStringLiteral("initialize")) {
        return successResponse(id, handleInitialize(params));
    }
    if (method == QStringLiteral("ping"))
        return successResponse(id, QJsonObject());
    if (method == QStringLiteral("tools/list"))
        return successResponse(id, toolsListResult());
    if (method == QStringLiteral("notifications/initialized") || method == QStringLiteral("notifications/cancelled"))
        return successResponse(id, QJsonObject());
    if (method == QStringLiteral("tools/call")) {
        const ToolCallOutcome outcome = handleToolsCall(params);
        if (outcome.rpcError)
            return errorResponse(id, outcome.code, outcome.message);
        return successResponse(id, outcome.result);
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
    QJsonObject response;
    try {
        response = dispatch(document.object(), &isNotification);
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

} // namespace Mcp
} // namespace DeepLux
