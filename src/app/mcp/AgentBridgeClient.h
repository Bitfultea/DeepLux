#ifndef DEEPLUX_MCP_AGENTBRIDGECLIENT_H
#define DEEPLUX_MCP_AGENTBRIDGECLIENT_H

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

namespace DeepLux {
namespace Mcp {

/**
 * @brief MCP 桥到 DeepLux GUI 的 AgentBridge v1.0 同步客户端
 *
 * 只实现 tool_call 转发与可选认证握手（不做 query/subscribe）。
 * 每次调用建立一条短连接并串行等待响应；所有等待都有上限，不会挂死进程。
 */
class AgentBridgeClient : public QObject {
    Q_OBJECT

public:
    enum class Status {
        Ok,            // 收到 type=result
        GuiNotRunning, // 连接失败 / 连接中途断开（MCP -32001）
        Timeout,       // 等待超时（MCP -32002）
        AuthFailed,    // 认证失败/超时（MCP -32003）
        BridgeError,   // 桥返回 type=error（工具级错误，映射为 isError）
    };

    struct CallResult {
        bool ok = false;
        Status status = Status::Ok;
        QJsonObject payload;  // type=result 时的 payload
        QString errorMessage; // 桥错误/诊断（不含令牌）
    };

    explicit AgentBridgeClient(QObject* parent = nullptr);

    /// AgentBridge 使用的套接字候选（与 AgentBridge.cpp 的寻址算法保持一致）
    static QStringList defaultSocketCandidates();

    void setSocketCandidates(const QStringList& candidates);
    QStringList socketCandidates() const;

    void setAuthToken(const QString& token);
    QString authToken() const;

    /// 单次 tools/call 的等待上限（含连接/认证）。配置读取侧负责 clamp。
    void setOperationTimeoutMs(int ms);
    int operationTimeoutMs() const;

    /// 连接超时；<=0 表示按 min(operationTimeout, 3000ms) 推导
    void setConnectTimeoutMs(int ms);
    int connectTimeoutMs() const;

    /// 转发一个只读工具调用（调用方负责权限预检）
    CallResult callTool(const QString& toolName, const QJsonObject& params);

    /// 最近一次 callTool 的原始请求 id（测试/诊断用）
    QString lastRequestId() const;

private:
    QStringList m_candidates;
    QString m_authToken;
    int m_operationTimeoutMs = 30000;
    int m_connectTimeoutMs = 0;
    quint64 m_nextRequestId = 1;
    QString m_lastRequestId;
};

} // namespace Mcp
} // namespace DeepLux

#endif // DEEPLUX_MCP_AGENTBRIDGECLIENT_H
