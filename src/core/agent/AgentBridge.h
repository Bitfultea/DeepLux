#ifndef DEEPLUX_AGENT_BRIDGE_H
#define DEEPLUX_AGENT_BRIDGE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QMap>
#include <QList>
#include <QTimer>
#include <QLocalServer>

namespace DeepLux {

class AgentConnection;

/**
 * @brief Agent 桥接层 - 通过 Unix Domain Socket / Named Pipe 与 LLM Agent 通信
 *
 * 支持平台:
 * - Linux: Unix Domain Socket
 * - Windows: Named Pipe
 *
 * 通信协议: JSON v1.0
 *
 * 消息类型:
 * - execute: 执行 bash 命令
 * - query: 查询系统状态
 * - ping: 心跳保活
 * - result: 命令/查询结果
 * - event: 主动事件推送
 * - pong: 心跳响应
 */
class AgentBridge : public QObject
{
    Q_OBJECT

public:
    static AgentBridge& instance();

    // 启动/停止
    bool start();
    void stop();
    bool isRunning() const { return m_running; }

    // 查询接口（注册式）
    using QueryHandler = std::function<QJsonObject(const QJsonObject& params)>;
    void registerQueryHandler(const QString& target, QueryHandler handler);

    // 事件订阅发送
    void sendEvent(const QString& event, const QJsonObject& payload);

    // 连接状态
    int connectedAgents() const { return m_connections.size(); }

signals:
    void agentConnected(const QString& clientId);
    void agentDisconnected(const QString& clientId);
    void agentConnectionLost(const QString& clientId);

private slots:
    void onNewConnection();
    void onClientMessage(const QString& clientId, const QJsonObject& msg);
    void onClientDisconnected(const QString& clientId);
    void onHeartbeatTimeout();
    void attemptReconnect(const QString& clientId);

private:
    AgentBridge(QObject* parent = nullptr);
    ~AgentBridge() override;

    bool m_running = false;
    QLocalServer* m_server = nullptr;
    QString m_socketPath;
    QList<AgentConnection*> m_connections;
    QMap<QLocalSocket*, QString> m_clientSocketToId;  // socket -> clientId

    // 心跳
    QTimer* m_heartbeatTimer = nullptr;
    QMap<QString, int> m_missedHeartbeats;  // clientId -> missed count

    // 查询处理器（注册式，支持扩展）
    QMap<QString, QueryHandler> m_queryHandlers;

    // 协议版本
    static constexpr const char* PROTOCOL_VERSION = "1.0";

    QJsonObject handleExecute(const QString& reqId, const QJsonObject& payload);
    QJsonObject handleQuery(const QString& reqId, const QJsonObject& payload);
    QJsonObject handlePing(const QString& reqId);
    void sendResponse(const QString& clientId, const QString& reqId, const QJsonObject& payload);
    void sendError(const QString& clientId, const QString& reqId, const QString& errorMsg);
    void broadcastEvent(const QString& event, const QJsonObject& payload);
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_BRIDGE_H
