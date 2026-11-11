#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/common/UserManager.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>
#include <atomic>

using namespace DeepLux;

namespace {

class TcpPeer final {
public:
    TcpPeer(QByteArray expectedRequest, QByteArray response, bool sendResponse = true)
        : m_expectedRequest(std::move(expectedRequest)), m_response(std::move(response)), m_sendResponse(sendResponse) {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (m_server.hasPendingConnections()) {
                QTcpSocket* socket = m_server.nextPendingConnection();
                ++m_connectionCount;
                QObject::connect(socket, &QTcpSocket::readyRead, &m_server, [this, socket] {
                    m_received += socket->readAll();
                    if (!m_replied && m_received == m_expectedRequest) {
                        m_replied = true;
                        if (m_sendResponse) {
                            socket->write(m_response);
                            socket->flush();
                        }
                    }
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen() {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const {
        return m_server.serverPort();
    }

    int connectionCount() const {
        return m_connectionCount;
    }

    QByteArray received() const {
        return m_received;
    }

private:
    QTcpServer m_server;
    QByteArray m_expectedRequest;
    QByteArray m_response;
    QByteArray m_received;
    int m_connectionCount = 0;
    bool m_replied = false;
    bool m_sendResponse = true;
};

class ThreadedTcpPeer final {
public:
    ThreadedTcpPeer(QByteArray expectedRequest, QByteArray response)
        : m_expectedRequest(std::move(expectedRequest)), m_response(std::move(response)) {
        m_worker = new QObject;
        m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
    }

    ~ThreadedTcpPeer() {
        if (m_worker && m_thread.isRunning()) {
            const bool queued = QMetaObject::invokeMethod(
                m_worker,
                [this] {
                    if (m_server) {
                        m_server->close();
                        delete m_server;
                        m_server = nullptr;
                    }
                    m_thread.quit();
                },
                Qt::QueuedConnection);
            if (!queued)
                qFatal("TCP peer shutdown could not be queued");
            if (!m_thread.wait(5000))
                qFatal("TCP peer worker did not stop within 5000 ms");
        }
        m_worker = nullptr;
        m_server = nullptr;
    }

    bool listen() {
        if (!m_worker || !m_thread.isRunning())
            return false;
        bool listening = false;
        QMetaObject::invokeMethod(
            m_worker,
            [this, &listening] {
                m_server = new QTcpServer(m_worker);
                QObject::connect(m_server, &QTcpServer::newConnection, m_worker, [this] {
                    while (m_server->hasPendingConnections()) {
                        QTcpSocket* socket = m_server->nextPendingConnection();
                        {
                            QMutexLocker locker(&m_stateMutex);
                            ++m_connectionCount;
                        }
                        QObject::connect(socket, &QTcpSocket::readyRead, m_worker, [this, socket] {
                            const QByteArray bytes = socket->readAll();
                            bool respond = false;
                            {
                                QMutexLocker locker(&m_stateMutex);
                                m_received.append(bytes);
                                respond = !m_replied && m_received == m_expectedRequest;
                                if (respond)
                                    m_replied = true;
                            }
                            if (respond) {
                                socket->write(m_response);
                                socket->flush();
                            }
                        });
                        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    }
                });
                listening = m_server->listen(QHostAddress::LocalHost, 0);
                if (listening) {
                    QMutexLocker locker(&m_stateMutex);
                    m_port = m_server->serverPort();
                }
            },
            Qt::BlockingQueuedConnection);
        return listening;
    }

    quint16 port() const {
        QMutexLocker locker(&m_stateMutex);
        return m_port;
    }

    int connectionCount() const {
        QMutexLocker locker(&m_stateMutex);
        return m_connectionCount;
    }

    QByteArray received() const {
        QMutexLocker locker(&m_stateMutex);
        return m_received;
    }

    bool waitForRequestAndNoResponse(int timeoutMs) const {
        QElapsedTimer timer;
        timer.start();
        QElapsedTimer quiet;
        int previousConnections = -1;
        QByteArray previousBytes;
        while (timer.elapsed() < timeoutMs) {
            const int connections = connectionCount();
            const QByteArray bytes = received();
            if (connections > 0 && bytes.size() >= m_expectedRequest.size()) {
                if (!quiet.isValid() || connections != previousConnections || bytes != previousBytes)
                    quiet.start();
                if (quiet.elapsed() >= 250)
                    return connections == 1 && bytes == m_expectedRequest && m_response.isEmpty();
            } else {
                quiet.invalidate();
            }
            previousConnections = connections;
            previousBytes = bytes;
            QTest::qWait(10);
        }
        return false;
    }

private:
    QByteArray m_expectedRequest;
    QByteArray m_response;
    mutable QMutex m_stateMutex;
    int m_connectionCount = 0;
    quint16 m_port = 0;
    QByteArray m_received;
    bool m_replied = false;
    QThread m_thread;
    QObject* m_worker = nullptr;
    QTcpServer* m_server = nullptr;
};

class ThreadedTcpCyclePeer final {
public:
    ThreadedTcpCyclePeer(QByteArray request, QByteArray response, int withholdResponseAtRequestIndex = -1)
        : m_expectedRequest(std::move(request)), m_response(std::move(response)),
          m_withholdResponseAtRequestIndex(withholdResponseAtRequestIndex) {
        m_worker = new QObject;
        m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
    }

    ~ThreadedTcpCyclePeer() {
        if (m_worker && m_thread.isRunning()) {
            const bool queued = QMetaObject::invokeMethod(
                m_worker,
                [this] {
                    if (m_server) {
                        m_server->close();
                        delete m_server;
                        m_server = nullptr;
                    }
                    m_thread.quit();
                },
                Qt::QueuedConnection);
            if (!queued)
                qFatal("TCP cycle peer shutdown could not be queued");
            if (!m_thread.wait(5000))
                qFatal("TCP cycle peer worker did not stop within 5000 ms");
        }
        m_worker = nullptr;
        m_server = nullptr;
    }

    bool listen() {
        bool listening = false;
        QMetaObject::invokeMethod(
            m_worker,
            [this, &listening] {
                m_server = new QTcpServer(m_worker);
                QObject::connect(m_server, &QTcpServer::newConnection, m_worker, [this] {
                    while (m_server->hasPendingConnections()) {
                        QTcpSocket* socket = m_server->nextPendingConnection();
                        {
                            QMutexLocker locker(&m_mutex);
                            ++m_connectionCount;
                        }
                        m_buffers.insert(socket, {});
                        QObject::connect(socket, &QTcpSocket::readyRead, m_worker, [this, socket] {
                            const QByteArray bytes = socket->readAll();
                            bool shouldReply = false;
                            {
                                QMutexLocker locker(&m_mutex);
                                m_receivedByteCount += static_cast<quint64>(bytes.size());
                                if (m_repliedSockets.contains(socket) && !bytes.isEmpty())
                                    m_invalidRequest = true;
                                m_buffers[socket].append(bytes);
                                const QByteArray received = m_buffers.value(socket);
                                if (!m_repliedSockets.contains(socket) && received.size() >= m_expectedRequest.size()) {
                                    if (received == m_expectedRequest) {
                                        const int requestIndex = m_requests.size();
                                        m_requests.append(received);
                                        m_repliedSockets.insert(socket);
                                        shouldReply = requestIndex != m_withholdResponseAtRequestIndex;
                                    } else {
                                        m_invalidRequest = true;
                                    }
                                }
                            }
                            if (shouldReply) {
                                const qint64 accepted = socket->write(m_response);
                                socket->flush();
                                QMutexLocker locker(&m_mutex);
                                if (accepted == m_response.size())
                                    m_responses.append(m_response);
                                else
                                    m_invalidRequest = true;
                            }
                        });
                        QObject::connect(socket, &QTcpSocket::disconnected, m_worker, [this, socket] {
                            m_buffers.remove(socket);
                            m_repliedSockets.remove(socket);
                            socket->deleteLater();
                        });
                    }
                });
                listening = m_server->listen(QHostAddress::LocalHost, 0);
                if (listening) {
                    QMutexLocker locker(&m_mutex);
                    m_port = m_server->serverPort();
                }
            },
            Qt::BlockingQueuedConnection);
        return listening;
    }

    quint16 port() const {
        QMutexLocker locker(&m_mutex);
        return m_port;
    }
    int connectionCount() const {
        QMutexLocker locker(&m_mutex);
        return m_connectionCount;
    }
    QList<QByteArray> requests() const {
        QMutexLocker locker(&m_mutex);
        return m_requests;
    }
    QList<QByteArray> responses() const {
        QMutexLocker locker(&m_mutex);
        return m_responses;
    }
    bool hasInvalidRequest() const {
        QMutexLocker locker(&m_mutex);
        return m_invalidRequest;
    }
    quint64 receivedByteCount() const {
        QMutexLocker locker(&m_mutex);
        return m_receivedByteCount;
    }

private:
    QByteArray m_expectedRequest;
    QByteArray m_response;
    int m_withholdResponseAtRequestIndex = -1;
    mutable QMutex m_mutex;
    int m_connectionCount = 0;
    quint16 m_port = 0;
    quint64 m_receivedByteCount = 0;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QSet<QTcpSocket*> m_repliedSockets;
    QList<QByteArray> m_requests;
    QList<QByteArray> m_responses;
    bool m_invalidRequest = false;
    QThread m_thread;
    QObject* m_worker = nullptr;
    QTcpServer* m_server = nullptr;
};

} // namespace

class TestAgentTCPClientExecutionG5 : public QObject {
    Q_OBJECT

private:
    struct EnvironmentRestore {
        QByteArray appDataDir = qgetenv("DEEPLUX_APP_DATA_DIR");
        bool hadAppDataDir = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");

        ~EnvironmentRestore() {
            if (hadAppDataDir)
                qputenv("DEEPLUX_APP_DATA_DIR", appDataDir);
            else
                qunsetenv("DEEPLUX_APP_DATA_DIR");
        }
    };

    EnvironmentRestore m_environmentRestore;
    QTemporaryDir m_appDataDir;

    static bool installTCPClientPlugin(const QString& pluginRoot) {
        const QDir pluginDir(QDir(pluginRoot).filePath(QStringLiteral("TCPClient")));
        if (!QDir().mkpath(pluginDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_TCPCLIENT_G5_METADATA),
                           pluginDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_TCPCLIENT_G5_LIBRARY),
                           pluginDir.filePath(QFileInfo(QStringLiteral(AGENT_TCPCLIENT_G5_LIBRARY)).fileName()));
    }

    static int moduleSignalCount(const QSignalSpy& spy, const QString& instanceId) {
        int count = 0;
        for (const QList<QVariant>& args : spy)
            if (!args.isEmpty() && args.first().toString() == instanceId)
                ++count;
        return count;
    }

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins"));
        QVERIFY(installTCPClientPlugin(pluginRoot));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        QVERIFY(plugins.pluginPaths().contains(QDir(pluginRoot).absolutePath()));
        const QList<PluginInfo> scanned = plugins.moduleInfos();
        QCOMPARE(scanned.size(), 1);
        QCOMPARE(scanned.first().id, QStringLiteral("com.deeplux.plugin.tcpclient"));
        QCOMPARE(scanned.first().name, QStringLiteral("TCP客户端"));
        QVERIFY(plugins.cameraInfos().isEmpty());
        QVERIFY(plugins.loadPlugin(QStringLiteral("TCP客户端")));
        QVERIFY(AgentController::instance().initialize());
    }

    void cleanupTestCase() {
        RunEngine& engine = RunEngine::instance();
        engine.stop();
        QElapsedTimer wait;
        wait.start();
        while (engine.isBusy() && wait.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QTest::qWait(10);
        }
        if (engine.isBusy()) {
            m_appDataDir.setAutoRemove(false);
            qCritical() << "TCPClient execution did not become idle; preserving plugin directory"
                        << m_appDataDir.path();
            QFAIL("RunEngine remained busy; temporary TCPClient plugin tree was preserved");
            return;
        }
        engine.clearModules();
        ProjectManager::instance().closeProject();
        AgentController::instance().shutdown();
        PluginManager::instance().shutdown();
    }

    void testRunFlowBindsEndpointExpiresOnPortChangeAndReadsResponse() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QByteArray request = QByteArrayLiteral("slice48-request");
        const QByteArray responseBytes = QByteArrayLiteral("slice48-response");
        ThreadedTcpPeer stalePeer(request, responseBytes);
        ThreadedTcpPeer approvedPeer(request, responseBytes);
        QVERIFY(stalePeer.listen());
        QVERIFY(approvedPeer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice48_tcpclient_target");
        ModuleInstance tcpClient;
        tcpClient.id = instanceId;
        tcpClient.moduleId = QStringLiteral("TCP客户端");
        tcpClient.name = QStringLiteral("Slice 48 loopback TCPClient");
        tcpClient.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                       {QStringLiteral("port"), stalePeer.port()},
                                       {QStringLiteral("timeout"), 1000},
                                       {QStringLiteral("writeData"), QString::fromUtf8(request)},
                                       {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                       {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        project->addModule(tcpClient);

        RunEngine& runEngine = engine;
        QSignalSpy moduleStarted(&runEngine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&runEngine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&runEngine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy response(&controller, &AgentController::llmResponseReceived);
        std::atomic<quintptr> executionThreadId{0};
        const QMetaObject::Connection threadConnection = connect(
            &runEngine, &RunEngine::moduleStarted, &runEngine,
            [&executionThreadId, instanceId](const QString& startedId) {
                if (startedId == instanceId)
                    executionThreadId.store(reinterpret_cast<quintptr>(QThread::currentThreadId()),
                                            std::memory_order_relaxed);
            },
            Qt::DirectConnection);
        const quintptr mainThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());

        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                                        QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray staleEffects{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                                      QStringLiteral("tcp_client:%1@127.0.0.1:%2 operation=WriteRead timeout=1000ms")
                                          .arg(instanceId)
                                          .arg(stalePeer.port())};
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 staleEffects);
        QVERIFY(!QJsonDocument(staleEffects).toJson(QJsonDocument::Compact).contains(request));
        QVERIFY(
            !QJsonDocument(controller.pendingConfirmationPayload()).toJson(QJsonDocument::Compact).contains(request));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("port"), approvedPeer.port()));
        const int auditCountBeforeStaleConfirmation = audit.count();
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeStaleConfirmation, 5000);
        QCOMPARE(stalePeer.connectionCount(), 0);
        QCOMPARE(approvedPeer.connectionCount(), 0);
        QCOMPARE(stalePeer.received(), QByteArray());
        QCOMPARE(approvedPeer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(!response.isEmpty());
        QVERIFY(response.last().at(0).toString().contains(QStringLiteral("确认已失效")));

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray approvedEffects{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                                         QStringLiteral("tcp_client:%1@127.0.0.1:%2 operation=WriteRead timeout=1000ms")
                                             .arg(instanceId)
                                             .arg(approvedPeer.port())};
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 approvedEffects);
        QVERIFY(
            !QJsonDocument(controller.pendingConfirmationPayload()).toJson(QJsonDocument::Compact).contains(request));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != staleId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(started.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);

        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success,
                 qPrintable(QStringLiteral("%1; stale connections=%2 bytes=%3; approved connections=%4 bytes=%5")
                                .arg(result.errorMessage)
                                .arg(stalePeer.connectionCount())
                                .arg(stalePeer.received().size())
                                .arg(approvedPeer.connectionCount())
                                .arg(approvedPeer.received().size())));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(!engine.isBusy());
        QCOMPARE(stalePeer.connectionCount(), 0);
        QCOMPARE(stalePeer.received(), QByteArray());
        QCOMPARE(approvedPeer.connectionCount(), 1);
        QCOMPARE(approvedPeer.received(), request);
        QVERIFY(executionThreadId.load(std::memory_order_relaxed) != 0);
        QVERIFY(executionThreadId.load(std::memory_order_relaxed) != mainThreadId);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject agentValue = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_reply")}});
        QCOMPARE(agentValue.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(agentValue.value(QStringLiteral("runId")).toString(), runId);
        QCOMPARE(agentValue.value(QStringLiteral("value")).toString(), QString::fromUtf8(responseBytes));
        const QJsonObject byteCount =
            controller.handleToolCall(QStringLiteral("get_module_output"),
                                      QJsonObject{{QStringLiteral("runId"), runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("agent_reply_bytes")}});
        QCOMPARE(byteCount.value(QStringLiteral("value")).toInt(), responseBytes.size());

        disconnect(threadConnection);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testConnectTimeoutReportsEndpointAndBudget() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        QTcpServer listeningPeer;
        QVERIFY(listeningPeer.listen(QHostAddress::LocalHost, 0));
        const quint16 targetPort = listeningPeer.serverPort();

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice48_tcpclient_connect_failure");
        constexpr int timeoutMs = 0;
        ModuleInstance tcpClient;
        tcpClient.id = instanceId;
        tcpClient.moduleId = QStringLiteral("TCP客户端");
        tcpClient.name = QStringLiteral("Slice 48 TCPClient connection failure");
        tcpClient.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                       {QStringLiteral("port"), targetPort},
                                       {QStringLiteral("timeout"), timeoutMs},
                                       {QStringLiteral("writeData"), QStringLiteral("must-not-send")},
                                       {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                       {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        project->addModule(tcpClient);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                                        QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray expectedEffects{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                                         QStringLiteral("tcp_client:%1@127.0.0.1:%2 operation=WriteRead timeout=0ms")
                                             .arg(instanceId)
                                             .arg(targetPort)};
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);

        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY(!result.success);
        QCOMPARE(result.failedModule, instanceId);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QVERIFY(result.errorMessage.contains(QStringLiteral("127.0.0.1")));
        QVERIFY(result.errorMessage.contains(QString::number(targetPort)));
        QVERIFY(result.errorMessage.contains(QStringLiteral("超时 (%1ms)").arg(timeoutMs)));
        const QJsonObject runStatus =
            controller.handleToolCall(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(runStatus.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(runStatus.value(QStringLiteral("lastError"))
                    .toObject()
                    .value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("超时 (%1ms)").arg(timeoutMs)));
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testWriteReadTimeoutIsUncertainAndPollingDoesNotRetry() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QByteArray request = QByteArrayLiteral("slice61-uncertain-request");
        TcpPeer peer(request, {}, false);
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice61_tcpclient_uncertain");
        ModuleInstance tcpClient;
        tcpClient.id = instanceId;
        tcpClient.moduleId = QStringLiteral("TCP客户端");
        tcpClient.name = QStringLiteral("Slice 61 TCPClient uncertain WriteRead");
        tcpClient.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                       {QStringLiteral("port"), peer.port()},
                                       {QStringLiteral("timeout"), 100},
                                       {QStringLiteral("writeData"), QString::fromUtf8(request)},
                                       {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                       {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        project->addModule(tcpClient);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);

        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                                        QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);

        pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);

        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(failed.runId, runId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(failed.failedModule, instanceId);
        QVERIFY(!failed.success);
        const QJsonObject agentResult = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                                        QJsonObject{{QStringLiteral("runId"), runId}});
        const QString runError =
            agentResult.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("message")).toString();
        QVERIFY(runError.contains(QStringLiteral("127.0.0.1")));
        QVERIFY(runError.contains(QString::number(peer.port())));
        QVERIFY(runError.contains(QStringLiteral("100ms")));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QTRY_COMPARE_WITH_TIMEOUT(peer.connectionCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.received(), request, 5000);
        QVERIFY(!engine.isBusy());

        for (int i = 0; i < 2; ++i) {
            const QJsonObject results = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                                        QJsonObject{{QStringLiteral("runId"), runId}});
            QCOMPARE(results.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QCOMPARE(results.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!results.value(QStringLiteral("retryable")).toBool());
        }
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.received(), request);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testTCPClientReadWriteAndEmptyWriteReadFailureBoundaries() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        auto runOnce = [&](const QString& instanceId, const QString& operation, const QString& writeData, TcpPeer& peer,
                           bool expectedSuccess, bool expectedUncertain) {
            Project* project = ProjectManager::instance().newProject();
            if (!project)
                return false;
            ModuleInstance tcpClient;
            tcpClient.id = instanceId;
            tcpClient.moduleId = QStringLiteral("TCP客户端");
            tcpClient.name = QStringLiteral("Slice 61 TCPClient boundary");
            tcpClient.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                           {QStringLiteral("port"), peer.port()},
                                           {QStringLiteral("timeout"), 100},
                                           {QStringLiteral("writeData"), writeData},
                                           {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                           {QStringLiteral("operation"), operation}};
            project->addModule(tcpClient);
            QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
            const QJsonObject pending = controller.handleToolCall(
                QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
            if (pending.value(QStringLiteral("status")).toString() != QStringLiteral("pending_confirmation") ||
                !controller.confirmPendingTools(controller.pendingConfirmationId()))
                return false;
            QElapsedTimer wait;
            wait.start();
            while (toolExecuted.isEmpty() && wait.elapsed() < 5000) {
                QCoreApplication::processEvents();
                QTest::qWait(1);
            }
            if (toolExecuted.isEmpty())
                return false;
            const QString runId = toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("runId")).toString();
            while (runFinished.isEmpty() && wait.elapsed() < 5000) {
                QCoreApplication::processEvents();
                QTest::qWait(1);
            }
            if (runId.isEmpty() || runFinished.isEmpty())
                return false;
            const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
            const QJsonObject agentResult = controller.actor()->executeTool(
                QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
            const QString expectedAgentStatus =
                expectedUncertain ? QStringLiteral("uncertain")
                                  : (expectedSuccess ? QStringLiteral("completed") : QStringLiteral("failed"));
            const bool statusMatches = result.runId == runId && result.success == expectedSuccess &&
                                       ((result.errorCode == ExecError::SideEffectUncertain) == expectedUncertain) &&
                                       moduleSignalCount(moduleStarted, instanceId) == 1 &&
                                       agentResult.value(QStringLiteral("runId")).toString() == runId &&
                                       agentResult.value(QStringLiteral("status")).toString() == expectedAgentStatus;
            engine.clearModules();
            ProjectManager::instance().closeProject();
            QCoreApplication::processEvents();
            return statusMatches;
        };

        const QByteArray writePayload = QByteArrayLiteral("slice61-oneway");
        TcpPeer writePeer(writePayload, {}, false);
        QVERIFY(writePeer.listen());
        QVERIFY(runOnce(QStringLiteral("slice61_tcpclient_write"), QStringLiteral("Write"),
                        QString::fromUtf8(writePayload), writePeer, true, false));
        QTRY_COMPARE_WITH_TIMEOUT(writePeer.connectionCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(writePeer.received(), writePayload, 5000);

        TcpPeer readPeer({}, {}, false);
        QVERIFY(readPeer.listen());
        QVERIFY(runOnce(QStringLiteral("slice61_tcpclient_read"), QStringLiteral("Read"), {}, readPeer, false, false));
        QCOMPARE(readPeer.connectionCount(), 1);
        QCOMPARE(readPeer.received(), QByteArray());

        TcpPeer emptyWriteReadPeer({}, {}, false);
        QVERIFY(emptyWriteReadPeer.listen());
        QVERIFY(runOnce(QStringLiteral("slice61_tcpclient_empty_write_read"), QStringLiteral("WriteRead"), {},
                        emptyWriteReadPeer, false, false));
        QCOMPARE(emptyWriteReadPeer.connectionCount(), 1);
        QCOMPARE(emptyWriteReadPeer.received(), QByteArray());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testStepOnceWriteTimeoutIsUncertainAndDoesNotRetry() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        constexpr int timeoutMs = 150;
        const QByteArray request = QByteArrayLiteral("slice81-step-uncertain-request");
        ThreadedTcpPeer peer(request, {});
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice81_tcpclient_step_uncertain");
        ModuleInstance client;
        client.id = instanceId;
        client.moduleId = QStringLiteral("TCP客户端");
        client.name = QStringLiteral("Slice 81 TCPClient step uncertain WriteRead");
        client.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                    {QStringLiteral("port"), peer.port()},
                                    {QStringLiteral("timeout"), timeoutMs},
                                    {QStringLiteral("writeData"), QString::fromUtf8(request)},
                                    {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                    {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        project->addModule(client);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QString effect = QStringLiteral("tcp_client:%1@127.0.0.1:%2 operation=WriteRead timeout=%3ms")
                                   .arg(instanceId)
                                   .arg(peer.port())
                                   .arg(timeoutMs);

        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(effect));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject step = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(step.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        const QString runId = failed.runId;
        QVERIFY(!runId.isEmpty());
        QVERIFY(peer.waitForRequestAndNoResponse(3000));
        QVERIFY(!engine.isBusy());
        QCOMPARE(failed.runId, runId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(failed.failedModule, instanceId);
        QVERIFY(failed.diagnostics.contains(QStringLiteral("响应超时")));
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.received(), request);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(!moduleFinished.last().at(1).toBool());

        const QJsonObject result = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                                   QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!result.value(QStringLiteral("retryable")).toBool());
        const QJsonObject output = controller.actor()->executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_reply")}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!output.contains(QStringLiteral("value")));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.received(), request);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testStepOnceConfirmsTCPClientSingleSend() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QByteArray request = QByteArrayLiteral("slice73-step-request");
        const QByteArray response = QByteArrayLiteral("slice73-step-response");
        ThreadedTcpPeer peer(request, response);
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice73_tcpclient_step");
        ModuleInstance client;
        client.id = instanceId;
        client.moduleId = QStringLiteral("TCP客户端");
        client.name = QStringLiteral("Slice 73 TCPClient step target");
        client.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                    {QStringLiteral("port"), peer.port()},
                                    {QStringLiteral("timeout"), 1000},
                                    {QStringLiteral("writeData"), QString::fromUtf8(request)},
                                    {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                    {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        project->addModule(client);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QString endpointEffect = QStringLiteral("tcp_client:%1@127.0.0.1:%2 operation=WriteRead timeout=1000ms")
                                           .arg(instanceId)
                                           .arg(peer.port());

        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(endpointEffect));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(endpointEffect));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject stepResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const QString runId = qvariant_cast<RunResult>(runFinished.last().at(0)).runId;
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(peer.connectionCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.received(), request, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(!engine.isBusy());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testResumeFlowWriteTimeoutIsUncertainAndDoesNotRetry() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        constexpr int timeoutMs = 150;
        const QByteArray request = QByteArrayLiteral("slice81-resume-uncertain-request");
        ThreadedTcpPeer peer(request, {});
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice81_tcpclient_resume_uncertain");
        ModuleInstance client;
        client.id = instanceId;
        client.moduleId = QStringLiteral("TCP客户端");
        client.name = QStringLiteral("Slice 81 TCPClient resume uncertain WriteRead");
        client.breakpoint = true;
        client.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                    {QStringLiteral("port"), peer.port()},
                                    {QStringLiteral("timeout"), timeoutMs},
                                    {QStringLiteral("writeData"), QString::fromUtf8(request)},
                                    {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                    {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        project->addModule(client);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString pausedRunId = engine.runObservation().runId;
        QVERIFY(!pausedRunId.isEmpty());
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        const QString effect = QStringLiteral("tcp_client:%1@127.0.0.1:%2 operation=WriteRead timeout=%3ms")
                                   .arg(instanceId)
                                   .arg(peer.port())
                                   .arg(timeoutMs);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(effect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, pausedRunId);
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("resumed"));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QVERIFY(peer.waitForRequestAndNoResponse(3000));
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Idle, 5000);
        QVERIFY(!engine.isBusy());
        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(failed.runId, pausedRunId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(failed.failedModule, instanceId);
        QVERIFY(failed.diagnostics.contains(QStringLiteral("响应超时")));
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.received(), request);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(!moduleFinished.last().at(1).toBool());

        const QJsonObject result = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                                   QJsonObject{{QStringLiteral("runId"), pausedRunId}});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!result.value(QStringLiteral("retryable")).toBool());
        const QJsonObject output = controller.actor()->executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), pausedRunId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_reply")}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!output.contains(QStringLiteral("value")));
        QCOMPARE(
            controller.actor()
                ->executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), pausedRunId}})
                .value(QStringLiteral("status"))
                .toString(),
            QStringLiteral("uncertain"));
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.received(), request);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testResumeFlowConfirmsTCPClientSingleSend() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QByteArray request = QByteArrayLiteral("slice73-resume-request");
        const QByteArray response = QByteArrayLiteral("slice73-resume-response");
        ThreadedTcpPeer peer(request, response);
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice73_tcpclient_resume");
        ModuleInstance client;
        client.id = instanceId;
        client.moduleId = QStringLiteral("TCP客户端");
        client.name = QStringLiteral("Slice 73 TCPClient resume target");
        client.breakpoint = true;
        client.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                    {QStringLiteral("port"), peer.port()},
                                    {QStringLiteral("timeout"), 1000},
                                    {QStringLiteral("writeData"), QString::fromUtf8(request)},
                                    {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                    {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        project->addModule(client);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        const QString endpointEffect = QStringLiteral("tcp_client:%1@127.0.0.1:%2 operation=WriteRead timeout=1000ms")
                                           .arg(instanceId)
                                           .arg(peer.port());
        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(endpointEffect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, runId);
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(endpointEffect));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(peer.connectionCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.received(), request, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 5000);
        QCOMPARE(engine.state(), RunState::Idle);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testBoundedCycleWriteReadConfirmsDriftAndCompletesExactCount() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        constexpr int maxCycles = 3;
        constexpr int timeoutMs = 1000;
        const QByteArray request = QByteArrayLiteral("slice96-cycle-request");
        const QByteArray responseBytes = QByteArrayLiteral("slice96-cycle-response");
        ThreadedTcpCyclePeer stalePeer(request, responseBytes);
        ThreadedTcpCyclePeer approvedPeer(request, responseBytes);
        QVERIFY(stalePeer.listen());
        QVERIFY(approvedPeer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice96_tcpclient_cycle");
        ModuleInstance client;
        client.id = instanceId;
        client.moduleId = QStringLiteral("TCP客户端");
        client.name = QStringLiteral("Slice 96 bounded TCPClient WriteRead");
        client.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                    {QStringLiteral("port"), stalePeer.port()},
                                    {QStringLiteral("timeout"), timeoutMs},
                                    {QStringLiteral("writeData"), QString::fromUtf8(request)},
                                    {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                    {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        project->addModule(client);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QString sessionId;
        QObject resultContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &resultContext,
                [&sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                });
        const QJsonObject cycleParams{{QStringLiteral("mode"), QStringLiteral("cycle")},
                                      {QStringLiteral("maxCycles"), maxCycles}};
        const QString irreversible = QStringLiteral("不可撤销：Agent Undo 不会回滚外部副作用");

        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QJsonObject context =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("context")).toObject();
        QCOMPARE(context.value(QStringLiteral("reversibilityLabel")).toString(), irreversible);
        QJsonArray effects = context.value(QStringLiteral("sideEffects")).toArray();
        QVERIFY(effects.contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
        QVERIFY(effects.contains(QStringLiteral("tcp_client:%1@127.0.0.1:%2 operation=WriteRead timeout=1000ms")
                                     .arg(instanceId)
                                     .arg(stalePeer.port())));
        QCOMPARE(stalePeer.connectionCount(), 0);
        QCOMPARE(approvedPeer.connectionCount(), 0);

        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("port"), approvedPeer.port()));
        const int auditCountBeforeStaleConfirmation = audit.count();
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeStaleConfirmation, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(stalePeer.connectionCount(), 0);
        QVERIFY(stalePeer.requests().isEmpty());
        QVERIFY(stalePeer.responses().isEmpty());
        QCOMPARE(approvedPeer.connectionCount(), 0);
        QVERIFY(approvedPeer.requests().isEmpty());
        QVERIFY(approvedPeer.responses().isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != staleId);
        context = pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("context")).toObject();
        effects = context.value(QStringLiteral("sideEffects")).toArray();
        QVERIFY(effects.contains(QStringLiteral("tcp_client:%1@127.0.0.1:%2 operation=WriteRead timeout=1000ms")
                                     .arg(instanceId)
                                     .arg(approvedPeer.port())));
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QCOMPARE(toolExecuted.count(), 1);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("started"));
        sessionId = toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("sessionId")).toString();
        QVERIFY(!sessionId.isEmpty());
        QCOMPARE(runFinished.count(), maxCycles);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), maxCycles);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), maxCycles);
        QCOMPARE(stalePeer.connectionCount(), 0);
        QVERIFY(stalePeer.requests().isEmpty());
        QCOMPARE(approvedPeer.connectionCount(), maxCycles);
        QCOMPARE(approvedPeer.requests(), (QList<QByteArray>{request, request, request}));
        QCOMPARE(approvedPeer.responses(), (QList<QByteArray>{responseBytes, responseBytes, responseBytes}));
        QVERIFY(!approvedPeer.hasInvalidRequest());
        for (const QList<QVariant>& args : runFinished) {
            const RunResult run = qvariant_cast<RunResult>(args.at(0));
            QVERIFY2(run.success, qPrintable(run.errorMessage));
        }

        QJsonObject session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                        QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QVERIFY(!session.value(QStringLiteral("isBusy")).toBool());
        QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), maxCycles);
        QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), maxCycles);
        const QString lastRunId = qvariant_cast<RunResult>(runFinished.last().at(0)).runId;
        QCOMPARE(session.value(QStringLiteral("lastSuccessfulRunId")).toString(), lastRunId);
        QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(), lastRunId);
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject output = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), lastRunId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_reply")}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(output.value(QStringLiteral("value")).toString(), QString::fromUtf8(responseBytes));
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        QTest::qWait(550);
        session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                            QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(approvedPeer.connectionCount(), maxCycles);
        QCOMPARE(approvedPeer.requests().size(), maxCycles);
        QCOMPARE(approvedPeer.responses().size(), maxCycles);
        QCOMPARE(approvedPeer.receivedByteCount(), static_cast<quint64>(maxCycles * request.size()));
        QVERIFY(!approvedPeer.hasInvalidRequest());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), maxCycles);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), maxCycles);
        QCOMPARE(runFinished.count(), maxCycles);
        QCOMPARE(cycleStopped.count(), 1);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }
    void testBoundedCycleSecondRequestUncertainStopsWithoutThirdRequest() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        constexpr int maxCycles = 3;
        constexpr int timeoutMs = 300;
        const QByteArray request = QByteArrayLiteral("slice100-cycle-request");
        const QByteArray responseBytes = QByteArrayLiteral("slice100-cycle-response");
        ThreadedTcpCyclePeer peer(request, responseBytes, 1);
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice100_tcpclient_cycle_uncertain");
        ModuleInstance client;
        client.id = instanceId;
        client.moduleId = QStringLiteral("TCP客户端");
        client.name = QStringLiteral("Slice 100 TCPClient uncertain bounded cycle");
        client.params = QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                    {QStringLiteral("port"), peer.port()},
                                    {QStringLiteral("timeout"), timeoutMs},
                                    {QStringLiteral("writeData"), QString::fromUtf8(request)},
                                    {QStringLiteral("readVariable"), QStringLiteral("agent_reply")},
                                    {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        project->addModule(client);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QString sessionId;
        QObject resultContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &resultContext,
                [&sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                });
        const QJsonObject cycleParams{{QStringLiteral("mode"), QStringLiteral("cycle")},
                                      {QStringLiteral("maxCycles"), maxCycles}};
        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject confirmation =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("context")).toObject();
        QVERIFY(confirmation.value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
        QCOMPARE(peer.connectionCount(), 0);
        QVERIFY(peer.requests().isEmpty());
        QVERIFY(peer.responses().isEmpty());
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(peer.connectionCount(), 0);
        QVERIFY(peer.requests().isEmpty());
        QVERIFY(peer.responses().isEmpty());
        QCOMPARE(peer.receivedByteCount(), quint64(0));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.requests().size(), 2, 5000);
        QVERIFY(!sessionId.isEmpty());
        QCOMPARE(toolExecuted.count(), 1);

        const RunResult firstRun = qvariant_cast<RunResult>(runFinished.at(0).at(0));
        QVERIFY2(firstRun.success, qPrintable(firstRun.errorMessage));
        const RunResult secondRun = qvariant_cast<RunResult>(runFinished.at(1).at(0));
        QVERIFY(!secondRun.success);
        QCOMPARE(secondRun.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(secondRun.failedModule, instanceId);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 2);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 2);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(peer.connectionCount(), 2);
        QCOMPARE(peer.requests(), (QList<QByteArray>{request, request}));
        QCOMPARE(peer.responses(), (QList<QByteArray>{responseBytes}));
        QCOMPARE(peer.receivedByteCount(), static_cast<quint64>(2 * request.size()));
        QVERIFY(!peer.hasInvalidRequest());

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QJsonObject session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                        QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(session.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!session.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!session.value(QStringLiteral("isBusy")).toBool());
        QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), maxCycles);
        QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), 1);
        QCOMPARE(session.value(QStringLiteral("lastSuccessfulRunId")).toString(), firstRun.runId);
        QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(), secondRun.runId);
        QCOMPARE(session.value(QStringLiteral("lastIterationRunId")).toString(), secondRun.runId);
        const QJsonObject uncertainOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), secondRun.runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_reply")}});
        QCOMPARE(uncertainOutput.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!uncertainOutput.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!uncertainOutput.contains(QStringLiteral("value")));

        QTest::qWait(550);
        for (int i = 0; i < 3; ++i) {
            session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                QJsonObject{{QStringLiteral("runId"), sessionId}});
            QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            const QJsonObject output =
                controller.handleToolCall(QStringLiteral("get_module_output"),
                                          QJsonObject{{QStringLiteral("runId"), secondRun.runId},
                                                      {QStringLiteral("instanceId"), instanceId},
                                                      {QStringLiteral("key"), QStringLiteral("agent_reply")}});
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
        }
        QCOMPARE(peer.connectionCount(), 2);
        QCOMPARE(peer.requests(), (QList<QByteArray>{request, request}));
        QCOMPARE(peer.responses(), (QList<QByteArray>{responseBytes}));
        QCOMPARE(peer.receivedByteCount(), static_cast<quint64>(2 * request.size()));
        QVERIFY(!peer.hasInvalidRequest());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 2);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 2);
        QCOMPARE(runFinished.count(), 2);
        QCOMPARE(cycleStopped.count(), 1);
        QVERIFY(!engine.isBusy());

        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }
};

QTEST_MAIN(TestAgentTCPClientExecutionG5)
#include "test_agent_tcpclient_execution_g5.moc"
