#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/common/UserManager.h"
#include "core/deeplux/DataContract.h"
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

using namespace DeepLux;

class ModbusReadSourcePeer final {
public:
    ModbusReadSourcePeer() {
        m_worker = new QObject;
        m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
    }

    ~ModbusReadSourcePeer() {
        if (m_worker && m_thread.isRunning()) {
            QMetaObject::invokeMethod(
                m_worker,
                [this] {
                    if (m_server) {
                        m_server->close();
                        delete m_server;
                        m_server = nullptr;
                    }
                    m_thread.quit();
                },
                Qt::BlockingQueuedConnection);
            if (!m_thread.wait(3000))
                qFatal("Modbus read source peer worker did not stop within 3000 ms");
        }
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
                        auto* buffer = new QByteArray;
                        QObject::connect(socket, &QTcpSocket::readyRead, m_worker, [this, socket, buffer] {
                            *buffer += socket->readAll();
                            if (buffer->size() < 12)
                                return;
                            const QByteArray request = buffer->left(12);
                            buffer->remove(0, 12);
                            const auto* bytes = reinterpret_cast<const uchar*>(request.constData());
                            const quint16 start = qFromBigEndian<quint16>(bytes + 8);
                            const quint16 value = start == 256 ? 0x1234 : (start == 257 ? 0xBEEF : 0);
                            QByteArray response = request.left(7);
                            qToBigEndian<quint16>(5, reinterpret_cast<uchar*>(response.data() + 4));
                            response.append(char(0x03));
                            response.append(char(0x02));
                            char valueBytes[2];
                            qToBigEndian<quint16>(value, reinterpret_cast<uchar*>(valueBytes));
                            response.append(valueBytes, 2);
                            if (socket->write(response) == response.size()) {
                                socket->flush();
                                QMutexLocker locker(&m_mutex);
                                m_requests.append(request);
                                ++m_responseCount;
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

    int requestCount() const {
        QMutexLocker locker(&m_mutex);
        return m_requests.size();
    }

    int responseCount() const {
        QMutexLocker locker(&m_mutex);
        return m_responseCount;
    }

    int connectionCount() const {
        QMutexLocker locker(&m_mutex);
        return m_connectionCount;
    }

    QList<QByteArray> requests() const {
        QMutexLocker locker(&m_mutex);
        return m_requests;
    }

private:
    QThread m_thread;
    QObject* m_worker = nullptr;
    QTcpServer* m_server = nullptr;
    mutable QMutex m_mutex;
    quint16 m_port = 0;
    QList<QByteArray> m_requests;
    int m_responseCount = 0;
    int m_connectionCount = 0;
};

class ModbusWritePeer final : public QObject {
public:
    ModbusWritePeer() {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections()) {
                QTcpSocket* socket = m_server.nextPendingConnection();
                ++m_connectionCount;
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

    void setReplyEnabled(bool enabled) {
        m_replyEnabled = enabled;
    }

    int requestCount() const {
        return m_requests.size();
    }

    int connectionCount() const {
        return m_connectionCount;
    }

    QList<QByteArray> requests() const {
        return m_requests;
    }

    QList<QByteArray> responses() const {
        return m_responses;
    }

    qsizetype rawByteCount() const {
        return m_rawByteCount;
    }

    bool waitForQuietWindow(int quietMs = 250, int timeoutMs = 3000) const {
        QElapsedTimer deadline;
        deadline.start();
        QElapsedTimer quiet;
        int previousConnections = -1;
        int previousRequests = -1;
        qsizetype previousRawBytes = -1;
        while (deadline.elapsed() < timeoutMs) {
            const int connections = connectionCount();
            const int requests = requestCount();
            const qsizetype rawBytes = rawByteCount();
            if (requests > 0) {
                if (!quiet.isValid() || connections != previousConnections || requests != previousRequests ||
                    rawBytes != previousRawBytes)
                    quiet.start();
                if (quiet.elapsed() >= quietMs) {
                    const QList<QByteArray> parsedRequests = this->requests();
                    return connections == 1 && requests == 1 && parsedRequests.size() == 1 &&
                           rawBytes == parsedRequests.first().size();
                }
            } else {
                quiet.invalidate();
            }
            previousConnections = connections;
            previousRequests = requests;
            previousRawBytes = rawBytes;
            QCoreApplication::processEvents();
            QTest::qWait(10);
        }
        return false;
    }

    bool waitForQuietCounts(int expectedConnections, int expectedRequests, int quietMs = 500,
                            int timeoutMs = 5000) const {
        QElapsedTimer deadline;
        deadline.start();
        QElapsedTimer quiet;
        int previousConnections = -1;
        int previousRequests = -1;
        qsizetype previousRawBytes = -1;
        while (deadline.elapsed() < timeoutMs) {
            const int connections = connectionCount();
            const int requests = requestCount();
            const qsizetype rawBytes = rawByteCount();
            if (connections == expectedConnections && requests == expectedRequests) {
                if (!quiet.isValid() || connections != previousConnections || requests != previousRequests ||
                    rawBytes != previousRawBytes)
                    quiet.start();
                if (quiet.elapsed() >= quietMs) {
                    qsizetype parsedBytes = 0;
                    for (const QByteArray& request : this->requests())
                        parsedBytes += request.size();
                    return rawBytes == parsedBytes;
                }
            } else {
                quiet.invalidate();
            }
            previousConnections = connections;
            previousRequests = requests;
            previousRawBytes = rawBytes;
            QCoreApplication::processEvents();
            QTest::qWait(10);
        }
        return false;
    }

    void close() {
        m_server.close();
    }

private:
    void receive(QTcpSocket* socket) {
        QByteArray& buffer = m_buffers[socket];
        const QByteArray received = socket->readAll();
        m_rawByteCount += received.size();
        buffer += received;
        while (buffer.size() >= 6) {
            const auto* bytes = reinterpret_cast<const uchar*>(buffer.constData());
            const quint16 mbapLength = qFromBigEndian<quint16>(bytes + 4);
            if (mbapLength < 2 || mbapLength > 260)
                return;
            const int frameSize = 6 + mbapLength;
            if (buffer.size() < frameSize)
                return;

            const QByteArray request = buffer.left(frameSize);
            buffer.remove(0, frameSize);
            m_requests.append(request);
            if (m_replyEnabled && request.size() >= 12 && static_cast<quint8>(request.at(7)) == 0x10) {
                QByteArray echo = request.left(12);
                echo[4] = 0;
                echo[5] = 6;
                m_responses.append(echo);
                socket->write(echo);
                socket->flush();
            }
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QList<QByteArray> m_requests;
    QList<QByteArray> m_responses;
    bool m_replyEnabled = true;
    int m_connectionCount = 0;
    qsizetype m_rawByteCount = 0;
};

class TestAgentPLCWriteExecutionG5 : public QObject {
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

    static bool installPLCWritePlugin(const QString& pluginRoot) {
        const QDir pluginDir(QDir(pluginRoot).filePath(QStringLiteral("PLCWrite")));
        if (!QDir().mkpath(pluginDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_PLCWRITE_G5_METADATA),
                           pluginDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_PLCWRITE_G5_LIBRARY),
                           pluginDir.filePath(QFileInfo(QStringLiteral(AGENT_PLCWRITE_G5_LIBRARY)).fileName()));
    }

    static bool installPLCReadPlugin(const QString& pluginRoot) {
        const QDir pluginDir(QDir(pluginRoot).filePath(QStringLiteral("PLCReadPlugin")));
        if (!QDir().mkpath(pluginDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_PLCREAD_G5_METADATA),
                           pluginDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_PLCREAD_G5_LIBRARY),
                           pluginDir.filePath(QFileInfo(QStringLiteral(AGENT_PLCREAD_G5_LIBRARY)).fileName()));
    }

    static bool installVariableArraySourcePlugin(const QString& pluginRoot) {
        const QDir pluginDir(QDir(pluginRoot).filePath(QStringLiteral("VariableArraySourceFixture")));
        if (!QDir().mkpath(pluginDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_VARIABLE_ARRAY_SOURCE_G5_METADATA),
                           pluginDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(
                   QStringLiteral(AGENT_VARIABLE_ARRAY_SOURCE_G5_LIBRARY),
                   pluginDir.filePath(QFileInfo(QStringLiteral(AGENT_VARIABLE_ARRAY_SOURCE_G5_LIBRARY)).fileName()));
    }

    static int moduleSignalCount(const QSignalSpy& spy, const QString& instanceId) {
        int count = 0;
        for (const QList<QVariant>& args : spy)
            if (!args.isEmpty() && args.first().toString() == instanceId)
                ++count;
        return count;
    }

    static QJsonObject moduleOutput(AgentController& controller, const QString& runId, const QString& instanceId,
                                    const QString& key) {
        return controller.actor()->executeTool(QStringLiteral("get_module_output"),
                                               QJsonObject{{QStringLiteral("runId"), runId},
                                                           {QStringLiteral("instanceId"), instanceId},
                                                           {QStringLiteral("key"), key}});
    }

    static QByteArray confirmationDisclosureBytes(const QJsonObject& payload) {
        const QJsonObject context = payload.value(QStringLiteral("context")).toObject();
        return QJsonDocument(QJsonObject{{QStringLiteral("tools"), payload.value(QStringLiteral("tools"))},
                                         {QStringLiteral("summary"), context.value(QStringLiteral("summary"))},
                                         {QStringLiteral("sideEffects"), context.value(QStringLiteral("sideEffects"))}})
            .toJson(QJsonDocument::Compact);
    }

    static bool isWriteRequest(const QByteArray& frame, quint8 unitId, quint16 address, quint16 value) {
        if (frame.size() != 15 || static_cast<quint8>(frame.at(2)) != 0 || static_cast<quint8>(frame.at(3)) != 0 ||
            static_cast<quint8>(frame.at(7)) != 0x10 || static_cast<quint8>(frame.at(6)) != unitId ||
            qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(frame.constData()) + 4) != 9 ||
            qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(frame.constData()) + 8) != address ||
            qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(frame.constData()) + 10) != 1 ||
            static_cast<quint8>(frame.at(12)) != 2 ||
            qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(frame.constData()) + 13) != value)
            return false;
        return true;
    }

    static bool isWriteEcho(const QByteArray& response, const QByteArray& request, quint8 unitId, quint16 address) {
        return response.size() == 12 && request.size() == 15 && response.left(2) == request.left(2) &&
               static_cast<quint8>(response.at(2)) == 0 && static_cast<quint8>(response.at(3)) == 0 &&
               qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(response.constData()) + 4) == 6 &&
               static_cast<quint8>(response.at(6)) == unitId && static_cast<quint8>(response.at(7)) == 0x10 &&
               qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(response.constData()) + 8) == address &&
               qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(response.constData()) + 10) == 1;
    }

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins"));
        QVERIFY(installPLCWritePlugin(pluginRoot));
        QVERIFY(installPLCReadPlugin(pluginRoot));
        QVERIFY(installVariableArraySourcePlugin(pluginRoot));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        QVERIFY(plugins.pluginPaths().contains(QDir(pluginRoot).absolutePath()));
        const QList<PluginInfo> scanned = plugins.moduleInfos();
        QCOMPARE(scanned.size(), 3);
        QSet<QString> pluginIds;
        for (const PluginInfo& info : scanned)
            pluginIds.insert(info.id);
        QCOMPARE(pluginIds, (QSet<QString>{QStringLiteral("com.deeplux.plugin.plcwrite"),
                                           QStringLiteral("com.deeplux.plugin.plcread"),
                                           QStringLiteral("com.deeplux.test.agent_variable_array_source")}));
        QVERIFY(plugins.cameraInfos().isEmpty());
        QVERIFY(plugins.loadPlugin(QStringLiteral("PLC写入")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("PLC读取")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("VariableArraySourceFixture")));
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
            qCritical() << "PLCWrite execution did not become idle; preserving plugin directory" << m_appDataDir.path();
            QFAIL("RunEngine remained busy; temporary PLCWrite plugin tree was preserved");
            return;
        }
        engine.clearModules();
        ProjectManager::instance().closeProject();
        AgentController::instance().shutdown();
        PluginManager::instance().shutdown();
    }

    void testPLCWriteRequiresConfirmationForRunFlowAndStepOnce() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        ModbusWritePeer peer;
        QVERIFY(peer.listen());
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice32_plcwrite_target");
        ModuleInstance plc;
        plc.id = instanceId;
        plc.moduleId = QStringLiteral("PLC写入");
        plc.name = QStringLiteral("Slice 32 PLCWrite target");
        plc.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                 {QStringLiteral("port"), peer.port()},
                                 {QStringLiteral("slaveId"), 7},
                                 {QStringLiteral("startAddress"), 4660},
                                 {QStringLiteral("values"), QJsonArray{0x1234}},
                                 {QStringLiteral("valueSource"), QStringLiteral("immediate")},
                                 {QStringLiteral("timeout"), 1000}};
        project->addModule(plc);

        const QJsonArray outputPorts = controller.actor()
                                           ->executeTool(QStringLiteral("get_module_params_schema"),
                                                         QJsonObject{{QStringLiteral("instanceId"), instanceId}})
                                           .value(QStringLiteral("ports"))
                                           .toObject()
                                           .value(QStringLiteral("outputs"))
                                           .toArray();
        const auto writeCountPort = std::find_if(outputPorts.cbegin(), outputPorts.cend(), [](const QJsonValue& port) {
            return port.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("plc_write_count");
        });
        QVERIFY(writeCountPort != outputPorts.cend());
        QCOMPARE(writeCountPort->toObject().value(QStringLiteral("type")).toString(), QStringLiteral("Integer"));
        QVERIFY(!writeCountPort->toObject().value(QStringLiteral("required")).toBool());
        QVERIFY(std::none_of(outputPorts.cbegin(), outputPorts.cend(), [](const QJsonValue& port) {
            return port.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("plc_write_success");
        }));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QVERIFY(!engine.isBusy());

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString runConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!runConfirmationId.isEmpty());
        QCOMPARE(peer.requestCount(), 0);
        QVERIFY(controller.confirmPendingTools(runConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject runStarted = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(runStarted.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = runStarted.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult runResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(runResult.runId, runId);
        QVERIFY2(runResult.success, qPrintable(runResult.errorMessage));
        QCOMPARE(peer.requestCount(), 1);
        QVERIFY(isWriteRequest(peer.requests().last(), 7, 4660, 0x1234));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!engine.isBusy());

        const QJsonObject runWriteSuccess =
            moduleOutput(controller, runId, instanceId, QStringLiteral("plc_write_success"));
        QCOMPARE(runWriteSuccess.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(runWriteSuccess.value(QStringLiteral("value")).toBool(), true);
        const QJsonObject runWriteCount =
            moduleOutput(controller, runId, instanceId, QStringLiteral("plc_write_count"));
        QCOMPARE(runWriteCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(runWriteCount.value(QStringLiteral("value")).toInt(), 1);
        const QJsonObject runWriteCountPort =
            controller.actor()->executeTool(QStringLiteral("get_module_output"),
                                            QJsonObject{{QStringLiteral("runId"), runId},
                                                        {QStringLiteral("instanceId"), instanceId},
                                                        {QStringLiteral("port"), QStringLiteral("plc_write_count")}});
        QCOMPARE(runWriteCountPort.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(runWriteCountPort.value(QStringLiteral("source")).toString(), QStringLiteral("port"));
        QCOMPARE(runWriteCountPort.value(QStringLiteral("value")).toInt(),
                 runWriteCount.value(QStringLiteral("value")).toInt());
        QCOMPARE(peer.requestCount(), 1);

        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("startAddress"), 22136));
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("values"), QJsonArray{0x4321}));
        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(peer.requestCount(), 1);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(peer.requestCount(), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(peer.requestCount(), 1);
        const QString stepConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!stepConfirmationId.isEmpty());
        const int toolsBeforeStepExecution = toolExecuted.count();
        QVERIFY(controller.confirmPendingTools(stepConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), toolsBeforeStepExecution + 1, 5000);
        const QJsonObject stepResultObject = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepResultObject.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString stepRunId = stepResultObject.value(QStringLiteral("runId")).toString();
        QVERIFY(!stepRunId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult stepResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(stepResult.runId, stepRunId);
        QVERIFY2(stepResult.success, qPrintable(stepResult.errorMessage));
        QCOMPARE(peer.requestCount(), 2);
        QVERIFY(isWriteRequest(peer.requests().last(), 7, 22136, 0x4321));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 2);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 2);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!engine.isBusy());

        const QJsonObject stepWriteSuccess =
            moduleOutput(controller, stepRunId, instanceId, QStringLiteral("plc_write_success"));
        QCOMPARE(stepWriteSuccess.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(stepWriteSuccess.value(QStringLiteral("value")).toBool(), true);
        const QJsonObject stepWriteCount =
            moduleOutput(controller, stepRunId, instanceId, QStringLiteral("plc_write_count"));
        QCOMPARE(stepWriteCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(stepWriteCount.value(QStringLiteral("value")).toInt(), 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
        peer.close();
    }

    void testImmediatePLCWritePreviewShowsInclusiveRegisterRange() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        ModbusWritePeer peer;
        QVERIFY(peer.listen());
        struct Case {
            int start;
            QJsonArray values;
            QString valueSource;
            int expectedEnd;
            bool valid;
        };
        const QList<Case> cases{{65535, QJsonArray{0x1234}, QStringLiteral("immediate"), 65535, true},
                                {65534, QJsonArray{0x1234, 0xBEEF}, QStringLiteral("immediate"), 65535, true},
                                {65535, QJsonArray{0x1234, 0xBEEF}, QStringLiteral("immediate"), 65536, false},
                                {65536, QJsonArray{0x1234}, QStringLiteral("immediate"), 65536, false},
                                {256, QJsonArray{0x1234}, QStringLiteral("future"), 256, false}};

        for (int i = 0; i < cases.size(); ++i) {
            const Case& current = cases.at(i);
            Project* project = projects.newProject();
            QVERIFY(project);
            const QString instanceId = QStringLiteral("slice123_boundary_%1").arg(i);
            ModuleInstance writer;
            writer.id = instanceId;
            writer.moduleId = QStringLiteral("PLC写入");
            writer.name = QStringLiteral("Slice 123 register boundary");
            writer.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                        {QStringLiteral("port"), peer.port()},
                                        {QStringLiteral("slaveId"), 7},
                                        {QStringLiteral("startAddress"), current.start},
                                        {QStringLiteral("values"), current.values},
                                        {QStringLiteral("valueSource"), current.valueSource},
                                        {QStringLiteral("timeout"), 1000}};
            project->addModule(writer);

            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            const int connectionsBefore = peer.connectionCount();
            const int requestsBefore = peer.requestCount();
            QJsonObject pending = controller.handleToolCall(
                QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const int quantity = current.values.size();
            const QString rangeDetails = current.valueSource == QLatin1String("immediate")
                                             ? QStringLiteral("quantity=%1 end=%2%3 valueSource=immediate")
                                                   .arg(quantity)
                                                   .arg(current.expectedEnd)
                                                   .arg(current.valid ? QString() : QStringLiteral(" range=invalid"))
                                             : QStringLiteral("quantity=<runtime> end=<runtime> "
                                                              "range=unresolved-at-confirmation valueSource=unknown:%1")
                                                   .arg(current.valueSource);
            const QString expectedEffect =
                QStringLiteral("plc_write:%1@127.0.0.1:%2 slave=7 address=%3 %4")
                    .arg(instanceId)
                    .arg(peer.port())
                    .arg(current.start >= 0 && current.start <= 65535 ? QString::number(current.start)
                                                                      : QStringLiteral("<invalid>"),
                         rangeDetails);
            const QJsonObject payload = controller.pendingConfirmationPayload();
            const QJsonArray effects =
                payload.value(QStringLiteral("context")).toObject().value(QStringLiteral("sideEffects")).toArray();
            QVERIFY(effects.contains(expectedEffect));
            const QByteArray compactPayload = confirmationDisclosureBytes(payload);
            QVERIFY(!compactPayload.contains("4660"));
            QVERIFY(!compactPayload.contains("48879"));
            QCOMPARE(peer.requestCount(), requestsBefore);
            QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);

            if (!current.valid) {
                QVERIFY(!qvariant_cast<RunResult>(runFinished.last().at(0)).success);
                QCOMPARE(peer.connectionCount(), connectionsBefore);
                QCOMPARE(peer.requestCount(), requestsBefore);
            } else {
                QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
                QCOMPARE(peer.requestCount(), requestsBefore + 1);
                const QByteArray request = peer.requests().last();
                QCOMPARE(static_cast<quint8>(request.at(7)), quint8(0x10));
                QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(request.constData()) + 8),
                         current.start);
                QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(request.constData()) + 10),
                         quint16(quantity));
                QCOMPARE(static_cast<quint8>(request.at(12)), quint8(quantity * 2));
                QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(request.constData()) + 13),
                         quint16(0x1234));
                if (quantity == 2)
                    QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(request.constData()) + 15),
                             quint16(0xBEEF));
            }

            engine.stop();
            engine.clearModules();
            projects.closeProject();
        }
        peer.close();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testPLCWriteConfirmationExpiresOnConfigDriftAndUncertainRunIsNotRetried() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        ModbusWritePeer originalPeer;
        ModbusWritePeer noEchoPeer;
        QVERIFY(originalPeer.listen());
        QVERIFY(noEchoPeer.listen());
        noEchoPeer.setReplyEnabled(false);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice33_plcwrite_target");
        ModuleInstance plc;
        plc.id = instanceId;
        plc.moduleId = QStringLiteral("PLC写入");
        plc.name = QStringLiteral("Slice 33 uncertain PLCWrite target");
        plc.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                 {QStringLiteral("port"), originalPeer.port()},
                                 {QStringLiteral("slaveId"), 11},
                                 {QStringLiteral("startAddress"), 8192},
                                 {QStringLiteral("values"), QJsonArray{0x2468}},
                                 {QStringLiteral("valueSource"), QStringLiteral("immediate")},
                                 {QStringLiteral("timeout"), 150}};
        project->addModule(plc);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy response(&controller, &AgentController::llmResponseReceived);

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const RunState stateBeforeStaleApproval = engine.state();
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("port"), noEchoPeer.port()));
        const int auditCountBeforeStaleConfirmation = audit.count();
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeStaleConfirmation, 5000);
        QCOMPARE(engine.state(), stateBeforeStaleApproval);
        QVERIFY(!engine.isBusy());
        QCOMPARE(originalPeer.requestCount(), 0);
        QCOMPARE(noEchoPeer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QVERIFY(!audit.isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(!response.isEmpty());
        QVERIFY(response.last().at(0).toString().contains(QStringLiteral("确认已失效")));

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(started.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(failed.runId, runId);
        QVERIFY(!failed.success);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(failed.failedModule, instanceId);
        QCOMPARE(noEchoPeer.requestCount(), 1);
        QVERIFY(isWriteRequest(noEchoPeer.requests().last(), 11, 8192, 0x2468));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QVERIFY(!engine.isBusy());

        const QJsonObject observed = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                                     QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(observed.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(observed.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(observed.contains(QStringLiteral("retryable")));
        QVERIFY(observed.value(QStringLiteral("retryable")).isBool());
        QVERIFY(!observed.value(QStringLiteral("retryable")).toBool());
        QCOMPARE(noEchoPeer.requestCount(), 1);
        const QJsonObject uncertainSuccess =
            moduleOutput(controller, runId, instanceId, QStringLiteral("plc_write_success"));
        const QJsonObject uncertainCount =
            moduleOutput(controller, runId, instanceId, QStringLiteral("plc_write_count"));
        for (const QJsonObject& output : {uncertainSuccess, uncertainCount}) {
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QVERIFY(output.contains(QStringLiteral("retryable")));
            QVERIFY(output.value(QStringLiteral("retryable")).isBool());
            QCOMPARE(output.value(QStringLiteral("retryable")).toBool(), false);
            QVERIFY(!output.contains(QStringLiteral("value")));
        }
        const QJsonObject uncertainCountPort =
            controller.actor()->executeTool(QStringLiteral("get_module_output"),
                                            QJsonObject{{QStringLiteral("runId"), runId},
                                                        {QStringLiteral("instanceId"), instanceId},
                                                        {QStringLiteral("port"), QStringLiteral("plc_write_count")}});
        QCOMPARE(uncertainCountPort.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(uncertainCountPort.contains(QStringLiteral("retryable")));
        QVERIFY(uncertainCountPort.value(QStringLiteral("retryable")).isBool());
        QCOMPARE(uncertainCountPort.value(QStringLiteral("retryable")).toBool(), false);
        QVERIFY(!uncertainCountPort.contains(QStringLiteral("value")));
        QCOMPARE(noEchoPeer.requestCount(), 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
        originalPeer.close();
        noEchoPeer.close();
    }

    void testPLCWriteRequiresConfirmationForResumeFlow() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        ModbusWritePeer peer;
        QVERIFY(peer.listen());
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice35_plcwrite_resume_target");
        ModuleInstance plc;
        plc.id = instanceId;
        plc.moduleId = QStringLiteral("PLC写入");
        plc.name = QStringLiteral("Slice 35 PLCWrite resume target");
        plc.breakpoint = true;
        plc.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                 {QStringLiteral("port"), peer.port()},
                                 {QStringLiteral("slaveId"), 7},
                                 {QStringLiteral("startAddress"), 4660},
                                 {QStringLiteral("values"), QJsonArray{0x1234}},
                                 {QStringLiteral("valueSource"), QStringLiteral("immediate")},
                                 {QStringLiteral("timeout"), 1000}};
        project->addModule(plc);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);

        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString rejectedConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!rejectedConfirmationId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedConfirmationId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!approvedConfirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(peer.requestCount(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 5000);
        QCOMPARE(engine.state(), RunState::Idle);
        QVERIFY(isWriteRequest(peer.requests().last(), 7, 4660, 0x1234));
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(!audit.isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        const QJsonObject resumeWriteSuccess =
            moduleOutput(controller, runId, instanceId, QStringLiteral("plc_write_success"));
        QCOMPARE(resumeWriteSuccess.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(resumeWriteSuccess.value(QStringLiteral("value")).toBool(), true);
        const QJsonObject resumeWriteCount =
            moduleOutput(controller, runId, instanceId, QStringLiteral("plc_write_count"));
        QCOMPARE(resumeWriteCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(resumeWriteCount.value(QStringLiteral("value")).toInt(), 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
        peer.close();
    }

    void testAutopilotRunFlowConfirmsPLCReadVariableSourceAndWritesRuntimePayloadOnce() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        ModbusReadSourcePeer readPeer;
        ModbusWritePeer writePeer;
        QVERIFY(readPeer.listen());
        QVERIFY(writePeer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString readInstanceId = QStringLiteral("slice68_plcread_source");
        const QString writeInstanceId = QStringLiteral("slice68_plcwrite_target");
        ModuleInstance read;
        read.id = readInstanceId;
        read.moduleId = QStringLiteral("PLC读取");
        read.name = QStringLiteral("Slice 68 deterministic PLCRead source");
        read.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                  {QStringLiteral("port"), readPeer.port()},
                                  {QStringLiteral("slaveId"), 9},
                                  {QStringLiteral("startAddress"), 256},
                                  {QStringLiteral("readCount"), 1},
                                  {QStringLiteral("outputVariable"), QStringLiteral("agent_registers")},
                                  {QStringLiteral("timeout"), 1000}};
        project->addModule(read);

        ModuleInstance write;
        write.id = writeInstanceId;
        write.moduleId = QStringLiteral("PLC写入");
        write.name = QStringLiteral("Slice 68 PLCWrite variable consumer");
        write.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                   {QStringLiteral("port"), writePeer.port()},
                                   {QStringLiteral("slaveId"), 7},
                                   {QStringLiteral("startAddress"), 8192},
                                   {QStringLiteral("values"), QStringLiteral("agent_registers")},
                                   {QStringLiteral("valueSource"), QStringLiteral("variable")},
                                   {QStringLiteral("timeout"), 1000}};
        project->addModule(write);

        ModuleConnection connection;
        connection.fromModuleId = readInstanceId;
        connection.toModuleId = writeInstanceId;
        connection.fromPort = QStringLiteral("image");
        connection.toPort = QStringLiteral("image");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        const auto requestRun = [&controller] {
            return controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        };
        const auto expectPendingDisclosure = [&controller, &readPeer, &writePeer, &writeInstanceId](int sourceAddress) {
            const QJsonObject payload = controller.pendingConfirmationPayload();
            const QJsonArray effects =
                payload.value(QStringLiteral("context")).toObject().value(QStringLiteral("sideEffects")).toArray();
            const QStringList effectTexts = [&effects] {
                QStringList values;
                for (const QJsonValue& effect : effects)
                    values.append(effect.toString());
                return values;
            }();
            const QString readEffect =
                QStringLiteral("plc_read:%1@127.0.0.1:%2 slave=9 start=%3 count=1 timeout=1000ms")
                    .arg(QStringLiteral("slice68_plcread_source"), QString::number(readPeer.port()),
                         QString::number(sourceAddress));
            const QString writeEffect =
                QStringLiteral("plc_write:%1@127.0.0.1:%2 slave=7 address=8192 "
                               "quantity=<runtime> end=<runtime> "
                               "range=unresolved-at-confirmation valueSource=variable:agent_registers")
                    .arg(writeInstanceId, QString::number(writePeer.port()));
            const QByteArray compactPayload = confirmationDisclosureBytes(payload);
            return effectTexts.contains(readEffect) && effectTexts.contains(writeEffect) &&
                   !compactPayload.contains("4660") && !compactPayload.contains("48879") &&
                   !compactPayload.contains("0x1234") && !compactPayload.contains("0xBEEF");
        };

        QJsonObject pending = requestRun();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectPendingDisclosure(256));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(readPeer.connectionCount(), 0);
        QCOMPARE(readPeer.requestCount(), 0);
        QCOMPARE(writePeer.connectionCount(), 0);
        QCOMPARE(writePeer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, readInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, readInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, writeInstanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QVERIFY(!engine.isBusy());

        pending = requestRun();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString expiredId = controller.pendingConfirmationId();
        QVERIFY(!expiredId.isEmpty());
        QVERIFY(expectPendingDisclosure(256));
        QVERIFY(project->setModuleParam(readInstanceId, QStringLiteral("startAddress"), 257));
        const int auditCountBeforeExpiry = audit.count();
        QVERIFY(controller.confirmPendingTools(expiredId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeExpiry, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(readPeer.connectionCount(), 0);
        QCOMPARE(readPeer.requestCount(), 0);
        QCOMPARE(writePeer.connectionCount(), 0);
        QCOMPARE(writePeer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, readInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, readInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, writeInstanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QVERIFY(!engine.isBusy());

        pending = requestRun();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(expectPendingDisclosure(257));
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject toolResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(toolResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = toolResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success,
                 qPrintable(QStringLiteral(
                                "%1; read connections=%2 requests=%3 responses=%4; write connections=%5 requests=%6")
                                .arg(result.errorMessage)
                                .arg(readPeer.connectionCount())
                                .arg(readPeer.requestCount())
                                .arg(readPeer.responseCount())
                                .arg(writePeer.connectionCount())
                                .arg(writePeer.requestCount())));
        QCOMPARE(readPeer.connectionCount(), 1);
        QCOMPARE(readPeer.requestCount(), 1);
        QCOMPARE(writePeer.connectionCount(), 1);
        QCOMPARE(writePeer.requestCount(), 1);

        const QByteArray readRequest = readPeer.requests().last();
        QCOMPARE(readRequest.size(), 12);
        QCOMPARE(static_cast<quint8>(readRequest.at(6)), static_cast<quint8>(9));
        QCOMPARE(static_cast<quint8>(readRequest.at(7)), static_cast<quint8>(0x03));
        QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(readRequest.constData()) + 8),
                 static_cast<quint16>(257));
        QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(readRequest.constData()) + 10),
                 static_cast<quint16>(1));
        QVERIFY(isWriteRequest(writePeer.requests().last(), 7, 8192, 0xBEEF));
        QCOMPARE(moduleSignalCount(moduleStarted, readInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, readInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, writeInstanceId), 1);
        QVERIFY(moduleFinished.at(0).at(1).toBool());
        QVERIFY(moduleFinished.at(1).at(1).toBool());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAutopilotStepOnceRetainsDeterministicArrayForPLCWrite() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        ModbusWritePeer writePeer;
        QVERIFY(writePeer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString sourceInstanceId = QStringLiteral("slice69_array_source");
        const QString writeInstanceId = QStringLiteral("slice69_plcwrite_target");
        ModuleInstance source;
        source.id = sourceInstanceId;
        source.moduleId = QStringLiteral("VariableArraySourceFixture");
        source.name = QStringLiteral("Slice 69 deterministic array source");
        source.params = QJsonObject{{QStringLiteral("outputVariable"), QStringLiteral("agent_registers")},
                                    {QStringLiteral("values"), QJsonArray{0x1234}}};
        project->addModule(source);

        ModuleInstance write;
        write.id = writeInstanceId;
        write.moduleId = QStringLiteral("PLC写入");
        write.name = QStringLiteral("Slice 69 PLCWrite variable consumer");
        write.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                   {QStringLiteral("port"), writePeer.port()},
                                   {QStringLiteral("slaveId"), 7},
                                   {QStringLiteral("startAddress"), 8192},
                                   {QStringLiteral("values"), QStringLiteral("agent_registers")},
                                   {QStringLiteral("valueSource"), QStringLiteral("variable")},
                                   {QStringLiteral("timeout"), 1000}};
        project->addModule(write);

        ModuleConnection connection;
        connection.fromModuleId = sourceInstanceId;
        connection.toModuleId = writeInstanceId;
        connection.fromPort = QStringLiteral("image");
        connection.toPort = QStringLiteral("image");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        const auto requestStep = [&controller] { return controller.handleToolCall(QStringLiteral("step_once"), {}); };
        const auto expectPendingDisclosure = [&controller, &writePeer, &writeInstanceId] {
            const QJsonObject payload = controller.pendingConfirmationPayload();
            const QJsonArray effects =
                payload.value(QStringLiteral("context")).toObject().value(QStringLiteral("sideEffects")).toArray();
            const QStringList effectTexts = [&effects] {
                QStringList values;
                for (const QJsonValue& effect : effects)
                    values.append(effect.toString());
                return values;
            }();
            const QString writeEffect =
                QStringLiteral("plc_write:%1@127.0.0.1:%2 slave=7 address=8192 "
                               "quantity=<runtime> end=<runtime> "
                               "range=unresolved-at-confirmation valueSource=variable:agent_registers")
                    .arg(writeInstanceId, QString::number(writePeer.port()));
            const QByteArray compactPayload = confirmationDisclosureBytes(payload);
            return effectTexts.contains(writeEffect) && !compactPayload.contains("4660") &&
                   !compactPayload.contains("0x1234");
        };

        QJsonObject pending = requestStep();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectPendingDisclosure());
        const QString firstRejectedId = controller.pendingConfirmationId();
        QVERIFY(!firstRejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(firstRejectedId));
        QCOMPARE(writePeer.connectionCount(), 0);
        QCOMPARE(writePeer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, sourceInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, sourceInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, writeInstanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QVERIFY(!engine.isBusy());

        pending = requestStep();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectPendingDisclosure());
        const QString firstApprovedId = controller.pendingConfirmationId();
        QVERIFY(!firstApprovedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(firstApprovedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject firstToolResult = toolExecuted.last().at(1).toJsonObject();
        QVERIFY2(firstToolResult.value(QStringLiteral("status")).toString() == QStringLiteral("started"),
                 qPrintable(QString::fromUtf8(QJsonDocument(firstToolResult).toJson(QJsonDocument::Compact))));
        const QString runId = firstToolResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult firstStep = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(firstStep.runId, runId);
        QVERIFY2(firstStep.success, qPrintable(firstStep.errorMessage));
        QCOMPARE(writePeer.connectionCount(), 0);
        QCOMPARE(writePeer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, writeInstanceId), 0);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(!engine.isBusy());

        pending = requestStep();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectPendingDisclosure());
        const QString secondRejectedId = controller.pendingConfirmationId();
        QVERIFY(!secondRejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(secondRejectedId));
        QCOMPARE(writePeer.connectionCount(), 0);
        QCOMPARE(writePeer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, writeInstanceId), 0);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(toolExecuted.count(), 1);
        QVERIFY(!engine.isBusy());

        pending = requestStep();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectPendingDisclosure());
        const QString secondApprovedId = controller.pendingConfirmationId();
        QVERIFY(!secondApprovedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(secondApprovedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 2, 5000);
        const QJsonObject secondToolResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(secondToolResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        QCOMPARE(secondToolResult.value(QStringLiteral("runId")).toString(), runId);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult secondStep = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(secondStep.runId, runId);
        QVERIFY2(secondStep.success, qPrintable(secondStep.errorMessage));
        QCOMPARE(writePeer.connectionCount(), 1);
        QCOMPARE(writePeer.requestCount(), 1);
        QVERIFY(isWriteRequest(writePeer.requests().last(), 7, 8192, 0x1234));
        QCOMPARE(moduleSignalCount(moduleStarted, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, writeInstanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAutopilotResumeFlowRetainsDeterministicArrayForPLCWrite() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        ModbusWritePeer writePeer;
        QVERIFY(writePeer.listen());

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString sourceInstanceId = QStringLiteral("slice70_array_source");
        const QString writeInstanceId = QStringLiteral("slice70_plcwrite_target");
        ModuleInstance source;
        source.id = sourceInstanceId;
        source.moduleId = QStringLiteral("VariableArraySourceFixture");
        source.name = QStringLiteral("Slice 70 deterministic array source");
        source.params = QJsonObject{{QStringLiteral("outputVariable"), QStringLiteral("agent_registers")},
                                    {QStringLiteral("values"), QJsonArray{0x1234}}};
        project->addModule(source);

        ModuleInstance write;
        write.id = writeInstanceId;
        write.moduleId = QStringLiteral("PLC写入");
        write.name = QStringLiteral("Slice 70 PLCWrite variable consumer");
        write.breakpoint = true;
        write.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                   {QStringLiteral("port"), writePeer.port()},
                                   {QStringLiteral("slaveId"), 7},
                                   {QStringLiteral("startAddress"), 8192},
                                   {QStringLiteral("values"), QStringLiteral("agent_registers")},
                                   {QStringLiteral("valueSource"), QStringLiteral("variable")},
                                   {QStringLiteral("timeout"), 1000}};
        project->addModule(write);

        ModuleConnection connection;
        connection.fromModuleId = sourceInstanceId;
        connection.toModuleId = writeInstanceId;
        connection.fromPort = QStringLiteral("image");
        connection.toPort = QStringLiteral("image");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        engine.runOnce();
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Paused, 5000);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QCOMPARE(writePeer.connectionCount(), 0);
        QCOMPARE(writePeer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, writeInstanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        const auto requestResume = [&controller] {
            return controller.handleToolCall(QStringLiteral("resume_flow"), {});
        };
        const auto expectPendingDisclosure = [&controller, &writePeer, &writeInstanceId] {
            const QJsonObject payload = controller.pendingConfirmationPayload();
            const QJsonArray effects =
                payload.value(QStringLiteral("context")).toObject().value(QStringLiteral("sideEffects")).toArray();
            const QStringList effectTexts = [&effects] {
                QStringList values;
                for (const QJsonValue& effect : effects)
                    values.append(effect.toString());
                return values;
            }();
            const QString writeEffect =
                QStringLiteral("plc_write:%1@127.0.0.1:%2 slave=7 address=8192 "
                               "quantity=<runtime> end=<runtime> "
                               "range=unresolved-at-confirmation valueSource=variable:agent_registers")
                    .arg(writeInstanceId, QString::number(writePeer.port()));
            const QByteArray compactPayload = confirmationDisclosureBytes(payload);
            return effectTexts.contains(writeEffect) && !compactPayload.contains("4660") &&
                   !compactPayload.contains("0x1234");
        };

        QJsonObject pending = requestResume();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectPendingDisclosure());
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, runId);
        QCOMPARE(writePeer.connectionCount(), 0);
        QCOMPARE(writePeer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        pending = requestResume();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectPendingDisclosure());
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject toolResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(toolResult.value(QStringLiteral("status")).toString(), QStringLiteral("resumed"));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(writePeer.connectionCount(), 1);
        QCOMPARE(writePeer.requestCount(), 1);
        QVERIFY(isWriteRequest(writePeer.requests().last(), 7, 8192, 0x1234));
        QCOMPARE(moduleSignalCount(moduleStarted, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, sourceInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, writeInstanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, writeInstanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QCOMPARE(engine.state(), RunState::Idle);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testStepOncePLCWriteTimeoutIsUncertainAndDoesNotRetry() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        constexpr int timeoutMs = 150;
        ModbusWritePeer peer;
        QVERIFY(peer.listen());
        peer.setReplyEnabled(false);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice82_plcwrite_step_uncertain");
        constexpr quint8 slaveId = 11;
        constexpr quint16 address = 8192;
        constexpr quint16 value = 0x2468;
        ModuleInstance write;
        write.id = instanceId;
        write.moduleId = QStringLiteral("PLC写入");
        write.name = QStringLiteral("Slice 82 PLCWrite step uncertain FC16");
        write.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                   {QStringLiteral("port"), peer.port()},
                                   {QStringLiteral("slaveId"), slaveId},
                                   {QStringLiteral("startAddress"), address},
                                   {QStringLiteral("values"), QJsonArray{value}},
                                   {QStringLiteral("valueSource"), QStringLiteral("immediate")},
                                   {QStringLiteral("timeout"), timeoutMs}};
        project->addModule(write);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString expectedEffect = QStringLiteral("plc_write:%1@127.0.0.1:%2 slave=%3 address=%4 "
                                                      "quantity=1 end=%4 valueSource=immediate")
                                           .arg(instanceId)
                                           .arg(peer.port())
                                           .arg(slaveId)
                                           .arg(address);
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(expectedEffect));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
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
        QVERIFY(peer.waitForQuietWindow());
        QVERIFY(!engine.isBusy());
        QCOMPARE(failed.runId, runId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(failed.failedModule, instanceId);
        QVERIFY(failed.errorMessage.contains(QStringLiteral("PLC无响应数据")) ||
                failed.errorMessage.contains(QStringLiteral("写入PLC数据超时")));
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.requestCount(), 1);
        QVERIFY(isWriteRequest(peer.requests().last(), slaveId, address, value));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(!moduleFinished.last().at(1).toBool());

        const QJsonObject result = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                                   QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!result.value(QStringLiteral("retryable")).toBool());
        for (const QString& key : {QStringLiteral("plc_write_success"), QStringLiteral("plc_write_count")}) {
            const QJsonObject output = moduleOutput(controller, runId, instanceId, key);
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!output.contains(QStringLiteral("value")));
        }
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.requestCount(), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
        peer.close();
    }

    void testBoundedCyclePLCWriteUncertainStopsWithoutReplay() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        constexpr int timeoutMs = 350;
        constexpr quint8 slaveId = 12;
        constexpr quint16 address = 9216;
        constexpr quint16 value = 0x5A3C;
        ModbusWritePeer peer;
        QVERIFY(peer.listen());
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice92_plcwrite_cycle_uncertain");
        ModuleInstance write;
        write.id = instanceId;
        write.moduleId = QStringLiteral("PLC写入");
        write.name = QStringLiteral("Slice 92 bounded PLCWrite uncertainty");
        write.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                   {QStringLiteral("port"), peer.port()},
                                   {QStringLiteral("slaveId"), slaveId},
                                   {QStringLiteral("startAddress"), address},
                                   {QStringLiteral("values"), QJsonArray{value}},
                                   {QStringLiteral("valueSource"), QStringLiteral("immediate")},
                                   {QStringLiteral("timeout"), timeoutMs}};
        project->addModule(write);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QString sessionId;
        QObject signalContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &signalContext,
                [&sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                });

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray effects = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray();
        QVERIFY(effects.contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
        QVERIFY(effects.contains(QStringLiteral("plc_write:%1@127.0.0.1:%2 slave=%3 address=%4 "
                                                "quantity=1 end=%4 valueSource=immediate")
                                     .arg(instanceId)
                                     .arg(peer.port())
                                     .arg(slaveId)
                                     .arg(address)));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QVERIFY(!sessionId.isEmpty());
        QCOMPARE(toolExecuted.last().at(0).toString(), QStringLiteral("run_flow"));
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("started"));

        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult firstRun = qvariant_cast<RunResult>(runFinished.at(0).at(0));
        QVERIFY(firstRun.success);
        QCOMPARE(peer.requestCount(), 1);
        QVERIFY(isWriteRequest(peer.requests().first(), slaveId, address, value));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.first().at(1).toBool());

        peer.setReplyEnabled(false);
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult secondRun = qvariant_cast<RunResult>(runFinished.at(1).at(0));
        QCOMPARE(secondRun.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(secondRun.failedModule, instanceId);
        QVERIFY(!secondRun.runId.isEmpty());
        QVERIFY(!secondRun.success);
        QVERIFY(isWriteRequest(peer.requests().last(), slaveId, address, value));
        QCOMPARE(peer.requestCount(), 2);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 2);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 2);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QCOMPARE(cycleStopped.count(), 1);

        QJsonObject session = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                              QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("runId")).toString(), sessionId);
        QCOMPARE(session.value(QStringLiteral("sessionId")).toString(), sessionId);
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!session.value(QStringLiteral("isBusy")).toBool());
        QCOMPARE(session.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!session.value(QStringLiteral("retryable")).toBool());
        QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), 3);
        QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), 1);
        QCOMPARE(session.value(QStringLiteral("lastSuccessfulRunId")).toString(), firstRun.runId);
        QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(), secondRun.runId);
        QCOMPARE(session.value(QStringLiteral("lastIterationRunId")).toString(), secondRun.runId);
        const QJsonObject lastError = session.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(lastError.value(QStringLiteral("code")).toInt(), static_cast<int>(ExecError::SideEffectUncertain));
        QCOMPARE(lastError.value(QStringLiteral("moduleId")).toString(), instanceId);

        for (const QString& key : {QStringLiteral("plc_write_success"), QStringLiteral("plc_write_count")}) {
            const QJsonObject output = moduleOutput(controller, secondRun.runId, instanceId, key);
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QCOMPARE(output.value(QStringLiteral("reason")).toString(), QStringLiteral("side_effect_outcome_unknown"));
            QCOMPARE(output.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
            QVERIFY(output.value(QStringLiteral("lastError")).isObject());
            QVERIFY(!output.contains(QStringLiteral("value")));
        }
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(peer.waitForQuietCounts(2, 2));
        QTest::qWait(550);
        session = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                  QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        for (const QString& key : {QStringLiteral("plc_write_success"), QStringLiteral("plc_write_count")}) {
            const QJsonObject output = moduleOutput(controller, secondRun.runId, instanceId, key);
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        }
        QCOMPARE(peer.connectionCount(), 2);
        QCOMPARE(peer.requestCount(), 2);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 2);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 2);
        QCOMPARE(cycleStopped.count(), 1);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testBoundedCyclePLCWriteCompletesExactWriteCount() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        constexpr int timeoutMs = 1000;
        constexpr quint8 slaveId = 13;
        constexpr quint16 address = 9728;
        constexpr quint16 value = 0x6B4D;
        constexpr int maxCycles = 3;
        ModbusWritePeer peer;
        QVERIFY(peer.listen());
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice94_plcwrite_cycle_success");
        ModuleInstance write;
        write.id = instanceId;
        write.moduleId = QStringLiteral("PLC写入");
        write.name = QStringLiteral("Slice 94 bounded PLCWrite exact count");
        write.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                   {QStringLiteral("port"), peer.port()},
                                   {QStringLiteral("slaveId"), slaveId},
                                   {QStringLiteral("startAddress"), address},
                                   {QStringLiteral("values"), QJsonArray{value}},
                                   {QStringLiteral("valueSource"), QStringLiteral("immediate")},
                                   {QStringLiteral("timeout"), timeoutMs}};
        project->addModule(write);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QString sessionId;
        QObject signalContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &signalContext,
                [&sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                });

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), maxCycles}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray effects = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray();
        QVERIFY(effects.contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
        QVERIFY(effects.contains(QStringLiteral("plc_write:%1@127.0.0.1:%2 slave=%3 address=%4 "
                                                "quantity=1 end=%4 valueSource=immediate")
                                     .arg(instanceId)
                                     .arg(peer.port())
                                     .arg(slaveId)
                                     .arg(address)));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(peer.responses().size(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), maxCycles}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QCOMPARE(toolExecuted.last().at(0).toString(), QStringLiteral("run_flow"));
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("started"));
        sessionId = toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("sessionId")).toString();
        QVERIFY(!sessionId.isEmpty());

        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QCOMPARE(runFinished.count(), maxCycles);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), maxCycles);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), maxCycles);
        QCOMPARE(peer.requestCount(), maxCycles);
        QCOMPARE(peer.connectionCount(), maxCycles);
        QCOMPARE(peer.responses().size(), maxCycles);
        const QList<QByteArray> requests = peer.requests();
        const QList<QByteArray> responses = peer.responses();
        for (int i = 0; i < maxCycles; ++i) {
            const RunResult run = qvariant_cast<RunResult>(runFinished.at(i).at(0));
            QVERIFY2(run.success, qPrintable(run.errorMessage));
            QCOMPARE(moduleFinished.at(i).at(0).toString(), instanceId);
            QVERIFY(moduleFinished.at(i).at(1).toBool());
            QVERIFY(isWriteRequest(requests.at(i), slaveId, address, value));
            QVERIFY(isWriteEcho(responses.at(i), requests.at(i), slaveId, address));
        }
        QCOMPARE(cycleStopped.count(), 1);

        QJsonObject session = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                              QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("runId")).toString(), sessionId);
        QCOMPARE(session.value(QStringLiteral("sessionId")).toString(), sessionId);
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QVERIFY(!session.value(QStringLiteral("isBusy")).toBool());
        QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), maxCycles);
        QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), maxCycles);
        QCOMPARE(session.value(QStringLiteral("lastSuccessfulRunId")).toString(),
                 qvariant_cast<RunResult>(runFinished.last().at(0)).runId);
        QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(),
                 qvariant_cast<RunResult>(runFinished.last().at(0)).runId);
        const QString lastRunId = session.value(QStringLiteral("lastCompletedRunId")).toString();
        const QJsonObject success =
            moduleOutput(controller, lastRunId, instanceId, QStringLiteral("plc_write_success"));
        QCOMPARE(success.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(success.value(QStringLiteral("value")).toBool(), true);
        const QJsonObject count = moduleOutput(controller, lastRunId, instanceId, QStringLiteral("plc_write_count"));
        QCOMPARE(count.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(count.value(QStringLiteral("value")).toInt(), 1);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        QVERIFY(peer.waitForQuietCounts(maxCycles, maxCycles));
        QTest::qWait(550);
        session = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                  QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(moduleOutput(controller, lastRunId, instanceId, QStringLiteral("plc_write_success"))
                     .value(QStringLiteral("value"))
                     .toBool(),
                 true);
        QCOMPARE(peer.connectionCount(), maxCycles);
        QCOMPARE(peer.requestCount(), maxCycles);
        QCOMPARE(peer.responses().size(), maxCycles);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), maxCycles);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), maxCycles);
        QCOMPARE(cycleStopped.count(), 1);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testResumeFlowPLCWriteTimeoutIsUncertainAndDoesNotRetry() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        constexpr int timeoutMs = 150;
        ModbusWritePeer peer;
        QVERIFY(peer.listen());
        peer.setReplyEnabled(false);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice82_plcwrite_resume_uncertain");
        constexpr quint8 slaveId = 11;
        constexpr quint16 address = 8192;
        constexpr quint16 value = 0x2468;
        ModuleInstance write;
        write.id = instanceId;
        write.moduleId = QStringLiteral("PLC写入");
        write.name = QStringLiteral("Slice 82 PLCWrite resume uncertain FC16");
        write.breakpoint = true;
        write.params = QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                   {QStringLiteral("port"), peer.port()},
                                   {QStringLiteral("slaveId"), slaveId},
                                   {QStringLiteral("startAddress"), address},
                                   {QStringLiteral("values"), QJsonArray{value}},
                                   {QStringLiteral("valueSource"), QStringLiteral("immediate")},
                                   {QStringLiteral("timeout"), timeoutMs}};
        project->addModule(write);
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
        QCOMPARE(peer.requestCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString expectedEffect = QStringLiteral("plc_write:%1@127.0.0.1:%2 slave=%3 address=%4 "
                                                      "quantity=1 end=%4 valueSource=immediate")
                                           .arg(instanceId)
                                           .arg(peer.port())
                                           .arg(slaveId)
                                           .arg(address);
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(expectedEffect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, pausedRunId);
        QCOMPARE(peer.connectionCount(), 0);
        QCOMPARE(peer.requestCount(), 0);
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
        QVERIFY(peer.waitForQuietWindow());
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Idle, 5000);
        QVERIFY(!engine.isBusy());
        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(failed.runId, pausedRunId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(failed.failedModule, instanceId);
        QVERIFY(failed.errorMessage.contains(QStringLiteral("PLC无响应数据")) ||
                failed.errorMessage.contains(QStringLiteral("写入PLC数据超时")));
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.requestCount(), 1);
        QVERIFY(isWriteRequest(peer.requests().last(), slaveId, address, value));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(!moduleFinished.last().at(1).toBool());

        const QJsonObject result = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                                   QJsonObject{{QStringLiteral("runId"), pausedRunId}});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!result.value(QStringLiteral("retryable")).toBool());
        for (const QString& key : {QStringLiteral("plc_write_success"), QStringLiteral("plc_write_count")}) {
            const QJsonObject output = moduleOutput(controller, pausedRunId, instanceId, key);
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!output.contains(QStringLiteral("value")));
        }
        QCOMPARE(
            controller.actor()
                ->executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), pausedRunId}})
                .value(QStringLiteral("status"))
                .toString(),
            QStringLiteral("uncertain"));
        QCOMPARE(peer.connectionCount(), 1);
        QCOMPARE(peer.requestCount(), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
        peer.close();
    }
};

QTEST_MAIN(TestAgentPLCWriteExecutionG5)
#include "test_agent_plcwrite_execution_g5.moc"
