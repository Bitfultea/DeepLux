#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/common/UserManager.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <QtTest/QtTest>
#include <atomic>
#include <utility>

using namespace DeepLux;

namespace {

struct ModbusRequest {
    quint16 transactionId = 0;
    quint16 protocolId = 0;
    quint16 length = 0;
    quint8 unitId = 0;
    quint8 function = 0;
    quint16 address = 0;
    quint16 count = 0;
};

bool decodeReadRequest(const QByteArray& frame, ModbusRequest& request) {
    if (frame.size() != 12)
        return false;
    QDataStream stream(frame);
    stream.setByteOrder(QDataStream::BigEndian);
    stream >> request.transactionId >> request.protocolId >> request.length >> request.unitId >> request.function >>
        request.address >> request.count;
    return stream.status() == QDataStream::Ok;
}

QByteArray readResponse(const ModbusRequest& request) {
    QByteArray response;
    QDataStream stream(&response, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.transactionId << quint16(0) << quint16(5) << request.unitId << quint8(0x03) << quint8(2)
           << quint16(0x002A);
    return response;
}

class ModbusReadPeer final : public QObject {
public:
    ModbusReadPeer() {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections()) {
                QTcpSocket* socket = m_server.nextPendingConnection();
                ++m_connectionCount;
                m_buffers.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readRequest(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
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

    int requestCount() const {
        return m_requests.size();
    }

    int responseCount() const {
        return m_responseCount;
    }

    QByteArray requestAt(int index) const {
        return index >= 0 && index < m_requests.size() ? m_requests.at(index) : QByteArray();
    }

private:
    void readRequest(QTcpSocket* socket) {
        QByteArray& buffer = m_buffers[socket];
        buffer += socket->readAll();
        while (buffer.size() >= 6) {
            const auto* bytes = reinterpret_cast<const uchar*>(buffer.constData());
            const int frameSize = 6 + qFromBigEndian<quint16>(bytes + 4);
            if (buffer.size() < frameSize)
                return;
            const QByteArray frame = buffer.left(frameSize);
            buffer.remove(0, frameSize);
            m_requests.append(frame);
            ModbusRequest request;
            if (!decodeReadRequest(frame, request))
                continue;
            const QByteArray response = readResponse(request);
            if (socket->write(response) == response.size() && socket->flush())
                ++m_responseCount;
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QList<QByteArray> m_requests;
    int m_connectionCount = 0;
    int m_responseCount = 0;
};

class ThreadedModbusReadPeer final {
public:
    ~ThreadedModbusReadPeer() {
        if (!m_worker || !m_thread.isRunning())
            return;
        m_thread.quit();
        if (!m_thread.wait(3000))
            qFatal("ThreadedModbusReadPeer worker did not stop within 3000 ms");
        m_worker = nullptr;
        m_server = nullptr;
    }

    bool listen() {
        m_worker = new QObject;
        m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
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
                        auto* buffer = new QByteArray;
                        QObject::connect(socket, &QTcpSocket::readyRead, m_worker, [this, socket, buffer] {
                            *buffer += socket->readAll();
                            while (buffer->size() >= 6) {
                                const auto* bytes = reinterpret_cast<const uchar*>(buffer->constData());
                                const int frameSize = 6 + qFromBigEndian<quint16>(bytes + 4);
                                if (buffer->size() < frameSize)
                                    return;
                                const QByteArray frame = buffer->left(frameSize);
                                buffer->remove(0, frameSize);
                                ModbusRequest parsed;
                                const bool valid = decodeReadRequest(frame, parsed) && parsed.protocolId == 0 &&
                                                   parsed.length == 6 && parsed.unitId == 1 &&
                                                   parsed.function == 0x03 && parsed.address == 0 && parsed.count == 1;
                                {
                                    QMutexLocker locker(&m_mutex);
                                    m_requests.append(frame);
                                    m_validRequests.append(valid);
                                }
                                if (valid) {
                                    const QByteArray response = readResponse(parsed);
                                    if (socket->write(response) == response.size()) {
                                        socket->flush();
                                        QMutexLocker locker(&m_mutex);
                                        ++m_responseCount;
                                    }
                                }
                            }
                        });
                        QObject::connect(socket, &QObject::destroyed, m_worker, [buffer] { delete buffer; });
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

    int requestCount() const {
        QMutexLocker locker(&m_mutex);
        return m_requests.size();
    }

    int responseCount() const {
        QMutexLocker locker(&m_mutex);
        return m_responseCount;
    }

    QByteArray requestAt(int index) const {
        QMutexLocker locker(&m_mutex);
        return index >= 0 && index < m_requests.size() ? m_requests.at(index) : QByteArray();
    }

    bool requestValidAt(int index) const {
        QMutexLocker locker(&m_mutex);
        return index >= 0 && index < m_validRequests.size() && m_validRequests.at(index);
    }

private:
    QThread m_thread;
    QObject* m_worker = nullptr;
    QTcpServer* m_server = nullptr;
    mutable QMutex m_mutex;
    quint16 m_port = 0;
    int m_connectionCount = 0;
    int m_responseCount = 0;
    QList<QByteArray> m_requests;
    QList<bool> m_validRequests;
};

} // namespace

class TestAgentPLCCommunicateExecutionG5 : public QObject {
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

    static int moduleSignalCount(const QSignalSpy& spy, const QString& instanceId) {
        int count = 0;
        for (const QList<QVariant>& args : spy)
            if (!args.isEmpty() && args.first().toString() == instanceId)
                ++count;
        return count;
    }

    static bool installPlugin(const QString& pluginRoot) {
        const QString pluginDir = QDir(pluginRoot).filePath(QStringLiteral("PLCCommunicatePlugin"));
        if (!QDir().mkpath(pluginDir))
            return false;
        return QFile::copy(QStringLiteral(AGENT_PLCCOMMUNICATE_G5_METADATA),
                           QDir(pluginDir).filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(
                   QStringLiteral(AGENT_PLCCOMMUNICATE_G5_LIBRARY),
                   QDir(pluginDir).filePath(QFileInfo(QStringLiteral(AGENT_PLCCOMMUNICATE_G5_LIBRARY)).fileName()));
    }

    static ModuleInstance plcCommunicateModule(const QString& instanceId, quint16 port) {
        ModuleInstance module;
        module.id = instanceId;
        module.moduleId = QStringLiteral("PLC通信测试");
        module.name = QStringLiteral("Slice 54 loopback PLC communication check");
        module.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                    {QStringLiteral("port"), port},
                                    {QStringLiteral("timeout"), 1000}};
        return module;
    }

    void resetAgentFlow() {
        RunEngine& engine = RunEngine::instance();
        engine.stop();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        AgentController::instance().clearConversation();
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
    }

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins"));
        QVERIFY(installPlugin(pluginRoot));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        QVERIFY(plugins.pluginPaths().contains(QDir(pluginRoot).absolutePath()));
        const QList<PluginInfo> modules = plugins.moduleInfos();
        QCOMPARE(modules.size(), 1);
        QCOMPARE(modules.first().id, QStringLiteral("com.deeplux.plugin.plccommunicate"));
        QCOMPARE(modules.first().name, QStringLiteral("PLC通信测试"));
        QVERIFY(plugins.cameraInfos().isEmpty());
        QVERIFY(plugins.loadPlugin(QStringLiteral("PLC通信测试")));
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
            qCritical() << "PLCCommunicate execution did not become idle; preserving plugin directory"
                        << m_appDataDir.path();
            QFAIL("RunEngine remained busy; temporary PLCCommunicate plugin tree was preserved");
            return;
        }
        engine.clearModules();
        ProjectManager::instance().closeProject();
        AgentController::instance().shutdown();
        PluginManager::instance().shutdown();
    }

    void testStepOnceExpiresDriftRejectsAndConfirmsSingleFC03() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ThreadedModbusReadPeer oldPeer;
        ThreadedModbusReadPeer newPeer;
        QVERIFY(oldPeer.listen());
        QVERIFY(newPeer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice77_plc_communicate_step");
        project->addModule(plcCommunicateModule(instanceId, oldPeer.port()));
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        auto expectedEffects = [instanceId](quint16 port) {
            return QJsonArray{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                              QStringLiteral("plc_communicate:%1@127.0.0.1:%2 function=03 address=0 count=1 "
                                             "timeout=1000ms")
                                  .arg(instanceId)
                                  .arg(port)};
        };
        auto startPending = [&controller] { return controller.handleToolCall(QStringLiteral("step_once"), {}); };

        QJsonObject pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray observedEffects = controller.pendingConfirmationPayload()
                                               .value(QStringLiteral("context"))
                                               .toObject()
                                               .value(QStringLiteral("sideEffects"))
                                               .toArray();
        QVERIFY2(observedEffects == expectedEffects(oldPeer.port()),
                 qPrintable(
                     QString::fromUtf8(QJsonDocument(observedEffects).toJson(QJsonDocument::Compact)) +
                     QStringLiteral(" != ") +
                     QString::fromUtf8(QJsonDocument(expectedEffects(oldPeer.port())).toJson(QJsonDocument::Compact))));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("port"), newPeer.port()));
        const int auditCountBeforeStaleConfirmation = audit.count();
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeStaleConfirmation, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(oldPeer.requestCount(), 0);
        QCOMPARE(oldPeer.responseCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 0);
        QCOMPARE(newPeer.requestCount(), 0);
        QCOMPARE(newPeer.responseCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects(newPeer.port()));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(rejectedId != staleId);
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(oldPeer.requestCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 0);
        QCOMPARE(newPeer.requestCount(), 0);
        QCOMPARE(newPeer.responseCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects(newPeer.port()));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject stepResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = stepResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(newPeer.requestCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(newPeer.responseCount(), 1, 5000);
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(oldPeer.requestCount(), 0);
        QCOMPARE(oldPeer.responseCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 1);
        QCOMPARE(newPeer.requestCount(), 1);
        QCOMPARE(newPeer.responseCount(), 1);
        QVERIFY(newPeer.requestValidAt(0));
        ModbusRequest request;
        QVERIFY(decodeReadRequest(newPeer.requestAt(0), request));
        QCOMPARE(request.transactionId, quint16(1));
        QCOMPARE(request.protocolId, quint16(0));
        QCOMPARE(request.length, quint16(6));
        QCOMPARE(request.unitId, quint8(1));
        QCOMPARE(request.function, quint8(0x03));
        QCOMPARE(request.address, quint16(0));
        QCOMPARE(request.count, quint16(1));
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());

        const QJsonObject connected = controller.actor()->executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("plc_connected")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(connected.value(QStringLiteral("value")).toBool(), true);
        const QJsonObject responseTime = controller.actor()->executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId},
                        {QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("key"), QStringLiteral("plc_response_time_ms")}});
        QCOMPARE(responseTime.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(responseTime.value(QStringLiteral("value")).toInt() >= 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testResumeFlowExpiresDriftRejectsAndConfirmsSingleFC03() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ThreadedModbusReadPeer oldPeer;
        ThreadedModbusReadPeer newPeer;
        QVERIFY(oldPeer.listen());
        QVERIFY(newPeer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice77_plc_communicate_resume");
        ModuleInstance module = plcCommunicateModule(instanceId, oldPeer.port());
        module.breakpoint = true;
        project->addModule(module);
        QVERIFY(engine.loadProject(project));
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        auto expectedEffects = [instanceId](quint16 port) {
            return QJsonArray{QStringLiteral("execution:resume_paused_session_may_touch_files_devices_network"),
                              QStringLiteral("plc_communicate:%1@127.0.0.1:%2 function=03 address=0 count=1 "
                                             "timeout=1000ms")
                                  .arg(instanceId)
                                  .arg(port)};
        };
        auto startPending = [&controller] { return controller.handleToolCall(QStringLiteral("resume_flow"), {}); };
        QJsonObject pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray observedResumeEffects = controller.pendingConfirmationPayload()
                                                     .value(QStringLiteral("context"))
                                                     .toObject()
                                                     .value(QStringLiteral("sideEffects"))
                                                     .toArray();
        QVERIFY2(observedResumeEffects == expectedEffects(oldPeer.port()),
                 qPrintable(
                     QString::fromUtf8(QJsonDocument(observedResumeEffects).toJson(QJsonDocument::Compact)) +
                     QStringLiteral(" != ") +
                     QString::fromUtf8(QJsonDocument(expectedEffects(oldPeer.port())).toJson(QJsonDocument::Compact))));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("port"), newPeer.port()));
        const int auditCountBeforeStaleConfirmation = audit.count();
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeStaleConfirmation, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, runId);
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(oldPeer.requestCount(), 0);
        QCOMPARE(oldPeer.responseCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 0);
        QCOMPARE(newPeer.requestCount(), 0);
        QCOMPARE(newPeer.responseCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("port"), oldPeer.port()));

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects(oldPeer.port()));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(rejectedId != staleId);
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, runId);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(oldPeer.requestCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 0);
        QCOMPARE(newPeer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects(oldPeer.port()));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(oldPeer.requestCount(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 5000);
        QCOMPARE(engine.state(), RunState::Idle);
        QCOMPARE(engine.runObservation().runId, runId);
        QCOMPARE(oldPeer.connectionCount(), 1);
        QCOMPARE(oldPeer.requestCount(), 1);
        QCOMPARE(oldPeer.responseCount(), 1);
        QCOMPARE(newPeer.connectionCount(), 0);
        QCOMPARE(newPeer.requestCount(), 0);
        QCOMPARE(newPeer.responseCount(), 0);
        QVERIFY(oldPeer.requestValidAt(0));
        ModbusRequest request;
        QVERIFY(decodeReadRequest(oldPeer.requestAt(0), request));
        QCOMPARE(request.transactionId, quint16(1));
        QCOMPARE(request.unitId, quint8(1));
        QCOMPARE(request.function, quint8(0x03));
        QCOMPARE(request.address, quint16(0));
        QCOMPARE(request.count, quint16(1));
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());

        const QJsonObject connected = controller.actor()->executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("plc_connected")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(connected.value(QStringLiteral("value")).toBool(), true);
        const QJsonObject responseTime = controller.actor()->executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId},
                        {QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("key"), QStringLiteral("plc_response_time_ms")}});
        QCOMPARE(responseTime.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(responseTime.value(QStringLiteral("value")).toInt() >= 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentConfirmationRejectsAndExpiresOldPLCCommunicateTarget() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ThreadedModbusReadPeer oldPeer;
        ThreadedModbusReadPeer newPeer;
        QVERIFY(oldPeer.listen());
        QVERIFY(newPeer.listen());
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice54_plc_communicate_target");
        project->addModule(plcCommunicateModule(instanceId, oldPeer.port()));
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        std::atomic<quintptr> executionThreadId{0};
        const QMetaObject::Connection threadConnection = connect(
            &engine, &RunEngine::moduleStarted, &engine,
            [&executionThreadId, instanceId](const QString& startedId) {
                if (startedId == instanceId)
                    executionThreadId.store(reinterpret_cast<quintptr>(QThread::currentThreadId()),
                                            std::memory_order_relaxed);
            },
            Qt::DirectConnection);
        const quintptr mainThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());

        auto startPending = [&controller] {
            return controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        };
        auto expectedEffects = [instanceId](quint16 port) {
            return QJsonArray{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                              QStringLiteral("plc_communicate:%1@127.0.0.1:%2 function=03 address=0 count=1 "
                                             "timeout=1000ms")
                                  .arg(instanceId)
                                  .arg(port)};
        };

        QJsonObject pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects(oldPeer.port()));
        QVERIFY(!QString::fromUtf8(QJsonDocument(controller.pendingConfirmationPayload()).toJson())
                     .contains(QStringLiteral("plc_response")));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(oldPeer.requestCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 0);
        QCOMPARE(newPeer.requestCount(), 0);
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleConfirmationId = controller.pendingConfirmationId();
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("port"), newPeer.port()));
        const int auditBaseline = audit.count();
        QVERIFY(controller.confirmPendingTools(staleConfirmationId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditBaseline, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(oldPeer.requestCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 0);
        QCOMPARE(newPeer.requestCount(), 0);
        QCOMPARE(moduleStarted.count(), 0);

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects(newPeer.port()));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QString runId = toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(newPeer.responseCount(), 1, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(QStringLiteral("%1 (old=%2/%3, new=%4/%5)")
                                                .arg(result.errorMessage)
                                                .arg(oldPeer.requestCount())
                                                .arg(oldPeer.responseCount())
                                                .arg(newPeer.requestCount())
                                                .arg(newPeer.responseCount())));
        QCOMPARE(moduleStarted.count(), 1);
        QCOMPARE(moduleFinished.count(), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(executionThreadId.load(std::memory_order_relaxed) != mainThreadId);
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(oldPeer.requestCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 1);
        QCOMPARE(newPeer.requestCount(), 1);
        QVERIFY(newPeer.requestValidAt(0));
        QCOMPARE(newPeer.responseCount(), 1);
        ModbusRequest request;
        QVERIFY(decodeReadRequest(newPeer.requestAt(0), request));
        QCOMPARE(request.protocolId, quint16(0));
        QCOMPARE(request.length, quint16(6));
        QCOMPARE(request.unitId, quint8(1));
        QCOMPARE(request.function, quint8(0x03));
        QCOMPARE(request.address, quint16(0));
        QCOMPARE(request.count, quint16(1));

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject connected = controller.actor()->executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("plc_connected")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(connected.value(QStringLiteral("value")).toBool(), true);
        const QJsonObject responseTime = controller.actor()->executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId},
                        {QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("key"), QStringLiteral("plc_response_time_ms")}});
        QCOMPARE(responseTime.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(responseTime.value(QStringLiteral("value")).toInt() >= 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        disconnect(threadConnection);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testPLCCommunicateSuccessfulBoundedCycleUsesExactlyThreeFC03Requests() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ThreadedModbusReadPeer peer;
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice129_plc_communicate_cycle");
        project->addModule(plcCommunicateModule(instanceId, peer.port()));
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        auto requestCycle = [&controller] {
            return controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        };
        auto expectedEffect = [instanceId](quint16 port) {
            return QStringLiteral("plc_communicate:%1@127.0.0.1:%2 function=03 address=0 count=1 timeout=1000ms")
                .arg(instanceId)
                .arg(port);
        };

        QJsonObject pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QJsonArray effects = controller.pendingConfirmationPayload()
                                 .value(QStringLiteral("context"))
                                 .toObject()
                                 .value(QStringLiteral("sideEffects"))
                                 .toArray();
        QVERIFY(effects.contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
        QVERIFY(effects.contains(expectedEffect(peer.port())));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(peer.responseCount(), 0);
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(!controller.hasPendingTools());
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(peer.responseCount(), 0);
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QVERIFY(!engine.isBusy());

        pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        effects = controller.pendingConfirmationPayload()
                      .value(QStringLiteral("context"))
                      .toObject()
                      .value(QStringLiteral("sideEffects"))
                      .toArray();
        QVERIFY(effects.contains(expectedEffect(peer.port())));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);

        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        QString sessionId;
        QJsonObject terminalSession;
        QObject signalContext;
        connect(actor, &AgentActor::toolExecuted, &signalContext,
                [&sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                });
        connect(
            &engine, &RunEngine::cycleStopped, &signalContext,
            [&]() {
                terminalSession =
                    actor->executeTool(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), sessionId}});
            },
            Qt::DirectConnection);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QVERIFY(!sessionId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.requestCount(), 3, 5000);
        QCOMPARE(peer.responseCount(), 3);
        QCOMPARE(peer.requestCount(), 3);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 3);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 3);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        for (int i = 0; i < peer.requestCount(); ++i) {
            ModbusRequest request;
            QVERIFY(decodeReadRequest(peer.requestAt(i), request));
            QCOMPARE(request.protocolId, quint16(0));
            QCOMPARE(request.length, quint16(6));
            QCOMPARE(request.unitId, quint8(1));
            QCOMPARE(request.function, quint8(0x03));
            QCOMPARE(request.address, quint16(0));
            QCOMPARE(request.count, quint16(1));
        }
        for (const QList<QVariant>& args : moduleFinished) {
            QCOMPARE(args.at(0).toString(), instanceId);
            QVERIFY(args.at(1).toBool());
        }
        for (const QList<QVariant>& args : runFinished) {
            const RunResult result = qvariant_cast<RunResult>(args.at(0));
            QVERIFY2(result.success, qPrintable(result.errorMessage));
        }
        QCOMPARE(terminalSession.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(terminalSession.value(QStringLiteral("isBusy")).toBool(), false);
        QCOMPARE(terminalSession.value(QStringLiteral("maxCycles")).toInt(), 3);
        QCOMPARE(terminalSession.value(QStringLiteral("completedCycles")).toInt(), 3);
        const QString lastRunId = terminalSession.value(QStringLiteral("lastCompletedRunId")).toString();
        QVERIFY(!lastRunId.isEmpty());
        QCOMPARE(terminalSession.value(QStringLiteral("lastSuccessfulRunId")).toString(), lastRunId);
        QCOMPARE(qvariant_cast<RunResult>(runFinished.last().at(0)).runId, lastRunId);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject connected = actor->executeTool(QStringLiteral("get_module_output"),
                                                         {{QStringLiteral("runId"), lastRunId},
                                                          {QStringLiteral("instanceId"), instanceId},
                                                          {QStringLiteral("key"), QStringLiteral("plc_connected")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(connected.value(QStringLiteral("value")).toBool(), true);
        QCOMPARE(audit.count(), 2);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        QTest::qWait(350);
        const QJsonObject quietSession =
            actor->executeTool(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), sessionId}});
        QCOMPARE(quietSession.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(quietSession.value(QStringLiteral("isBusy")).toBool(), false);
        QCOMPARE(quietSession.value(QStringLiteral("maxCycles")).toInt(), 3);
        QCOMPARE(quietSession.value(QStringLiteral("completedCycles")).toInt(), 3);
        QCOMPARE(quietSession.value(QStringLiteral("lastCompletedRunId")).toString(), lastRunId);
        QCOMPARE(quietSession.value(QStringLiteral("lastSuccessfulRunId")).toString(), lastRunId);
        QCOMPARE(peer.requestCount(), 3);
        QCOMPARE(peer.responseCount(), 3);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 3);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 3);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(audit.count(), 2);
        QCOMPARE(engine.isBusy(), false);
        int runFlowCalls = 0;
        int runResultsCalls = 0;
        int outputQueries = 0;
        for (const QList<QVariant>& args : toolExecuted) {
            const QString name = args.at(0).toString();
            runFlowCalls += name == QLatin1String("run_flow") ? 1 : 0;
            runResultsCalls += name == QLatin1String("get_run_results") ? 1 : 0;
            outputQueries += name == QLatin1String("get_module_output") ? 1 : 0;
        }
        QCOMPARE(runFlowCalls, 1);
        QCOMPARE(runResultsCalls, 2);
        QCOMPARE(outputQueries, 1);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }
};

QTEST_MAIN(TestAgentPLCCommunicateExecutionG5)
#include "test_agent_plccommunicate_execution_g5.moc"
