#include "AgentBridgeClient.h"

#include "core/platform/PathUtils.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalSocket>

namespace DeepLux {
namespace Mcp {

namespace {
// AgentConnection 的入站上限是 1MiB；桥侧对超限行按错误处理，不静默卡死
constexpr qint64 kMaxBridgeLineBytes = 1 * 1024 * 1024;

QByteArray encodeFrame(const QJsonObject& frame) {
    return QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
}

bool writeFrame(QLocalSocket& socket, const QJsonObject& frame) {
    const QByteArray data = encodeFrame(frame);
    if (socket.write(data) != data.size())
        return false;
    socket.flush();
    return true;
}

enum class ReadLineStatus { Ok, Timeout, Disconnected, TooLarge };

ReadLineStatus readLine(QLocalSocket& socket, QByteArray& buffer, QByteArray& line, int remainingMs) {
    QElapsedTimer timer;
    timer.start();
    while (true) {
        const int index = buffer.indexOf('\n');
        if (index >= 0) {
            line = buffer.left(index);
            buffer.remove(0, index + 1);
            if (line.endsWith('\r'))
                line.chop(1);
            return ReadLineStatus::Ok;
        }
        if (buffer.size() > kMaxBridgeLineBytes)
            return ReadLineStatus::TooLarge;
        const int waitMs = remainingMs - static_cast<int>(timer.elapsed());
        if (waitMs <= 0)
            return ReadLineStatus::Timeout;
        if (socket.state() != QLocalSocket::ConnectedState)
            return ReadLineStatus::Disconnected;
        if (socket.waitForReadyRead(waitMs)) {
            buffer.append(socket.readAll());
        } else if (socket.state() != QLocalSocket::ConnectedState) {
            return ReadLineStatus::Disconnected;
        }
    }
}
} // namespace

AgentBridgeClient::AgentBridgeClient(QObject* parent) : QObject(parent), m_candidates(defaultSocketCandidates()) {}

QStringList AgentBridgeClient::defaultSocketCandidates() {
    QStringList candidates;
#if defined(Q_OS_WIN)
    // 与 AgentBridge::start() 的 Windows 分支完全一致（named pipe）
    candidates << QStringLiteral("\\\\.\\pipe\\deeplux_agent");
#else
    // 与 AgentBridge::start() 的 Linux 分支一致：首选 appData/agent.sock，
    // 回落 deeplux_agent_<qHash(socketDir) 的 16 进制>。
    const QString socketDir = PathUtils::appDataPath();
    candidates << socketDir + QStringLiteral("/agent.sock");
    candidates << QStringLiteral("deeplux_agent_%1").arg(QString::number(qHash(socketDir), 16));
#endif
    return candidates;
}

void AgentBridgeClient::setSocketCandidates(const QStringList& candidates) {
    m_candidates = candidates;
}

QStringList AgentBridgeClient::socketCandidates() const {
    return m_candidates;
}

void AgentBridgeClient::setAuthToken(const QString& token) {
    m_authToken = token;
}

QString AgentBridgeClient::authToken() const {
    return m_authToken;
}

void AgentBridgeClient::setOperationTimeoutMs(int ms) {
    m_operationTimeoutMs = ms;
}

int AgentBridgeClient::operationTimeoutMs() const {
    return m_operationTimeoutMs;
}

void AgentBridgeClient::setConnectTimeoutMs(int ms) {
    m_connectTimeoutMs = ms;
}

int AgentBridgeClient::connectTimeoutMs() const {
    return m_connectTimeoutMs;
}

QString AgentBridgeClient::lastRequestId() const {
    return m_lastRequestId;
}

AgentBridgeClient::CallResult AgentBridgeClient::callTool(const QString& toolName, const QJsonObject& params) {
    CallResult result;
    if (m_candidates.isEmpty()) {
        result.status = Status::GuiNotRunning;
        result.errorMessage = QStringLiteral("DeepLux GUI is not running; start DeepLux first");
        return result;
    }

    const int totalMs = qMax(1, m_operationTimeoutMs);
    QElapsedTimer timer;
    timer.start();
    const auto remaining = [&timer, totalMs]() { return totalMs - static_cast<int>(timer.elapsed()); };

    QLocalSocket socket;
    bool connected = false;
    const int configuredConnect = m_connectTimeoutMs > 0 ? m_connectTimeoutMs : 3000;
    const int connectTimeout = qBound(1, qMin(configuredConnect, totalMs), 30000);
    for (const QString& candidate : m_candidates) {
        socket.connectToServer(candidate);
        if (socket.waitForConnected(connectTimeout)) {
            connected = true;
            break;
        }
        socket.abort();
    }
    if (!connected) {
        result.status = Status::GuiNotRunning;
        result.errorMessage = QStringLiteral("DeepLux GUI is not running; start DeepLux first");
        return result;
    }

    QByteArray buffer;

    // 可选认证握手：配置了 agent/bridgeToken 时，首帧必须是 auth
    if (!m_authToken.isEmpty()) {
        const QJsonObject authFrame{{QStringLiteral("version"), QStringLiteral("1.0")},
                                    {QStringLiteral("type"), QStringLiteral("auth")},
                                    {QStringLiteral("id"), QStringLiteral("mcp-auth")},
                                    {QStringLiteral("payload"), QJsonObject{{QStringLiteral("token"), m_authToken}}}};
        if (!writeFrame(socket, authFrame)) {
            result.status = Status::GuiNotRunning;
            result.errorMessage = QStringLiteral("DeepLux GUI is not running; start DeepLux first");
            socket.abort();
            return result;
        }
        QByteArray line;
        const ReadLineStatus status = readLine(socket, buffer, line, qMax(1, remaining()));
        if (status == ReadLineStatus::Disconnected) {
            result.status = Status::GuiNotRunning;
            result.errorMessage = QStringLiteral("DeepLux GUI is not running; start DeepLux first");
            socket.abort();
            return result;
        }
        bool authenticated = false;
        if (status == ReadLineStatus::Ok) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            const QJsonObject response = doc.object();
            authenticated =
                parseError.error == QJsonParseError::NoError &&
                response.value(QStringLiteral("type")).toString() == QStringLiteral("result") &&
                response.value(QStringLiteral("payload")).toObject().value(QStringLiteral("status")).toString() ==
                    QStringLiteral("authenticated");
        }
        if (!authenticated) {
            // 令牌缺失/错误/认证超时统一 -32003；不回显令牌
            result.status = Status::AuthFailed;
            result.errorMessage = QStringLiteral("Bridge authentication failed");
            socket.abort();
            return result;
        }
    }

    const QString requestId = QStringLiteral("mcp-%1").arg(m_nextRequestId++);
    m_lastRequestId = requestId;
    const QJsonObject payload{{QStringLiteral("tool"), toolName},
                              {QStringLiteral("params"), params},
                              {QStringLiteral("origin"), QStringLiteral("mcp")}};
    const QJsonObject frame{{QStringLiteral("version"), QStringLiteral("1.0")},
                            {QStringLiteral("type"), QStringLiteral("tool_call")},
                            {QStringLiteral("id"), requestId},
                            {QStringLiteral("payload"), payload}};
    if (!writeFrame(socket, frame)) {
        result.status = Status::GuiNotRunning;
        result.errorMessage = QStringLiteral("DeepLux GUI is not running; start DeepLux first");
        socket.abort();
        return result;
    }

    while (true) {
        const int remain = remaining();
        if (remain <= 0) {
            result.status = Status::Timeout;
            break;
        }
        QByteArray line;
        const ReadLineStatus status = readLine(socket, buffer, line, remain);
        if (status == ReadLineStatus::Timeout) {
            result.status = Status::Timeout;
            break;
        }
        if (status == ReadLineStatus::Disconnected) {
            result.status = Status::GuiNotRunning;
            result.errorMessage = QStringLiteral("DeepLux GUI is not running; start DeepLux first");
            break;
        }
        if (status == ReadLineStatus::TooLarge) {
            result.status = Status::BridgeError;
            result.errorMessage = QStringLiteral("Bridge response exceeded the frame limit");
            break;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            qWarning() << "[mcp-bridge] discarding non-JSON bridge frame";
            continue;
        }
        const QJsonObject response = doc.object();
        if (response.value(QStringLiteral("id")).toString() != requestId) {
            qWarning() << "[mcp-bridge] discarding late/unknown bridge response for id"
                       << response.value(QStringLiteral("id")).toString();
            continue;
        }
        const QString type = response.value(QStringLiteral("type")).toString();
        const QJsonObject responsePayload = response.value(QStringLiteral("payload")).toObject();
        if (type == QStringLiteral("result")) {
            result.ok = true;
            result.status = Status::Ok;
            result.payload = responsePayload;
        } else if (type == QStringLiteral("error")) {
            const QString message = responsePayload.value(QStringLiteral("message")).toString();
            result.errorMessage = message;
            const QString lower = message.toLower();
            result.status = (lower.contains(QStringLiteral("auth")) || lower.contains(QStringLiteral("unauthorized")))
                                ? Status::AuthFailed
                                : Status::BridgeError;
        } else {
            qWarning() << "[mcp-bridge] discarding bridge frame with unknown type" << type;
            continue;
        }
        break;
    }

    socket.disconnectFromServer();
    if (result.status == Status::GuiNotRunning && result.errorMessage.isEmpty())
        result.errorMessage = QStringLiteral("DeepLux GUI is not running; start DeepLux first");
    if (result.status == Status::Timeout && result.errorMessage.isEmpty())
        result.errorMessage = QStringLiteral("Bridge request timed out after %1 ms").arg(totalMs);
    if (result.status == Status::AuthFailed && result.errorMessage.isEmpty())
        result.errorMessage = QStringLiteral("Bridge authentication failed");
    return result;
}

} // namespace Mcp
} // namespace DeepLux
