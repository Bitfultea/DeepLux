#ifndef DEEPLUX_MCP_AGENTBRIDGECLIENT_H
#define DEEPLUX_MCP_AGENTBRIDGECLIENT_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QLocalSocket;
class QTimer;

namespace DeepLux {
namespace Mcp {

/**
 * @brief MCP 桥到 DeepLux GUI 的 AgentBridge v1.0 客户端
 *
 * 异步实现：连接/认证/等待全部由 QLocalSocket 信号驱动，调用期间不阻塞事件循环，
 * 因此 MCP 服务端可以继续接收取消通知与后续请求。同一实例同一时刻只允许一个在途调用。
 * `callTool()` 保留为同步便利封装（内部使用局部 QEventLoop），仅用于测试/诊断。
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

    /// 异步发起一个只读工具调用（调用方负责权限预检）
    void startCall(const QString& toolName, const QJsonObject& params);
    /// 取消在途调用：立即断开本地连接，不再发出 callFinished
    void cancelCall();
    bool isCallActive() const;

    /// 同步封装：启动异步调用并等待 callFinished / 超时
    CallResult callTool(const QString& toolName, const QJsonObject& params);

    /// 最近一次 callTool/startCall 的原始请求 id（测试/诊断用）
    QString lastRequestId() const;

signals:
    void callFinished(const CallResult& result);

private:
    enum class Phase { Idle, Connecting, Authenticating, Waiting };

    void tryNextCandidate();
    void onConnected();
    void onReadyRead();
    void onSocketError();
    void onDisconnected();
    void sendAuth();
    void sendToolCall();
    void processBufferedLines();
    void restartTimeout(int ms);
    void finish(Status status, const QString& message = QString(), const QJsonObject& payload = QJsonObject());
    void resetTransport();

    QStringList m_candidates;
    QString m_authToken;
    int m_operationTimeoutMs = 30000;
    int m_connectTimeoutMs = 0;
    quint64 m_nextRequestId = 1;
    QString m_lastRequestId;

    QPointer<QLocalSocket> m_socket;
    QTimer* m_connectTimer = nullptr;
    QTimer* m_operationTimer = nullptr;
    QByteArray m_buffer;
    QStringList m_remainingCandidates;
    Phase m_phase = Phase::Idle;
    QString m_requestId;
    QJsonObject m_toolFrame;
};

} // namespace Mcp
} // namespace DeepLux

#endif // DEEPLUX_MCP_AGENTBRIDGECLIENT_H
