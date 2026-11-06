#ifndef DEEPLUX_MCP_MCPSTDIOSERVER_H
#define DEEPLUX_MCP_MCPSTDIOSERVER_H

#include "AgentBridgeClient.h"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>

namespace DeepLux {
namespace Mcp {

/**
 * @brief MCP stdio 服务端：行分隔 JSON-RPC 2.0
 *
 * 只暴露 tools 能力（initialize / ping / tools/list / tools/call），工具清单来自
 * ToolSchema（单源），调用经 AgentBridge v1.0 转发到运行中的 GUI。
 *
 * 输入侧（ingestChunk）有界增量分帧，业务侧单请求在途；工具调用异步执行，
 * 因此通知/取消与后续请求可在等待期间继续处理。processLine() 保留同步语义，
 * 供既有单元测试直接喂行。
 *
 * 外部调用等价于 Observer（只读）：非 readOnly 工具在本地拒绝且**不建立 IPC 连接**。
 */
class McpStdioServer : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxFrameBytes = 4 * 1024 * 1024;
    /// 待处理 tools/call 上限（B0 冻结；测试可注入更小值）
    static constexpr int kMaxPendingCalls = 8;
    static constexpr qint64 kMaxPendingBytes = 8LL * 1024 * 1024;
    static constexpr const char* kProtocolVersion = "2024-11-05";
    static constexpr const char* kServerName = "deeplux-mcp-bridge";

    explicit McpStdioServer(QObject* parent = nullptr);
    ~McpStdioServer() override;

    /// 同步处理一行 JSON-RPC 文本；返回需写 stdout 的完整响应行（通知/空行返回空）
    QByteArray processLine(const QByteArray& line);

    /// 增量输入：按换行分帧；超长帧立即回 -32700 并丢弃到下一个换行
    void ingestChunk(const QByteArray& chunk);
    /// 增量输入的单帧入口（无换行）
    void ingestLine(const QByteArray& line);
    /// stdin EOF：丢弃半帧，不再产生响应
    void finishInput();

    AgentBridgeClient* bridgeClient() const {
        return m_bridge;
    }

    /// tools/list 的 result（工具清单 + annotations）
    QJsonObject toolsListResult() const;

    /// 权限预检：只有 ToolSchema 中 readOnly==true 的工具允许转发
    static bool isReadOnlyTool(const QString& name);

    // ---- 测试/诊断可见 ----
    int pendingCallCount() const {
        return m_pendingCalls.size();
    }
    bool hasInFlightCall() const;
    void setQueueLimitsForTest(int maxCalls, qint64 maxBytes);

signals:
    /// 异步响应帧（已包含结尾换行）；stdout 只允许写入该信号的内容
    void responseReady(const QByteArray& frame);

private:
    struct ToolCallOutcome {
        bool rpcError = false;
        int code = 0;
        QString message;
        QJsonObject result;
    };

    struct PendingCall {
        QJsonValue id;
        QString key;
        QString name;
        QJsonObject arguments;
        qint64 bytes = 0;
    };

    struct Frame {
        bool tooLarge = false;
        QByteArray line;
    };

    QJsonObject dispatch(const QJsonObject& request, bool* isNotification, bool asyncMode, bool* handledAsync);
    QJsonObject handleInitialize(const QJsonObject& params);
    ToolCallOutcome validateToolCall(const QJsonObject& params, PendingCall* call);
    ToolCallOutcome executeToolCallSync(const PendingCall& call);
    void handleCancellation(const QJsonObject& params);
    void enqueueCall(const PendingCall& call);
    void pumpQueue();
    void onBridgeFinished(const AgentBridgeClient::CallResult& result);
    void finishInFlight();

    QList<Frame> takeFrames(const QByteArray& chunk);
    static QString requestKey(const QJsonValue& id);

    static QJsonObject successResponse(const QJsonValue& id, const QJsonObject& result);
    static QJsonObject errorResponse(const QJsonValue& id, int code, const QString& message);
    static QJsonObject toolResult(const QJsonObject& payload, bool isError);
    static QJsonObject toolResultForOutcome(const ToolCallOutcome& outcome);
    static QByteArray serialize(const QJsonObject& response);

    AgentBridgeClient* m_bridge;
    QByteArray m_inputBuffer;
    bool m_discardingOverlong = false;

    QQueue<PendingCall> m_pendingCalls;
    qint64 m_pendingBytes = 0;
    int m_maxPendingCalls = kMaxPendingCalls;
    qint64 m_maxPendingBytes = kMaxPendingBytes;

    bool m_inFlight = false;
    QString m_inFlightKey;
    QJsonValue m_inFlightId;
    QSet<QString> m_cancelledKeys;
};

} // namespace Mcp
} // namespace DeepLux

#endif // DEEPLUX_MCP_MCPSTDIOSERVER_H
