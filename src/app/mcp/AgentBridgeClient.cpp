#include "AgentBridgeClient.h"

#include "core/platform/PathUtils.h"

#include <QDebug>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QTimer>

namespace DeepLux {
namespace Mcp {

namespace {
// AgentConnection 的入站上限是 1MiB；桥侧对超限行按错误处理，不静默卡死
constexpr qint64 kMaxBridgeLineBytes = 1 * 1024 * 1024;
constexpr int kMaxConnectTimeoutMs = 30000;

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
} // namespace

AgentBridgeClient::AgentBridgeClient(QObject* parent) : QObject(parent), m_candidates(defaultSocketCandidates()) {
    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, [this]() {
        if (m_phase == Phase::Connecting)
            tryNextCandidate();
    });
    m_operationTimer = new QTimer(this);
    m_operationTimer->setSingleShot(true);
    connect(m_operationTimer, &QTimer::timeout, this, [this]() { finish(Status::Timeout); });
}

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

bool AgentBridgeClient::isCallActive() const {
    return m_phase != Phase::Idle;
}

void AgentBridgeClient::startCall(const QString& toolName, const QJsonObject& params) {
    if (m_phase != Phase::Idle) {
        finish(Status::BridgeError, QStringLiteral("Bridge client already has an in-flight call"));
        return;
    }

    m_remainingCandidates = m_candidates;
    m_buffer.clear();
    m_requestId = QStringLiteral("mcp-%1").arg(m_nextRequestId++);
    m_lastRequestId = m_requestId;
    const QJsonObject payload{{QStringLiteral("tool"), toolName},
                              {QStringLiteral("params"), params},
                              {QStringLiteral("origin"), QStringLiteral("mcp")}};
    m_toolFrame = QJsonObject{{QStringLiteral("version"), QStringLiteral("1.0")},
                              {QStringLiteral("type"), QStringLiteral("tool_call")},
                              {QStringLiteral("id"), m_requestId},
                              {QStringLiteral("payload"), payload}};

    m_phase = Phase::Connecting;
    restartTimeout(qMax(1, m_operationTimeoutMs));

    if (m_remainingCandidates.isEmpty()) {
        finish(Status::GuiNotRunning);
        return;
    }
    tryNextCandidate();
}

void AgentBridgeClient::cancelCall() {
    if (m_phase == Phase::Idle)
        return;
    // 取消只断开本地连接并抑制后续回调；不声称能中止 GUI 内已开始的操作。
    resetTransport();
}

void AgentBridgeClient::tryNextCandidate() {
    m_connectTimer->stop();
    if (m_phase != Phase::Connecting)
        return;
    if (m_remainingCandidates.isEmpty()) {
        finish(Status::GuiNotRunning);
        return;
    }

    const QString candidate = m_remainingCandidates.takeFirst();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    auto* socket = new QLocalSocket(this);
    m_socket = socket;
    connect(socket, &QLocalSocket::connected, this, &AgentBridgeClient::onConnected);
    connect(socket, &QLocalSocket::readyRead, this, &AgentBridgeClient::onReadyRead);
    connect(socket, &QLocalSocket::disconnected, this, &AgentBridgeClient::onDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QLocalSocket::errorOccurred, this, &AgentBridgeClient::onSocketError);
#else
    connect(socket, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error), this,
            &AgentBridgeClient::onSocketError);
#endif

    const int configuredConnect = m_connectTimeoutMs > 0 ? m_connectTimeoutMs : 3000;
    m_connectTimer->start(qBound(1, qMin(configuredConnect, m_operationTimeoutMs), kMaxConnectTimeoutMs));
    socket->connectToServer(candidate);
}

void AgentBridgeClient::onConnected() {
    m_connectTimer->stop();
    if (m_phase != Phase::Connecting)
        return;
    if (!m_authToken.isEmpty()) {
        sendAuth();
    } else {
        sendToolCall();
    }
}

void AgentBridgeClient::sendAuth() {
    const QJsonObject authFrame{{QStringLiteral("version"), QStringLiteral("1.0")},
                                {QStringLiteral("type"), QStringLiteral("auth")},
                                {QStringLiteral("id"), QStringLiteral("mcp-auth")},
                                {QStringLiteral("payload"), QJsonObject{{QStringLiteral("token"), m_authToken}}}};
    if (!m_socket || !writeFrame(*m_socket, authFrame)) {
        finish(Status::GuiNotRunning);
        return;
    }
    m_phase = Phase::Authenticating;
}

void AgentBridgeClient::sendToolCall() {
    if (!m_socket || !writeFrame(*m_socket, m_toolFrame)) {
        finish(Status::GuiNotRunning);
        return;
    }
    m_phase = Phase::Waiting;
}

void AgentBridgeClient::onReadyRead() {
    if (!m_socket || m_phase == Phase::Idle)
        return;
    m_buffer.append(m_socket->readAll());
    if (m_buffer.size() > kMaxBridgeLineBytes) {
        finish(Status::BridgeError, QStringLiteral("Bridge response exceeded the frame limit"));
        return;
    }
    processBufferedLines();
}

void AgentBridgeClient::processBufferedLines() {
    while (m_phase == Phase::Authenticating || m_phase == Phase::Waiting) {
        const int index = m_buffer.indexOf('\n');
        if (index < 0)
            return;
        QByteArray line = m_buffer.left(index);
        m_buffer.remove(0, index + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty())
            continue;

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            qWarning() << "[mcp-bridge] discarding non-JSON bridge frame";
            continue;
        }
        const QJsonObject response = doc.object();
        const QString responseId = response.value(QStringLiteral("id")).toString();
        const QString expectedId = (m_phase == Phase::Authenticating) ? QStringLiteral("mcp-auth") : m_requestId;
        if (responseId != expectedId) {
            qWarning() << "[mcp-bridge] discarding late/unknown bridge response for id" << responseId;
            continue;
        }

        const QString type = response.value(QStringLiteral("type")).toString();
        const QJsonObject payload = response.value(QStringLiteral("payload")).toObject();

        if (m_phase == Phase::Authenticating) {
            const bool ok = type == QStringLiteral("result") &&
                            payload.value(QStringLiteral("status")).toString() == QStringLiteral("authenticated");
            if (!ok) {
                finish(Status::AuthFailed);
                return;
            }
            sendToolCall();
            continue;
        }

        if (type == QStringLiteral("result")) {
            finish(Status::Ok, QString(), payload);
            return;
        }
        if (type == QStringLiteral("error")) {
            const QString message = payload.value(QStringLiteral("message")).toString();
            const QString lower = message.toLower();
            const Status status =
                (lower.contains(QStringLiteral("auth")) || lower.contains(QStringLiteral("unauthorized")))
                    ? Status::AuthFailed
                    : Status::BridgeError;
            finish(status, message);
            return;
        }
        qWarning() << "[mcp-bridge] discarding bridge frame with unknown type" << type;
    }
}

void AgentBridgeClient::onSocketError() {
    if (m_phase == Phase::Connecting) {
        // 当前候选不可用：尝试下一个
        tryNextCandidate();
        return;
    }
    if (m_phase == Phase::Authenticating || m_phase == Phase::Waiting)
        finish(Status::GuiNotRunning);
}

void AgentBridgeClient::onDisconnected() {
    if (m_phase == Phase::Connecting) {
        tryNextCandidate();
        return;
    }
    if (m_phase == Phase::Authenticating || m_phase == Phase::Waiting)
        finish(Status::GuiNotRunning);
}

void AgentBridgeClient::restartTimeout(int ms) {
    m_operationTimer->start(qMax(1, ms));
}

void AgentBridgeClient::finish(Status status, const QString& message, const QJsonObject& payload) {
    if (m_phase == Phase::Idle && status == Status::Ok)
        return;
    resetTransport();

    CallResult result;
    result.status = status;
    result.payload = payload;
    result.errorMessage = message;
    if (status == Status::Ok) {
        result.ok = true;
    } else if (status == Status::GuiNotRunning && result.errorMessage.isEmpty()) {
        result.errorMessage = QStringLiteral("DeepLux GUI is not running; start DeepLux first");
    } else if (status == Status::Timeout && result.errorMessage.isEmpty()) {
        result.errorMessage = QStringLiteral("Bridge request timed out after %1 ms").arg(m_operationTimeoutMs);
    } else if (status == Status::AuthFailed && result.errorMessage.isEmpty()) {
        result.errorMessage = QStringLiteral("Bridge authentication failed");
    }
    emit callFinished(result);
}

void AgentBridgeClient::resetTransport() {
    m_connectTimer->stop();
    m_operationTimer->stop();
    m_phase = Phase::Idle;
    m_buffer.clear();
    m_remainingCandidates.clear();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

AgentBridgeClient::CallResult AgentBridgeClient::callTool(const QString& toolName, const QJsonObject& params) {
    CallResult result;
    bool completed = false;

    QEventLoop loop;
    const QMetaObject::Connection connection =
        connect(this, &AgentBridgeClient::callFinished, &loop, [&](const CallResult& value) {
            result = value;
            completed = true;
            loop.quit();
        });
    // 防御性兜底：异步层自身有超时；这里再加 1s 防止信号丢失导致挂死
    QTimer guard;
    guard.setSingleShot(true);
    connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(qMax(1, m_operationTimeoutMs) + 1000);

    startCall(toolName, params);
    if (!completed)
        loop.exec();
    disconnect(connection);

    if (!completed) {
        cancelCall();
        result.status = Status::Timeout;
        result.errorMessage = QStringLiteral("Bridge request timed out after %1 ms").arg(m_operationTimeoutMs);
    }
    return result;
}

} // namespace Mcp
} // namespace DeepLux
