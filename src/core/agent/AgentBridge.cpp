#include "AgentBridge.h"
#include "AgentConnection.h"

#include "manager/ProjectManager.h"
#include "engine/RunEngine.h"
#include "manager/PluginManager.h"
#include "model/Project.h"
#include "config/SystemConfig.h"
#include "platform/Platform.h"

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
    m_socketPath = "/run/deeplux/agent.sock";
    QDir().mkpath(QFileInfo(m_socketPath).dir().path());
#elif defined(Q_OS_WINDOWS)
    m_socketPath = "\\\\.\\pipe\\deeplux_agent";
#endif

    m_server = new QLocalServer(this);

    // 设置连接接受信号
    connect(m_server, &QLocalServer::newConnection, this, &AgentBridge::onNewConnection);

    // 尝试删除旧的 socket 文件（Linux）
#if defined(Q_OS_LINUX)
    if (QFileInfo::exists(m_socketPath)) {
        QFile::remove(m_socketPath);
    }
#endif

    if (!m_server->listen(m_socketPath)) {
        qWarning() << "Failed to start AgentBridge server:" << m_server->errorString();
        return false;
    }

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
    for (AgentConnection* conn : m_connections) {
        conn->deleteLater();
    }
    m_connections.clear();

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

#if defined(Q_OS_LINUX)
    if (QFileInfo::exists(m_socketPath)) {
        QFile::remove(m_socketPath);
    }
#endif

    qDebug() << "AgentBridge stopped";
}

void AgentBridge::registerQueryHandler(const QString& target, QueryHandler handler)
{
    m_queryHandlers[target] = handler;
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
    m_connections.append(conn);
    m_clientSocketToId[socket] = clientId;
    m_missedHeartbeats[clientId] = 0;

    connect(conn, &AgentConnection::messageReceived, this, &AgentBridge::onClientMessage);
    connect(conn, &AgentConnection::disconnected, this, &AgentBridge::onClientDisconnected);

    qDebug() << "Agent connected:" << clientId;
    emit agentConnected(clientId);
}

void AgentBridge::onClientMessage(const QString& clientId, const QJsonObject& msg)
{
    QString type = msg.value("type").toString();
    QString reqId = msg.value("id").toString();
    QJsonObject payload = msg.value("payload").toObject();

    QJsonObject result;

    if (type == "execute") {
        result = handleExecute(reqId, payload);
    } else if (type == "query") {
        result = handleQuery(reqId, payload);
    } else if (type == "ping") {
        result = handlePing(reqId);
    } else if (type == "subscribe") {
        // 处理事件订阅
        QString event = payload.value("event").toString();
        registerEventSubscription(clientId, event);
        result = QJsonObject{{"status", "subscribed"}, {"event", event}};
    } else {
        // 未知消息类型
        sendError(clientId, reqId, "Unknown message type: " + type);
        return;
    }

    // 重置心跳计数
    m_missedHeartbeats[clientId] = 0;

    sendResponse(clientId, reqId, result);
}

void AgentBridge::onClientDisconnected(const QString& clientId)
{
    // 找到并移除连接
    for (int i = 0; i < m_connections.size(); ++i) {
        if (m_connections[i]->clientId() == clientId) {
            m_connections[i]->deleteLater();
            m_connections.removeAt(i);
            break;
        }
    }

    m_missedHeartbeats.remove(clientId);
    m_eventSubscriptions.remove(clientId);  // 清理事件订阅
    emit agentDisconnected(clientId);
    qDebug() << "Agent disconnected:" << clientId;
}

void AgentBridge::registerEventSubscription(const QString& clientId, const QString& event)
{
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
    // 发送 ping 到所有连接
    QJsonObject pingMsg;
    pingMsg["version"] = PROTOCOL_VERSION;
    pingMsg["type"] = "ping";

    for (AgentConnection* conn : m_connections) {
        if (conn->isConnected()) {
            conn->send(pingMsg);
        }
    }

    // 检查超时
    QStringList timedOutClients;
    for (auto it = m_missedHeartbeats.begin(); it != m_missedHeartbeats.end(); ++it) {
        it.value()++;
        if (it.value() >= 3) {  // 30 秒无响应
            timedOutClients.append(it.key());
        }
    }

    // 断开超时的客户端
    for (const QString& clientId : timedOutClients) {
        qDebug() << "Agent heartbeat timeout:" << clientId;
        emit agentConnectionLost(clientId);

        // 5 秒后尝试重连
        QTimer::singleShot(5000, this, [this, clientId]() {
            attemptReconnect(clientId);
        });

        // 移除连接
        for (int i = 0; i < m_connections.size(); ++i) {
            if (m_connections[i]->clientId() == clientId) {
                m_connections[i]->deleteLater();
                m_connections.removeAt(i);
                break;
            }
        }

        m_missedHeartbeats.remove(clientId);
    }
}

void AgentBridge::attemptReconnect(const QString& clientId)
{
    qDebug() << "Attempting to reconnect agent:" << clientId;
    // 重连逻辑 - 在此简单实现，实际可能需要更复杂的重连策略
}

QJsonObject AgentBridge::handleExecute(const QString& reqId, const QJsonObject& payload)
{
    Q_UNUSED(reqId);

    QString command = payload.value("command").toString();
    Q_UNUSED(command);

    // TODO: 通过信号触发命令执行
    return QJsonObject{
        {"exitCode", 0},
        {"message", "Command queued"}
    };
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
    QJsonObject result;
    result["type"] = "pong";
    return result;
}

void AgentBridge::sendResponse(const QString& clientId, const QString& reqId, const QJsonObject& payload)
{
    for (AgentConnection* conn : m_connections) {
        if (conn->clientId() == clientId) {
            QJsonObject msg;
            msg["version"] = PROTOCOL_VERSION;
            msg["type"] = "result";
            msg["id"] = reqId;
            msg["payload"] = payload;
            conn->send(msg);
            break;
        }
    }
}

void AgentBridge::sendError(const QString& clientId, const QString& reqId, const QString& errorMsg)
{
    for (AgentConnection* conn : m_connections) {
        if (conn->clientId() == clientId) {
            QJsonObject msg;
            msg["version"] = PROTOCOL_VERSION;
            msg["type"] = "error";
            msg["id"] = reqId;
            QJsonObject errPayload;
            errPayload["message"] = errorMsg;
            msg["payload"] = errPayload;
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

    // 只发送给订阅了该事件的客户端
    for (auto it = m_eventSubscriptions.begin(); it != m_eventSubscriptions.end(); ++it) {
        const QString& clientId = it.key();
        const QStringList& events = it.value();

        if (events.contains(event) || events.contains("*")) {
            // 找到对应的连接
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
