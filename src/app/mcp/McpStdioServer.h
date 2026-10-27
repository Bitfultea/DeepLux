#ifndef DEEPLUX_MCP_MCPSTDIOSERVER_H
#define DEEPLUX_MCP_MCPSTDIOSERVER_H

#include "AgentBridgeClient.h"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

namespace DeepLux {
namespace Mcp {

/**
 * @brief MCP stdio 服务端：行分隔 JSON-RPC 2.0
 *
 * 只暴露 tools 能力（initialize / ping / tools/list / tools/call），工具清单来自
 * ToolSchema（单源），调用经 AgentBridge v1.0 转发到运行中的 GUI。本类不依赖 stdio，
 * 便于测试直接喂行并断言响应；main.cpp 负责 stdin/stdout 循环。
 *
 * 外部调用等价于 Observer（只读）：非 readOnly 工具在本地拒绝且**不建立 IPC 连接**。
 */
class McpStdioServer : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxFrameBytes = 4 * 1024 * 1024;
    static constexpr const char* kProtocolVersion = "2024-11-05";
    static constexpr const char* kServerName = "deeplux-mcp-bridge";

    explicit McpStdioServer(QObject* parent = nullptr);
    ~McpStdioServer() override;

    /// 处理一行 JSON-RPC 文本；返回需写 stdout 的完整响应行（通知/空行返回空）
    QByteArray processLine(const QByteArray& line);

    AgentBridgeClient* bridgeClient() const {
        return m_bridge;
    }

    /// tools/list 的 result（工具清单 + annotations）
    QJsonObject toolsListResult() const;

    /// 权限预检：只有 ToolSchema 中 readOnly==true 的工具允许转发
    static bool isReadOnlyTool(const QString& name);

private:
    struct ToolCallOutcome {
        bool rpcError = false;
        int code = 0;
        QString message;
        QJsonObject result;
    };

    QJsonObject dispatch(const QJsonObject& request, bool* isNotification);
    static QJsonObject handleInitialize(const QJsonObject& params);
    ToolCallOutcome handleToolsCall(const QJsonObject& params);

    static QJsonObject successResponse(const QJsonValue& id, const QJsonObject& result);
    static QJsonObject errorResponse(const QJsonValue& id, int code, const QString& message);
    static QJsonObject toolResult(const QJsonObject& payload, bool isError);
    static QByteArray serialize(const QJsonObject& response);

    AgentBridgeClient* m_bridge;
};

} // namespace Mcp
} // namespace DeepLux

#endif // DEEPLUX_MCP_MCPSTDIOSERVER_H
