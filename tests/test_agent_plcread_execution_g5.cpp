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
#include <QJsonArray>
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
#include <functional>

using namespace DeepLux;

namespace {

struct ParsedReadRequest {
    quint16 transactionId = 0;
    quint16 protocolId = 0;
    quint16 length = 0;
    quint8 unitId = 0;
    quint8 function = 0;
    quint16 startAddress = 0;
    quint16 quantity = 0;
};

bool parseReadRequest(const QByteArray& frame, ParsedReadRequest& request) {
    if (frame.size() != 12)
        return false;
    QDataStream stream(frame);
    stream.setByteOrder(QDataStream::BigEndian);
    stream >> request.transactionId >> request.protocolId >> request.length >> request.unitId >> request.function >>
        request.startAddress >> request.quantity;
    return stream.status() == QDataStream::Ok && request.protocolId == 0 && request.length == 6 &&
           request.function == 0x03 && request.quantity > 0;
}

QByteArray buildReadResponse(const ParsedReadRequest& request, const QVector<quint16>& registers) {
    QByteArray response;
    QDataStream stream(&response, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << request.transactionId << quint16(0) << static_cast<quint16>(3 + registers.size() * 2) << request.unitId
           << quint8(0x03) << static_cast<quint8>(registers.size() * 2);
    for (quint16 value : registers)
        stream << value;
    return response;
}

class ThreadedModbusReadPeer final {
public:
    explicit ThreadedModbusReadPeer(QVector<quint16> values, int responseLimit = 0)
        : m_values(std::move(values)), m_responseLimit(responseLimit) {}

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
                                ParsedReadRequest parsed;
                                const bool valid = parseReadRequest(frame, parsed);
                                int requestNumber = 0;
                                {
                                    QMutexLocker locker(&m_mutex);
                                    m_requests.append(frame);
                                    m_validRequests.append(valid);
                                    requestNumber = m_requests.size();
                                }
                                if (valid && (m_responseLimit <= 0 || requestNumber <= m_responseLimit)) {
                                    const QByteArray response = buildReadResponse(parsed, m_values);
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
    QVector<quint16> m_values;
    int m_responseLimit = 0;
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

class ModbusReadPeer final : public QObject {
public:
    using ReplyHandler = std::function<QByteArray(const QByteArray&, int)>;

    ModbusReadPeer() {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections()) {
                QTcpSocket* socket = m_server.nextPendingConnection();
                ++m_connectionCount;
                m_buffers.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { receive(socket); });
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

    QByteArray requestAt(int index) const {
        return index >= 0 && index < m_requests.size() ? m_requests.at(index) : QByteArray();
    }

    void setReplyHandler(ReplyHandler handler) {
        m_replyHandler = std::move(handler);
    }

private:
    void receive(QTcpSocket* socket) {
        QByteArray& buffer = m_buffers[socket];
        buffer += socket->readAll();
        while (buffer.size() >= 6) {
            const auto* bytes = reinterpret_cast<const uchar*>(buffer.constData());
            const int frameSize = 6 + qFromBigEndian<quint16>(bytes + 4);
            if (buffer.size() < frameSize)
                return;
            const QByteArray request = buffer.left(frameSize);
            buffer.remove(0, frameSize);
            m_requests.append(request);
            if (m_replyHandler) {
                const QByteArray response = m_replyHandler(request, m_requests.size());
                if (!response.isEmpty()) {
                    socket->write(response);
                    socket->flush();
                }
            }
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QList<QByteArray> m_requests;
    int m_connectionCount = 0;
    ReplyHandler m_replyHandler;
};

} // namespace

class TestAgentPLCReadExecutionG5 : public QObject {
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

    static bool installPLCReadPlugin(const QString& pluginRoot) {
        const QDir pluginDir(QDir(pluginRoot).filePath(QStringLiteral("PLCReadPlugin")));
        if (!QDir().mkpath(pluginDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_PLCREAD_G5_METADATA),
                           pluginDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_PLCREAD_G5_LIBRARY),
                           pluginDir.filePath(QFileInfo(QStringLiteral(AGENT_PLCREAD_G5_LIBRARY)).fileName()));
    }

    static int moduleSignalCount(const QSignalSpy& spy, const QString& instanceId) {
        int count = 0;
        for (const QList<QVariant>& args : spy)
            if (!args.isEmpty() && args.first().toString() == instanceId)
                ++count;
        return count;
    }

    static ModuleInstance plcReadModule(const QString& id, quint16 port, quint8 slaveId, int timeoutMs) {
        ModuleInstance module;
        module.id = id;
        module.moduleId = QStringLiteral("PLC读取");
        module.name = QStringLiteral("Slice 50 loopback PLCRead");
        module.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                    {QStringLiteral("port"), port},
                                    {QStringLiteral("slaveId"), slaveId},
                                    {QStringLiteral("startAddress"), 0x1234},
                                    {QStringLiteral("readCount"), 3},
                                    {QStringLiteral("outputVariable"), QStringLiteral("agent_registers")},
                                    {QStringLiteral("timeout"), timeoutMs}};
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
        QVERIFY(installPLCReadPlugin(pluginRoot));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        QVERIFY(plugins.pluginPaths().contains(QDir(pluginRoot).absolutePath()));
        const QList<PluginInfo> scanned = plugins.moduleInfos();
        QCOMPARE(scanned.size(), 1);
        QCOMPARE(scanned.first().id, QStringLiteral("com.deeplux.plugin.plcread"));
        QCOMPARE(scanned.first().name, QStringLiteral("PLC读取"));
        QVERIFY(plugins.cameraInfos().isEmpty());
        QVERIFY(plugins.loadPlugin(QStringLiteral("PLC读取")));
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
            qCritical() << "PLCRead execution did not become idle; preserving plugin directory" << m_appDataDir.path();
            QFAIL("RunEngine remained busy; temporary PLCRead plugin tree was preserved");
            return;
        }
        engine.clearModules();
        ProjectManager::instance().closeProject();
        AgentController::instance().shutdown();
        PluginManager::instance().shutdown();
    }

    void testStepOnceConfirmsSingleFC03AndReturnsRunBoundRegisters() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const QVector<quint16> registerValues{0x1234, 0xABCD, 42};
        ThreadedModbusReadPeer peer(registerValues);
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice76_plcread_step");
        project->addModule(plcReadModule(instanceId, peer.port(), 7, 1000));
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        const QString targetEffect =
            QStringLiteral("plc_read:%1@127.0.0.1:%2 slave=7 start=4660 count=3 timeout=1000ms")
                .arg(instanceId)
                .arg(peer.port());
        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(peer.responseCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(peer.responseCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
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
                    .contains(targetEffect));
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
        QTRY_COMPARE_WITH_TIMEOUT(peer.connectionCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.requestCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.responseCount(), 1, 5000);
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.requestCount(), 1);
        QCOMPARE(peer.responseCount(), 1);
        QVERIFY(peer.requestValidAt(0));
        ParsedReadRequest request;
        QVERIFY(parseReadRequest(peer.requestAt(0), request));
        QCOMPARE(request.unitId, static_cast<quint8>(7));
        QCOMPARE(request.startAddress, static_cast<quint16>(0x1234));
        QCOMPARE(request.quantity, static_cast<quint16>(3));
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());

        const QJsonObject firstPage = controller.actor()->executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_registers")},
                                                             {QStringLiteral("offset"), 0},
                                                             {QStringLiteral("limit"), 2}});
        QCOMPARE(firstPage.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(firstPage.value(QStringLiteral("value")).toArray(), (QJsonArray{0x1234, 0xABCD}));
        QCOMPARE(firstPage.value(QStringLiteral("totalRows")).toInt(), 3);
        const QJsonObject secondPage = controller.actor()->executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_registers")},
                                                             {QStringLiteral("offset"), 2},
                                                             {QStringLiteral("limit"), 2}});
        QCOMPARE(secondPage.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(secondPage.value(QStringLiteral("value")).toArray(), (QJsonArray{42}));
        const QJsonObject count = controller.actor()->executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId},
                        {QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("key"), QStringLiteral("agent_registers_count")}});
        QCOMPARE(count.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(count.value(QStringLiteral("value")).toInt(), 3);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testResumeFlowConfirmsSingleFC03AndReturnsPausedRunRegisters() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const QVector<quint16> registerValues{0x2345, 0xBCDE, 84};
        ThreadedModbusReadPeer peer(registerValues);
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice76_plcread_resume");
        ModuleInstance module = plcReadModule(instanceId, peer.port(), 7, 1000);
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
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        const QString targetEffect =
            QStringLiteral("plc_read:%1@127.0.0.1:%2 slave=7 start=4660 count=3 timeout=1000ms")
                .arg(instanceId)
                .arg(peer.port());
        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, runId);
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(peer.responseCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.connectionCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.requestCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.responseCount(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 5000);
        QCOMPARE(engine.state(), RunState::Idle);
        QCOMPARE(engine.runObservation().runId, runId);
        QCOMPARE(peer.requestCount(), 1);
        QCOMPARE(peer.responseCount(), 1);
        QVERIFY(peer.requestValidAt(0));
        ParsedReadRequest request;
        QVERIFY(parseReadRequest(peer.requestAt(0), request));
        QCOMPARE(request.unitId, static_cast<quint8>(7));
        QCOMPARE(request.startAddress, static_cast<quint16>(0x1234));
        QCOMPARE(request.quantity, static_cast<quint16>(3));
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());

        const QJsonObject output = controller.actor()->executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_registers")},
                                                             {QStringLiteral("offset"), 0},
                                                             {QStringLiteral("limit"), 3}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(output.value(QStringLiteral("value")).toArray(), (QJsonArray{0x2345, 0xBCDE, 84}));
        QCOMPARE(output.value(QStringLiteral("totalRows")).toInt(), 3);
        const QJsonObject count = controller.actor()->executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId},
                        {QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("key"), QStringLiteral("agent_registers_count")}});
        QCOMPARE(count.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(count.value(QStringLiteral("value")).toInt(), 3);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testConfirmationRejectAndPortSlaveDriftHaveZeroFc03() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ModbusReadPeer oldPeer;
        ModbusReadPeer newPeer;
        QVERIFY(oldPeer.listen());
        QVERIFY(newPeer.listen());
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice50_plcread_target");
        project->addModule(plcReadModule(instanceId, oldPeer.port(), 7, 500));
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        auto startPending = [&controller] {
            return controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        };
        const auto expectedEffects = [instanceId](quint16 port, int slave) {
            return QJsonArray{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                              QStringLiteral("plc_read:%1@127.0.0.1:%2 slave=%3 start=4660 count=3 timeout=500ms")
                                  .arg(instanceId)
                                  .arg(port)
                                  .arg(slave)};
        };

        QJsonObject pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects(oldPeer.port(), 7));
        const QString oldPortConfirmation = controller.pendingConfirmationId();
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("port"), newPeer.port()));
        const int auditCountBeforePortExpiry = audit.count();
        QVERIFY(controller.confirmPendingTools(oldPortConfirmation));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforePortExpiry, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects(newPeer.port(), 7));
        const QString oldSlaveConfirmation = controller.pendingConfirmationId();
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("slaveId"), 8));
        const int auditCountBeforeSlaveExpiry = audit.count();
        QVERIFY(controller.confirmPendingTools(oldSlaveConfirmation));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeSlaveExpiry, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects(newPeer.port(), 8));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(oldPeer.connectionCount(), 0);
        QCOMPARE(oldPeer.requestCount(), 0);
        QCOMPARE(newPeer.connectionCount(), 0);
        QCOMPARE(newPeer.requestCount(), 0);
        QVERIFY(!engine.isBusy());
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testRunFlowReadsRegistersAndTimeoutDoesNotReuseOrRetryOutput() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ThreadedModbusReadPeer peer({0x1234, 0xABCD, 42}, 1);
        QVERIFY(peer.listen());
        const QVector<quint16> registerValues{0x1234, 0xABCD, 42};

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice50_plcread_roundtrip");
        project->addModule(plcReadModule(instanceId, peer.port(), 9, 150));
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
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

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray expectedEffects{
            QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
            QStringLiteral("plc_read:%1@127.0.0.1:%2 slave=9 start=4660 count=3 timeout=150ms")
                .arg(instanceId)
                .arg(peer.port())};
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects);
        const QString firstConfirmationId = controller.pendingConfirmationId();
        QVERIFY(controller.confirmPendingTools(firstConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QString successRunId = toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("runId")).toString();
        QVERIFY(!successRunId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult success = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(success.runId, successRunId);
        QVERIFY2(success.success, qPrintable(success.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(executionThreadId.load(std::memory_order_relaxed) == mainThreadId, false);
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.requestCount(), 1);
        QCOMPARE(peer.responseCount(), 1);
        ParsedReadRequest request;
        QVERIFY(parseReadRequest(peer.requestAt(0), request));
        QCOMPARE(request.unitId, static_cast<quint8>(9));
        QCOMPARE(request.function, static_cast<quint8>(0x03));
        QCOMPARE(request.startAddress, static_cast<quint16>(0x1234));
        QCOMPARE(request.quantity, static_cast<quint16>(3));

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject firstPage = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), successRunId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_registers")},
                                                             {QStringLiteral("offset"), 0},
                                                             {QStringLiteral("limit"), 2}});
        QVERIFY2(firstPage.value(QStringLiteral("status")).toString() == QStringLiteral("available"),
                 qPrintable(QString::fromUtf8(QJsonDocument(firstPage).toJson(QJsonDocument::Compact))));
        QCOMPARE(firstPage.value(QStringLiteral("totalRows")).toInt(), 3);
        QCOMPARE(firstPage.value(QStringLiteral("offset")).toInt(), 0);
        QCOMPARE(firstPage.value(QStringLiteral("limit")).toInt(), 2);
        QCOMPARE(firstPage.value(QStringLiteral("truncated")).toBool(), true);
        QCOMPARE(firstPage.value(QStringLiteral("value")).toArray(), (QJsonArray{0x1234, 0xABCD}));
        const QJsonObject secondPage = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), successRunId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_registers")},
                                                             {QStringLiteral("offset"), 2},
                                                             {QStringLiteral("limit"), 2}});
        QCOMPARE(secondPage.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(secondPage.value(QStringLiteral("totalRows")).toInt(), 3);
        QCOMPARE(secondPage.value(QStringLiteral("offset")).toInt(), 2);
        QCOMPARE(secondPage.value(QStringLiteral("limit")).toInt(), 2);
        QCOMPARE(secondPage.value(QStringLiteral("truncated")).toBool(), false);
        QCOMPARE(secondPage.value(QStringLiteral("value")).toArray(), (QJsonArray{42}));
        const QJsonArray successfulValues{firstPage.value(QStringLiteral("value")).toArray().at(0),
                                          firstPage.value(QStringLiteral("value")).toArray().at(1),
                                          secondPage.value(QStringLiteral("value")).toArray().at(0)};
        const QJsonObject count =
            controller.handleToolCall(QStringLiteral("get_module_output"),
                                      QJsonObject{{QStringLiteral("runId"), successRunId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("agent_registers_count")}});
        QCOMPARE(count.value(QStringLiteral("value")).toInt(), 3);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString timeoutConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!timeoutConfirmationId.isEmpty());
        QVERIFY(timeoutConfirmationId != firstConfirmationId);
        const int toolsBeforeTimeoutRun = toolExecuted.count();
        QVERIFY(controller.confirmPendingTools(timeoutConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), toolsBeforeTimeoutRun + 1, 5000);
        const QString failedRunId = toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("runId")).toString();
        QVERIFY(!failedRunId.isEmpty());
        QVERIFY(failedRunId != successRunId);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(failed.runId, failedRunId);
        QVERIFY(!failed.success);
        QCOMPARE(failed.failedModule, instanceId);
        QVERIFY(failed.errorMessage.contains(QStringLiteral("读取PLC数据超时")));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 2);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 2);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QCOMPARE(peer.connectionCount(), 2);
        QCOMPARE(peer.requestCount(), 2);
        ParsedReadRequest timedOutRequest;
        QVERIFY(parseReadRequest(peer.requestAt(1), timedOutRequest));
        QCOMPARE(timedOutRequest.unitId, static_cast<quint8>(9));
        QCOMPARE(timedOutRequest.function, static_cast<quint8>(0x03));

        const QJsonObject failedStatus = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                                   QJsonObject{{QStringLiteral("runId"), failedRunId}});
        QCOMPARE(failedStatus.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(failedStatus.value(QStringLiteral("lastError"))
                    .toObject()
                    .value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("读取PLC数据超时")));
        const QJsonObject failedOutput =
            controller.handleToolCall(QStringLiteral("get_module_output"),
                                      QJsonObject{{QStringLiteral("runId"), failedRunId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("agent_registers")}});
        QCOMPARE(failedOutput.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(failedOutput.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
        QVERIFY(!failedOutput.contains(QStringLiteral("value")));
        const QJsonObject failedCount =
            controller.handleToolCall(QStringLiteral("get_module_output"),
                                      QJsonObject{{QStringLiteral("runId"), failedRunId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("agent_registers_count")}});
        QCOMPARE(failedCount.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(failedCount.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
        QVERIFY(!failedCount.contains(QStringLiteral("value")));
        QVERIFY(!successfulValues.isEmpty());

        disconnect(threadConnection);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testPLCReadSuccessfulBoundedCycleUsesExactlyThreeFC03Requests() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const QVector<quint16> registerValues{0x1234, 0xABCD, 42};
        ThreadedModbusReadPeer peer(registerValues);
        QVERIFY(peer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice129_plcread_cycle");
        project->addModule(plcReadModule(instanceId, peer.port(), 7, 1000));
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
        const QString expectedEffect =
            QStringLiteral("plc_read:%1@127.0.0.1:%2 slave=7 start=4660 count=3 timeout=1000ms")
                .arg(instanceId)
                .arg(peer.port());

        QJsonObject pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QJsonArray effects = controller.pendingConfirmationPayload()
                                 .value(QStringLiteral("context"))
                                 .toObject()
                                 .value(QStringLiteral("sideEffects"))
                                 .toArray();
        QVERIFY(effects.contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
        QVERIFY(effects.contains(expectedEffect));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(peer.responseCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
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
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
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
        QVERIFY(effects.contains(expectedEffect));
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
        QTRY_COMPARE_WITH_TIMEOUT(peer.responseCount(), 3, 5000);
        QCOMPARE(peer.requestCount(), 3);
        QCOMPARE(peer.responseCount(), 3);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 3);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 3);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        for (int i = 0; i < peer.requestCount(); ++i) {
            QVERIFY(peer.requestValidAt(i));
            ParsedReadRequest request;
            QVERIFY(parseReadRequest(peer.requestAt(i), request));
            QCOMPARE(request.unitId, static_cast<quint8>(7));
            QCOMPARE(request.function, static_cast<quint8>(0x03));
            QCOMPARE(request.startAddress, static_cast<quint16>(0x1234));
            QCOMPARE(request.quantity, static_cast<quint16>(3));
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
        const QJsonObject output = actor->executeTool(QStringLiteral("get_module_output"),
                                                      {{QStringLiteral("runId"), lastRunId},
                                                       {QStringLiteral("instanceId"), instanceId},
                                                       {QStringLiteral("key"), QStringLiteral("agent_registers")},
                                                       {QStringLiteral("offset"), 0},
                                                       {QStringLiteral("limit"), 3}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(output.value(QStringLiteral("value")).toArray(), (QJsonArray{0x1234, 0xABCD, 42}));

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

QTEST_MAIN(TestAgentPLCReadExecutionG5)
#include "test_agent_plcread_execution_g5.moc"
