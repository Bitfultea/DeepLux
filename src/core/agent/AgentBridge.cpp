#include "AgentBridge.h"

#include "AgentConnection.h"
#include "config/SystemConfig.h"
#include "engine/RunEngine.h"
#include "manager/ConfigManager.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"
#include "model/Project.h"
#include "platform/PathUtils.h"
#include "platform/Platform.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>

namespace DeepLux {

AgentBridge::AgentBridge(QObject* parent) : QObject(parent), m_server(nullptr), m_heartbeatTimer(new QTimer(this)) {
    // 心跳定时器 - 10 秒发送一次
    m_heartbeatTimer->setInterval(10000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &AgentBridge::onHeartbeatTimeout);

    // 注册默认查询处理器
    registerDefaultQueryHandlers();
}

void AgentBridge::registerDefaultQueryHandlers() {
    // project query
    registerQueryHandler("project", [this](const QJsonObject& params) {
        Q_UNUSED(params);
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) {
            return QJsonObject{
                {"name", proj->name()}, {"path", proj->filePath()}, {"moduleCount", proj->modules().size()}};
        }
        return QJsonObject{{"error", "No project opened"}};
    });

    // run_state query
    registerQueryHandler("run_state", [this](const QJsonObject& params) {
        Q_UNUSED(params);
        RunEngine& engine = RunEngine::instance();
        RunState state = engine.state();
        QString stateStr;
        switch (state) {
        case RunState::Idle:
            stateStr = "idle";
            break;
        case RunState::Running:
            stateStr = "running";
            break;
        case RunState::Paused:
            stateStr = "paused";
            break;
        case RunState::Stopped:
            stateStr = "stopped";
            break;
        default:
            stateStr = "unknown";
            break;
        }
        QJsonObject result;
        result["state"] = stateStr;
        result["isRunning"] = engine.isRunning();
        return result;
    });

    // plugins query - 返回可用的插件模块
    registerQueryHandler("plugins", [this](const QJsonObject& params) {
        Q_UNUSED(params);
        QStringList modules = PluginManager::instance().availableModules();
        return QJsonObject{{"modules", QJsonArray::fromStringList(modules)}};
    });

    // system query
    registerQueryHandler("system", [this](const QJsonObject& params) {
        Q_UNUSED(params);
        QJsonObject result;
        result["platform"] = DEEPLUX_PLATFORM_NAME;
        result["version"] = DEEPLUX_VERSION_STRING;
        return result;
    });

    // modules query
    registerQueryHandler("modules", [this](const QJsonObject& params) {
        Q_UNUSED(params);
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj) {
            return QJsonObject{{"error", "No project opened"}};
        }
        QJsonArray moduleList;
        // proj->modules() 返回 QList<ModuleInstance>
        for (const ModuleInstance& inst : proj->modules()) {
            moduleList.append(inst.id);
        }
        QJsonObject result;
        result["modules"] = moduleList;
        return result;
    });
}

AgentBridge::~AgentBridge() {
    stop();
}

AgentBridge& AgentBridge::instance() {
    static AgentBridge instance;
    return instance;
}

bool AgentBridge::start() {
    if (m_running)
        return true;

    // 阶7 Agent 评审修复（P2-8）：可选握手令牌；空值=不启用（保持既有行为）
    m_authToken = ConfigManager::instance().groupString("agent", "bridgeToken", QString());

#if defined(Q_OS_LINUX)
    // 使用用户可写的 socket 路径
    QString socketDir = PathUtils::appDataPath();
    QDir().mkpath(socketDir);
    QString preferredSocketPath = socketDir + "/agent.sock";
    QString fallbackSocketName = QString("deeplux_agent_%1").arg(QString::number(qHash(socketDir), 16));
#elif defined(Q_OS_WINDOWS)
    m_socketPath = "\\\\.\\pipe\\deeplux_agent";
#endif

    m_server = new QLocalServer(this);

    // 设置连接接受信号
    connect(m_server, &QLocalServer::newConnection, this, &AgentBridge::onNewConnection);

    // 尝试删除旧的 socket 文件（Linux）
#if defined(Q_OS_LINUX)
    QLocalServer::removeServer(preferredSocketPath);
    QLocalServer::removeServer(fallbackSocketName);
#endif

#if defined(Q_OS_LINUX)
    if (m_server->listen(preferredSocketPath)) {
        m_socketPath = preferredSocketPath;
    } else {
        qWarning() << "Failed to start AgentBridge server:" << preferredSocketPath << m_server->serverError()
                   << m_server->errorString() << "falling back to" << fallbackSocketName;
        if (!m_server->listen(fallbackSocketName)) {
            qWarning() << "Failed to start AgentBridge fallback server:" << fallbackSocketName
                       << m_server->serverError() << m_server->errorString();
            delete m_server;
            m_server = nullptr;
            return false;
        }
        m_socketPath = fallbackSocketName;
    }
#else
    if (!m_server->listen(m_socketPath)) {
        qWarning() << "Failed to start AgentBridge server:" << m_socketPath << m_server->serverError()
                   << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
#endif

    m_running = true;
    m_heartbeatTimer->start();
    qDebug() << "AgentBridge started on" << m_socketPath;
    return true;
}

void AgentBridge::stop() {
    if (!m_running) {
        QMutexLocker locker(&m_connectionMutex);
        m_missedHeartbeats.clear();
        m_eventSubscriptions.clear();
        m_authenticated.clear();
        return;
    }

    m_heartbeatTimer->stop();
    m_running = false;

    // 断开所有连接
    {
        QMutexLocker locker(&m_connectionMutex);
        for (AgentConnection* conn : m_connections) {
            conn->deleteLater();
        }
        m_connections.clear();
        m_missedHeartbeats.clear();
        m_eventSubscriptions.clear();
        m_authenticated.clear();
    }

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

#if defined(Q_OS_LINUX)
    QLocalServer::removeServer(m_socketPath);
#endif

    qDebug() << "AgentBridge stopped";
}

void AgentBridge::registerQueryHandler(const QString& target, QueryHandler handler) {
    m_queryHandlers[target] = handler;
}

void AgentBridge::setToolCallCallback(ToolCallCallback callback) {
    m_toolCallCallback = callback;
}

void AgentBridge::sendEvent(const QString& event, const QJsonObject& payload) {
    broadcastEvent(event, payload);
}

void AgentBridge::onNewConnection() {
    QLocalSocket* socket = m_server->nextPendingConnection();
    if (!socket)
        return;

    QString clientId;
    {
        QMutexLocker locker(&m_connectionMutex);
        clientId = QStringLiteral("agent-%1").arg(m_nextClientId++);
    }

    AgentConnection* conn = new AgentConnection(socket, clientId, this);
    {
        QMutexLocker locker(&m_connectionMutex);
        m_connections.append(conn);
        m_missedHeartbeats[clientId] = 0;
    }

    connect(conn, &AgentConnection::messageReceived, this, &AgentBridge::onClientMessage);
    connect(conn, &AgentConnection::disconnected, this, &AgentBridge::onClientDisconnected);

    qDebug() << "Agent connected:" << clientId;
    emit agentConnected(clientId);
}

void AgentBridge::onClientMessage(const QString& clientId, const QJsonObject& msg) {
    ProtocolResponse response = processMessage(clientId, msg);
    if (response.error) {
        sendError(clientId, response.reqId, response.errorMessage);
        return;
    }

    // 重置心跳计数
    {
        QMutexLocker locker(&m_connectionMutex);
        m_missedHeartbeats[clientId] = 0;
    }
    sendResponse(clientId, response.reqId, response.payload);
}

AgentBridge::ProtocolResponse AgentBridge::processMessage(const QString& clientId, const QJsonObject& msg) {
    QString type = msg.value("type").toString();
    QString reqId = msg.value("id").toString();
    QJsonObject payload = msg.value("payload").toObject();

    ProtocolResponse response;
    response.reqId = reqId;

    // 阶7 Agent 评审修复（P2-8）：配置 agent/bridgeToken 后启用握手认证
    if (!m_authToken.isEmpty()) {
        bool authed = false;
        {
            QMutexLocker locker(&m_connectionMutex);
            authed = m_authenticated.value(clientId, false);
        }
        if (!authed) {
            if (type == "auth") {
                const bool ok = payload.value("token").toString() == m_authToken;
                {
                    QMutexLocker locker(&m_connectionMutex);
                    m_authenticated[clientId] = ok;
                }
                if (ok) {
                    response.payload = QJsonObject{{"status", "authenticated"}};
                    return response;
                }
                response.error = true;
                response.errorMessage = "Invalid auth token";
                forceCloseClient(clientId);
                return response;
            }
            response.error = true;
            response.errorMessage = "Unauthorized: send {\"type\":\"auth\",\"payload\":{\"token\":...}} first";
            return response;
        }
    }

    if (type == "execute") {
        // execute 类型已废弃：Agent 不再能执行任意 bash 命令
        response.error = true;
        response.errorMessage = "Message type 'execute' is deprecated. Use 'tool_call' instead.";
    } else if (type == "tool_call") {
        response.payload = handleToolCall(reqId, payload);
    } else if (type == "query") {
        response.payload = handleQuery(reqId, payload);
    } else if (type == "ping") {
        response.payload = handlePing(reqId);
    } else if (type == "subscribe") {
        // 处理事件订阅
        QString event = payload.value("event").toString();
        registerEventSubscription(clientId, event);
        response.payload = QJsonObject{{"status", "subscribed"}, {"event", event}};
    } else {
        // 未知消息类型
        response.error = true;
        response.errorMessage = "Unknown message type: " + type;
    }
    return response;
}

void AgentBridge::onClientDisconnected(const QString& clientId) {
    {
        QMutexLocker locker(&m_connectionMutex);
        for (int i = 0; i < m_connections.size(); ++i) {
            if (m_connections[i]->clientId() == clientId) {
                m_connections[i]->deleteLater();
                m_connections.removeAt(i);
                break;
            }
        }
        m_missedHeartbeats.remove(clientId);
        m_eventSubscriptions.remove(clientId);
        m_authenticated.remove(clientId);
    }
    emit agentDisconnected(clientId);
    qDebug() << "Agent disconnected:" << clientId;
}

void AgentBridge::registerEventSubscription(const QString& clientId, const QString& event) {
    QMutexLocker locker(&m_connectionMutex);
    if (!m_eventSubscriptions.contains(clientId)) {
        m_eventSubscriptions[clientId] = QStringList();
    }
    if (!m_eventSubscriptions[clientId].contains(event)) {
        m_eventSubscriptions[clientId].append(event);
        qDebug() << "Agent" << clientId << "subscribed to event:" << event;
    }
}

void AgentBridge::onHeartbeatTimeout() {
    QJsonObject pingMsg;
    pingMsg["version"] = PROTOCOL_VERSION;
    pingMsg["type"] = "ping";

    QStringList timedOutClients;
    QList<AgentConnection*> toRemove;
    QList<QPointer<AgentConnection>> heartbeatTargets;

    {
        QMutexLocker locker(&m_connectionMutex);
        for (AgentConnection* conn : m_connections) {
            if (conn->isConnected()) {
                heartbeatTargets.append(conn);
            }
        }
        for (auto it = m_missedHeartbeats.begin(); it != m_missedHeartbeats.end(); ++it) {
            it.value()++;
            if (it.value() >= 3) {
                timedOutClients.append(it.key());
            }
        }
        for (const QString& clientId : timedOutClients) {
            for (int i = 0; i < m_connections.size(); ++i) {
                if (m_connections[i]->clientId() == clientId) {
                    toRemove.append(m_connections[i]);
                    m_connections.removeAt(i);
                    break;
                }
            }
            m_missedHeartbeats.remove(clientId);
            m_eventSubscriptions.remove(clientId);
            m_authenticated.remove(clientId);
        }
    }

    // Socket writes can synchronously emit disconnected(), which re-enters this class.
    for (const QPointer<AgentConnection>& conn : heartbeatTargets) {
        if (conn) {
            conn->send(pingMsg);
        }
    }

    // 在锁外执行 deleteLater 和信号发射
    for (AgentConnection* conn : toRemove) {
        conn->deleteLater();
    }
    for (const QString& clientId : timedOutClients) {
        qDebug() << "Agent heartbeat timeout:" << clientId;
        emit agentConnectionLost(clientId);
        // 阶7 Agent 评审修复（P3-9）：移除空实现的 attemptReconnect 死代码；
        // 重连由客户端侧发起（bridge 仅被动 listen）
    }
}

void AgentBridge::forceCloseClient(const QString& clientId) {
    AgentConnection* target = nullptr;
    {
        QMutexLocker locker(&m_connectionMutex);
        for (AgentConnection* conn : m_connections) {
            if (conn->clientId() == clientId) {
                target = conn;
                break;
            }
        }
    }
    if (target)
        target->forceClose();
}

QJsonObject AgentBridge::handleToolCall(const QString& reqId, const QJsonObject& payload) {
    Q_UNUSED(reqId);

    QString toolName = payload.value("tool").toString();
    QJsonObject params = payload.value("params").toObject();

    if (toolName.isEmpty()) {
        return QJsonObject{{"error", "Missing 'tool' field in tool_call"}};
    }

    if (m_toolCallCallback) {
        return m_toolCallCallback(toolName, params);
    }

    return QJsonObject{{"error", "Tool call callback not set"}};
}

QJsonObject AgentBridge::handleQuery(const QString& reqId, const QJsonObject& payload) {
    Q_UNUSED(reqId);

    QString target = payload.value("target").toString();

    if (m_queryHandlers.contains(target)) {
        return m_queryHandlers[target](payload);
    }

    return QJsonObject{{"error", "Unknown query target: " + target}};
}

QJsonObject AgentBridge::handlePing(const QString& reqId) {
    Q_UNUSED(reqId);
    QJsonObject result;
    result["type"] = "pong";
    return result;
}

void AgentBridge::sendResponse(const QString& clientId, const QString& reqId, const QJsonObject& payload) {
    QJsonObject msg;
    msg["version"] = PROTOCOL_VERSION;
    msg["type"] = "result";
    msg["id"] = reqId;
    msg["payload"] = payload;

    QPointer<AgentConnection> target;
    {
        QMutexLocker locker(&m_connectionMutex);
        for (AgentConnection* conn : m_connections) {
            if (conn->clientId() == clientId) {
                target = conn;
                break;
            }
        }
    }
    if (target) {
        target->send(msg);
    }
}

void AgentBridge::sendError(const QString& clientId, const QString& reqId, const QString& errorMsg) {
    QJsonObject msg;
    msg["version"] = PROTOCOL_VERSION;
    msg["type"] = "error";
    msg["id"] = reqId;
    QJsonObject errPayload;
    errPayload["message"] = errorMsg;
    msg["payload"] = errPayload;

    QPointer<AgentConnection> target;
    {
        QMutexLocker locker(&m_connectionMutex);
        for (AgentConnection* conn : m_connections) {
            if (conn->clientId() == clientId) {
                target = conn;
                break;
            }
        }
    }
    if (target) {
        target->send(msg);
    }
}

void AgentBridge::broadcastEvent(const QString& event, const QJsonObject& payload) {
    QJsonObject msg;
    msg["version"] = PROTOCOL_VERSION;
    msg["type"] = "event";
    msg["event"] = event;
    msg["payload"] = payload;

    QList<QPointer<AgentConnection>> targets;
    {
        QMutexLocker locker(&m_connectionMutex);
        for (auto it = m_eventSubscriptions.begin(); it != m_eventSubscriptions.end(); ++it) {
            const QString& clientId = it.key();
            const QStringList& events = it.value();

            if (events.contains(event) || events.contains("*")) {
                for (AgentConnection* conn : m_connections) {
                    if (conn->clientId() == clientId && conn->isConnected()) {
                        targets.append(conn);
                        break;
                    }
                }
            }
        }
    }
    for (const QPointer<AgentConnection>& conn : targets) {
        if (conn) {
            conn->send(msg);
        }
    }
}

} // namespace DeepLux
