#include "AgentBridge.h"
#include "AgentConnection.h"

#include "manager/ProjectManager.h"
#include "engine/RunEngine.h"
#include "manager/PluginManager.h"
#include "model/Project.h"
#include "config/SystemConfig.h"
#include "platform/Platform.h"
#include "platform/PathUtils.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

namespace DeepLux {

AgentBridge::AgentBridge(QObject* parent)
    : QObject(parent)
    , m_server(nullptr)
    , m_heartbeatTimer(new QTimer(this))
{
    // 心跳定时器 - 10 秒发送一次
    m_heartbeatTimer->setInterval(10000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &AgentBridge::onHeartbeatTimeout);

    // 注册默认查询处理器
    registerDefaultQueryHandlers();
}

void AgentBridge::registerDefaultQueryHandlers()
{
    // project query
    registerQueryHandler("project", [this](const QJsonObject& params) {
        Q_UNUSED(params);
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) {
            return QJsonObject{
                {"name", proj->name()},
                {"path", proj->filePath()},
                {"moduleCount", proj->modules().size()}
            };
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
        case RunState::Idle: stateStr = "idle"; break;
        case RunState::Running: stateStr = "running"; break;
        case RunState::Paused: stateStr = "paused"; break;
        case RunState::Stopped: stateStr = "stopped"; break;
        default: stateStr = "unknown"; break;
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

AgentBridge::~AgentBridge()
{
    stop();
}

AgentBridge& AgentBridge::instance()
{
    static AgentBridge instance;
    return instance;
}

bool AgentBridge::start()
{
    if (m_running) return true;

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
            qWarning() << "Failed to start AgentBridge fallback server:" << fallbackSocketName << m_server->serverError()
                       << m_server->errorString();
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

void AgentBridge::stop()
{
    if (!m_running) return;

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
        m_clientSocketToId.clear();
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

void AgentBridge::registerQueryHandler(const QString& target, QueryHandler handler)
{
    m_queryHandlers[target] = handler;
}

void AgentBridge::setToolCallCallback(ToolCallCallback callback)
{
    m_toolCallCallback = callback;
}

void AgentBridge::sendEvent(const QString& event, const QJsonObject& payload)
{
    broadcastEvent(event, payload);
}

void AgentBridge::onNewConnection()
{
    QLocalSocket* socket = m_server->nextPendingConnection();
    if (!socket) return;

    QString clientId = QString("agent-%1").arg(reinterpret_cast<quintptr>(socket), 8, 16, QChar('0'));

    AgentConnection* conn = new AgentConnection(socket, clientId, this);
    {
        QMutexLocker locker(&m_connectionMutex);
        m_connections.append(conn);
        m_clientSocketToId[socket] = clientId;
        m_missedHeartbeats[clientId] = 0;
    }

    connect(conn, &AgentConnection::messageReceived, this, &AgentBridge::onClientMessage);
    connect(conn, &AgentConnection::disconnected, this, &AgentBridge::onClientDisconnected);

    qDebug() << "Agent connected:" << clientId;
    emit agentConnected(clientId);
}

void AgentBridge::onClientMessage(const QString& clientId, const QJsonObject& msg)
{
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

AgentBridge::ProtocolResponse AgentBridge::processMessage(const QString& clientId, const QJsonObject& msg)
{
    QString type = msg.value("type").toString();
    QString reqId = msg.value("id").toString();
    QJsonObject payload = msg.value("payload").toObject();

    ProtocolResponse response;
    response.reqId = reqId;
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

void AgentBridge::onClientDisconnected(const QString& clientId)
{
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
    }
    emit agentDisconnected(clientId);
    qDebug() << "Agent disconnected:" << clientId;
}

void AgentBridge::registerEventSubscription(const QString& clientId, const QString& event)
{
    QMutexLocker locker(&m_connectionMutex);
    if (!m_eventSubscriptions.contains(clientId)) {
        m_eventSubscriptions[clientId] = QStringList();
    }
    if (!m_eventSubscriptions[clientId].contains(event)) {
        m_eventSubscriptions[clientId].append(event);
        qDebug() << "Agent" << clientId << "subscribed to event:" << event;
    }
}

void AgentBridge::onHeartbeatTimeout()
{
    QJsonObject pingMsg;
    pingMsg["version"] = PROTOCOL_VERSION;
    pingMsg["type"] = "ping";

    QStringList timedOutClients;
    QList<AgentConnection*> toRemove;

    {
        QMutexLocker locker(&m_connectionMutex);
        for (AgentConnection* conn : m_connections) {
            if (conn->isConnected()) {
                conn->send(pingMsg);
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
        }
    }

    // 在锁外执行 deleteLater 和信号发射
    for (AgentConnection* conn : toRemove) {
        conn->deleteLater();
    }
    for (const QString& clientId : timedOutClients) {
        qDebug() << "Agent heartbeat timeout:" << clientId;
        emit agentConnectionLost(clientId);
        QTimer::singleShot(5000, this, [this, clientId]() {
            attemptReconnect(clientId);
        });
    }
}

void AgentBridge::attemptReconnect(const QString& clientId)
{
    qDebug() << "Attempting to reconnect agent:" << clientId;
    // 重连逻辑 - 在此简单实现，实际可能需要更复杂的重连策略
}

QJsonObject AgentBridge::handleToolCall(const QString& reqId, const QJsonObject& payload)
{
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

QJsonObject AgentBridge::handleQuery(const QString& reqId, const QJsonObject& payload)
{
    Q_UNUSED(reqId);

    QString target = payload.value("target").toString();

    if (m_queryHandlers.contains(target)) {
        return m_queryHandlers[target](payload);
    }

    return QJsonObject{{"error", "Unknown query target: " + target}};
}

QJsonObject AgentBridge::handlePing(const QString& reqId)
{
    Q_UNUSED(reqId);
    QJsonObject result;
    result["type"] = "pong";
    return result;
}

void AgentBridge::sendResponse(const QString& clientId, const QString& reqId, const QJsonObject& payload)
{
    QJsonObject msg;
    msg["version"] = PROTOCOL_VERSION;
    msg["type"] = "result";
    msg["id"] = reqId;
    msg["payload"] = payload;

    QMutexLocker locker(&m_connectionMutex);
    for (AgentConnection* conn : m_connections) {
        if (conn->clientId() == clientId) {
            conn->send(msg);
            break;
        }
    }
}

void AgentBridge::sendError(const QString& clientId, const QString& reqId, const QString& errorMsg)
{
    QJsonObject msg;
    msg["version"] = PROTOCOL_VERSION;
    msg["type"] = "error";
    msg["id"] = reqId;
    QJsonObject errPayload;
    errPayload["message"] = errorMsg;
    msg["payload"] = errPayload;

    QMutexLocker locker(&m_connectionMutex);
    for (AgentConnection* conn : m_connections) {
        if (conn->clientId() == clientId) {
            conn->send(msg);
            break;
        }
    }
}

void AgentBridge::broadcastEvent(const QString& event, const QJsonObject& payload)
{
    QJsonObject msg;
    msg["version"] = PROTOCOL_VERSION;
    msg["type"] = "event";
    msg["event"] = event;
    msg["payload"] = payload;

    QMutexLocker locker(&m_connectionMutex);
    for (auto it = m_eventSubscriptions.begin(); it != m_eventSubscriptions.end(); ++it) {
        const QString& clientId = it.key();
        const QStringList& events = it.value();

        if (events.contains(event) || events.contains("*")) {
            for (AgentConnection* conn : m_connections) {
                if (conn->clientId() == clientId && conn->isConnected()) {
                    conn->send(msg);
                    break;
                }
            }
        }
    }
}

} // namespace DeepLux
