#include "TCPServerPlugin.h"
#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/common/Logger.h"
#include "core/common/UserManager.h"
#include "core/engine/RunEngine.h"
#include "core/interface/IModule.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPalette>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>
#include <QtTest/QtTest>
#include <atomic>
#include <thread>

using namespace DeepLux;

class TestAgentTCPServerExecutionG5 : public QObject {
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

    static bool installTCPServerPlugin(const QString& pluginRoot) {
        const QDir pluginDir(QDir(pluginRoot).filePath(QStringLiteral("TCPServer")));
        if (!QDir().mkpath(pluginDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_TCPSERVER_G5_METADATA),
                           pluginDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_TCPSERVER_G5_LIBRARY),
                           pluginDir.filePath(QFileInfo(QStringLiteral(AGENT_TCPSERVER_G5_LIBRARY)).fileName()));
    }

    static quint16 reserveLocalPort() {
        QTcpServer reservation;
        if (!reservation.listen(QHostAddress::LocalHost, 0))
            return 0;
        const quint16 port = reservation.serverPort();
        reservation.close();
        return port;
    }

    static bool canConnectLocal(quint16 port) {
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, port);
        const bool connected = probe.waitForConnected(50);
        probe.abort();
        return connected;
    }

    static int moduleSignalCount(const QSignalSpy& spy, const QString& instanceId) {
        int count = 0;
        for (const QList<QVariant>& args : spy)
            if (!args.isEmpty() && args.first().toString() == instanceId)
                ++count;
        return count;
    }

    static ModuleInstance serverModule(const QString& id, quint16 port, int timeoutMs, const QString& bindAddress,
                                       const QString& response) {
        ModuleInstance module;
        module.id = id;
        module.moduleId = QStringLiteral("TCP服务器");
        module.name = QStringLiteral("Slice 49 loopback TCPServer");
        module.params = QJsonObject{{QStringLiteral("bindAddress"), bindAddress},
                                    {QStringLiteral("port"), port},
                                    {QStringLiteral("timeout"), timeoutMs},
                                    {QStringLiteral("writeData"), response},
                                    {QStringLiteral("readVariable"), QStringLiteral("agent_request")},
                                    {QStringLiteral("operation"), QStringLiteral("WriteRead")}};
        return module;
    }

    class ThreadedTcpClient final {
    public:
        ThreadedTcpClient(QByteArray request, QByteArray response)
            : m_request(std::move(request)), m_expectedResponse(std::move(response)) {}

        ~ThreadedTcpClient() {
            m_stop.store(true, std::memory_order_release);
            if (m_worker && m_thread.isRunning()) {
                QMetaObject::invokeMethod(
                    m_worker, [this] { m_stop.store(true, std::memory_order_release); }, Qt::QueuedConnection);
                m_thread.quit();
                m_thread.wait();
            }
        }

        void start(quint16 port) {
            m_worker = new QObject;
            m_worker->moveToThread(&m_thread);
            QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
            m_thread.start();
            QMetaObject::invokeMethod(
                m_worker,
                [this, port] {
                    QTcpSocket* socket = new QTcpSocket(m_worker);
                    QTimer retryTimer;
                    QTimer pollTimer;
                    QTimer deadlineTimer;
                    QEventLoop loop;
                    bool requestSent = false;
                    QObject::connect(socket, &QTcpSocket::connected, m_worker, [this, socket, &requestSent] {
                        if (requestSent)
                            return;
                        requestSent = true;
                        const qint64 written = socket->write(m_request);
                        socket->flush();
                        QMutexLocker locker(&m_mutex);
                        ++m_connectionCount;
                        m_sent = written == m_request.size() ? m_request : QByteArray();
                        if (written != m_request.size())
                            m_error = QStringLiteral("TCP client could not queue the complete request");
                    });
                    QObject::connect(socket, &QTcpSocket::readyRead, m_worker, [this, socket, &loop] {
                        bool complete = false;
                        {
                            QMutexLocker locker(&m_mutex);
                            m_received += socket->readAll();
                            complete = m_received == m_expectedResponse;
                        }
                        if (complete)
                            loop.quit();
                    });
                    QObject::connect(socket, &QTcpSocket::errorOccurred, m_worker,
                                     [socket, &retryTimer, &requestSent](QAbstractSocket::SocketError) {
                                         if (!requestSent) {
                                             socket->abort();
                                             retryTimer.start(20);
                                         }
                                     });
                    QObject::connect(&retryTimer, &QTimer::timeout, m_worker, [this, socket, port, &loop] {
                        if (m_stop.load(std::memory_order_acquire)) {
                            loop.quit();
                            return;
                        }
                        if (socket->state() == QAbstractSocket::UnconnectedState)
                            socket->connectToHost(QHostAddress::LocalHost, port);
                    });
                    QObject::connect(&pollTimer, &QTimer::timeout, m_worker, [this, &loop] {
                        if (m_stop.load(std::memory_order_acquire))
                            loop.quit();
                    });
                    QObject::connect(&deadlineTimer, &QTimer::timeout, m_worker, [this, &loop] {
                        QMutexLocker locker(&m_mutex);
                        m_error = QStringLiteral("TCP client did not finish before its deadline");
                        loop.quit();
                    });
                    {
                        QMutexLocker locker(&m_mutex);
                        m_armed = true;
                        m_condition.wakeAll();
                    }
                    retryTimer.setSingleShot(true);
                    retryTimer.start(0);
                    pollTimer.start(20);
                    deadlineTimer.setSingleShot(true);
                    deadlineTimer.start(4000);
                    loop.exec();
                    socket->abort();
                    {
                        QMutexLocker locker(&m_mutex);
                        m_finished = true;
                        m_condition.wakeAll();
                    }
                    m_thread.quit();
                },
                Qt::QueuedConnection);
        }

        bool waitUntilArmed(int timeoutMs) {
            QMutexLocker locker(&m_mutex);
            if (m_armed)
                return true;
            return m_condition.wait(&m_mutex, timeoutMs) && m_armed;
        }

        bool waitForFinished(int timeoutMs) {
            QMutexLocker locker(&m_mutex);
            if (!m_finished)
                m_condition.wait(&m_mutex, timeoutMs);
            const bool finished = m_finished;
            locker.unlock();
            if (finished)
                m_thread.wait();
            return finished;
        }

        int connectionCount() const {
            QMutexLocker locker(&m_mutex);
            return m_connectionCount;
        }

        QByteArray sent() const {
            QMutexLocker locker(&m_mutex);
            return m_sent;
        }

        QByteArray received() const {
            QMutexLocker locker(&m_mutex);
            return m_received;
        }

        QString error() const {
            QMutexLocker locker(&m_mutex);
            return m_error;
        }

    private:
        QByteArray m_request;
        QByteArray m_expectedResponse;
        mutable QMutex m_mutex;
        QWaitCondition m_condition;
        QThread m_thread;
        QObject* m_worker = nullptr;
        int m_connectionCount = 0;
        QByteArray m_sent;
        QByteArray m_received;
        QString m_error;
        bool m_armed = false;
        bool m_finished = false;
        std::atomic_bool m_stop{false};
    };

    class ReconnectingTcpCycleClient final {
    public:
        ReconnectingTcpCycleClient(QList<QByteArray> requests, QByteArray response)
            : m_requests(std::move(requests)), m_expectedResponse(std::move(response)) {}

        ~ReconnectingTcpCycleClient() {
            m_stop.store(true, std::memory_order_release);
            if (m_thread.joinable())
                m_thread.join();
        }

        void start(quint16 port) {
            m_thread = std::thread([this, port] {
                for (const QByteArray& request : m_requests) {
                    if (m_stop.load(std::memory_order_acquire))
                        break;
                    QTcpSocket socket;
                    QElapsedTimer connectDeadline;
                    connectDeadline.start();
                    bool connected = false;
                    while (!m_stop.load(std::memory_order_acquire) && connectDeadline.elapsed() < 8000) {
                        socket.abort();
                        socket.connectToHost(QHostAddress::LocalHost, port);
                        if (socket.waitForConnected(50)) {
                            connected = true;
                            break;
                        }
                        socket.abort();
                        QThread::msleep(10);
                    }
                    if (!connected) {
                        setError(QStringLiteral("TCPServer did not accept a cycle connection"));
                        break;
                    }
                    {
                        QMutexLocker locker(&m_mutex);
                        ++m_connectionCount;
                    }

                    if (socket.write(request) != request.size()) {
                        setError(QStringLiteral("TCP client could not queue the complete cycle request"));
                        break;
                    }
                    socket.flush();
                    QElapsedTimer writeDeadline;
                    writeDeadline.start();
                    while (socket.bytesToWrite() > 0 && writeDeadline.elapsed() < 1000) {
                        if (!socket.waitForBytesWritten(100))
                            break;
                    }
                    if (socket.bytesToWrite() > 0) {
                        setError(QStringLiteral("TCP client did not finish sending the cycle request"));
                        break;
                    }
                    {
                        QMutexLocker locker(&m_mutex);
                        m_sent.append(request);
                    }

                    QByteArray received;
                    bool responseClosed = false;
                    QElapsedTimer responseDeadline;
                    responseDeadline.start();
                    while (responseDeadline.elapsed() < 3000) {
                        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(100) &&
                            socket.state() == QAbstractSocket::UnconnectedState) {
                            received += socket.readAll();
                            responseClosed = true;
                            break;
                        }
                        received += socket.readAll();
                    }
                    {
                        QMutexLocker locker(&m_mutex);
                        m_received.append(received);
                    }
                    if (!responseClosed || received != m_expectedResponse) {
                        setError(QStringLiteral("TCPServer cycle response was not an exact closed exchange"));
                        break;
                    }
                    socket.abort();
                }
                m_finished.store(true, std::memory_order_release);
            });
        }

        bool waitForFinished(int timeoutMs) {
            QElapsedTimer timer;
            timer.start();
            while (!m_finished.load(std::memory_order_acquire) && timer.elapsed() < timeoutMs)
                QTest::qWait(10);
            if (!m_finished.load(std::memory_order_acquire))
                return false;
            if (m_thread.joinable())
                m_thread.join();
            return true;
        }

        int connectionCount() const {
            QMutexLocker locker(&m_mutex);
            return m_connectionCount;
        }

        QList<QByteArray> sent() const {
            QMutexLocker locker(&m_mutex);
            return m_sent;
        }

        QList<QByteArray> received() const {
            QMutexLocker locker(&m_mutex);
            return m_received;
        }

        QString error() const {
            QMutexLocker locker(&m_mutex);
            return m_error;
        }

    private:
        void setError(const QString& error) {
            QMutexLocker locker(&m_mutex);
            if (m_error.isEmpty())
                m_error = error;
        }

        QList<QByteArray> m_requests;
        QByteArray m_expectedResponse;
        mutable QMutex m_mutex;
        int m_connectionCount = 0;
        QList<QByteArray> m_sent;
        QList<QByteArray> m_received;
        QString m_error;
        std::thread m_thread;
        std::atomic_bool m_stop{false};
        std::atomic_bool m_finished{false};
    };

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
        QVERIFY(installTCPServerPlugin(pluginRoot));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        QVERIFY(plugins.pluginPaths().contains(QDir(pluginRoot).absolutePath()));
        const QList<PluginInfo> scanned = plugins.moduleInfos();
        QCOMPARE(scanned.size(), 1);
        QCOMPARE(scanned.first().id, QStringLiteral("com.deeplux.plugin.tcpserver"));
        QCOMPARE(scanned.first().name, QStringLiteral("TCP服务器"));
        QVERIFY(plugins.cameraInfos().isEmpty());
        QVERIFY(plugins.loadPlugin(QStringLiteral("TCP服务器")));
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
            qCritical() << "TCPServer execution did not become idle; preserving plugin directory"
                        << m_appDataDir.path();
            QFAIL("RunEngine remained busy; temporary TCPServer plugin tree was preserved");
            return;
        }
        engine.clearModules();
        ProjectManager::instance().closeProject();
        AgentController::instance().shutdown();
        PluginManager::instance().shutdown();
    }

    void testBindAddressControlIsVisibleAndSelectable() {
        const QJsonObject schema = AgentController::instance().handleToolCall(
            QStringLiteral("get_module_params_schema"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("TCP服务器")}});
        const QJsonObject bindAddressSchema =
            schema.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("bindAddress")).toObject();
        QCOMPARE(bindAddressSchema.value(QStringLiteral("enum")).toArray(),
                 (QJsonArray{QStringLiteral("Any"), QStringLiteral("LocalHost")}));

        PluginManager::ModulePtr module = PluginManager::instance().createManagedModule(QStringLiteral("TCP服务器"));
        QVERIFY(module);
        QWidget* widget = module->createConfigWidget();
        QVERIFY(widget);
        widget->resize(420, 360);
        widget->show();
        QCoreApplication::processEvents();
        QComboBox* bindAddress = widget->findChild<QComboBox*>(QStringLiteral("tcpServerBindAddressCombo"));
        QVERIFY(bindAddress);
        QVERIFY(bindAddress->isVisible());
        QCOMPARE(bindAddress->count(), 2);
        QCOMPARE(bindAddress->itemData(0).toString(), QStringLiteral("Any"));
        QCOMPARE(bindAddress->itemData(1).toString(), QStringLiteral("LocalHost"));
        QCOMPARE(bindAddress->currentData().toString(), QStringLiteral("Any"));
        bindAddress->setCurrentIndex(1);
        QCOMPARE(module->currentParams().value(QStringLiteral("bindAddress")).toString(), QStringLiteral("LocalHost"));

        const QString captureDir = qEnvironmentVariable("DEEPLUX_UI_CAPTURE_DIR");
        if (!captureDir.isEmpty()) {
            QVERIFY(QDir().mkpath(captureDir));
            const QPalette originalPalette = widget->palette();
            for (const int width : {420, 260}) {
                widget->resize(width, 360);
                widget->setPalette(originalPalette);
                QCoreApplication::processEvents();
                QImage light(widget->size(), QImage::Format_ARGB32_Premultiplied);
                light.fill(Qt::transparent);
                widget->render(&light);
                QVERIFY(
                    light.save(QDir(captureDir).filePath(QStringLiteral("tcpserver-config-light-%1.png").arg(width))));
                bindAddress->showPopup();
                QCoreApplication::processEvents();
                QWidget* lightPopup = QApplication::activePopupWidget();
                QVERIFY(lightPopup);
                QImage lightPopupImage(lightPopup->size(), QImage::Format_ARGB32_Premultiplied);
                lightPopupImage.fill(Qt::transparent);
                lightPopup->render(&lightPopupImage);
                QVERIFY(lightPopupImage.save(
                    QDir(captureDir).filePath(QStringLiteral("tcpserver-popup-light-%1.png").arg(width))));
                bindAddress->hidePopup();

                QPalette darkPalette = widget->palette();
                darkPalette.setColor(QPalette::Window, QColor(42, 42, 42));
                darkPalette.setColor(QPalette::WindowText, QColor(235, 235, 235));
                darkPalette.setColor(QPalette::Base, QColor(30, 30, 30));
                darkPalette.setColor(QPalette::Text, QColor(235, 235, 235));
                darkPalette.setColor(QPalette::Button, QColor(55, 55, 55));
                darkPalette.setColor(QPalette::ButtonText, QColor(235, 235, 235));
                darkPalette.setColor(QPalette::Highlight, QColor(70, 110, 170));
                darkPalette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
                widget->setPalette(darkPalette);
                QImage dark(widget->size(), QImage::Format_ARGB32_Premultiplied);
                dark.fill(Qt::transparent);
                widget->render(&dark);
                QVERIFY(
                    dark.save(QDir(captureDir).filePath(QStringLiteral("tcpserver-config-dark-%1.png").arg(width))));
                bindAddress->showPopup();
                QCoreApplication::processEvents();
                QWidget* darkPopup = QApplication::activePopupWidget();
                QVERIFY(darkPopup);
                QImage darkPopupImage(darkPopup->size(), QImage::Format_ARGB32_Premultiplied);
                darkPopupImage.fill(Qt::transparent);
                darkPopup->render(&darkPopupImage);
                QVERIFY(darkPopupImage.save(
                    QDir(captureDir).filePath(QStringLiteral("tcpserver-popup-dark-%1.png").arg(width))));
                bindAddress->hidePopup();
            }
        }
        widget->close();
        delete widget;
    }

    void testDefaultAnyBindingIsDisclosedWithoutStartingServer() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const quint16 port = reserveLocalPort();
        QVERIFY(port != 0);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice49_tcpserver_default_any");
        ModuleInstance module =
            serverModule(instanceId, port, 100, QStringLiteral("Any"), QStringLiteral("secret-response"));
        module.params.remove(QStringLiteral("bindAddress"));
        project->addModule(module);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        const QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray effects{
            QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
            QStringLiteral("tcp_server:%1@0.0.0.0/all_interfaces:%2 timeout=100ms operation=WriteRead")
                .arg(instanceId)
                .arg(port)};
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 effects);
        QVERIFY(!QJsonDocument(effects).toJson(QJsonDocument::Compact).contains("secret-response"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QVERIFY(!canConnectLocal(port));
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testRunFlowBindsLocalEndpointExpiresOnPortChangeAndReadsRequest() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const quint16 oldPort = reserveLocalPort();
        const quint16 approvedPort = reserveLocalPort();
        QVERIFY(oldPort != 0);
        QVERIFY(approvedPort != 0);
        QVERIFY(oldPort != approvedPort);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice49_tcpserver_target");
        const QByteArray request = QString::fromUtf8("请求-α-49").toUtf8();
        const QByteArray response = QString::fromUtf8("响应-β-49").toUtf8();
        Logger::instance().clearLogs();
        project->addModule(
            serverModule(instanceId, oldPort, 2000, QStringLiteral("LocalHost"), QString::fromUtf8(response)));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy responseSpy(&controller, &AgentController::llmResponseReceived);
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
        const QString requestText = QString::fromUtf8(request);
        const QJsonArray oldEffects{
            QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
            QStringLiteral("tcp_server:%1@127.0.0.1/loopback:%2 timeout=2000ms operation=WriteRead")
                .arg(instanceId)
                .arg(oldPort)};
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 oldEffects);
        QVERIFY(!QJsonDocument(oldEffects).toJson(QJsonDocument::Compact).contains(request));
        const QString oldConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!oldConfirmationId.isEmpty());
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("port"), approvedPort));
        const int auditCountBeforeExpiry = audit.count();
        QVERIFY(controller.confirmPendingTools(oldConfirmationId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeExpiry, 5000);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("确认已失效")));
        QVERIFY(!canConnectLocal(oldPort));
        QVERIFY(!canConnectLocal(approvedPort));

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray approvedEffects{
            QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
            QStringLiteral("tcp_server:%1@127.0.0.1/loopback:%2 timeout=2000ms operation=WriteRead")
                .arg(instanceId)
                .arg(approvedPort)};
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 approvedEffects);
        const QString approvedConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!approvedConfirmationId.isEmpty());
        QVERIFY(approvedConfirmationId != oldConfirmationId);
        QVERIFY(!canConnectLocal(approvedPort));
        ThreadedTcpClient client(request, response);
        QVERIFY(controller.confirmPendingTools(approvedConfirmationId));
        client.start(approvedPort);
        QVERIFY(client.waitUntilArmed(1000));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(started.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());

        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success,
                 qPrintable(QStringLiteral("%1; client connections=%2 sent=%3 received=%4 client error=%5")
                                .arg(result.errorMessage)
                                .arg(client.connectionCount())
                                .arg(client.sent().size())
                                .arg(client.received().size())
                                .arg(client.error())));
        QVERIFY2(client.waitForFinished(5000), qPrintable(client.error()));
        QVERIFY2(client.connectionCount() == 1,
                 qPrintable(QStringLiteral("client connections=%1 sent=%2 received=%3 error=%4")
                                .arg(client.connectionCount())
                                .arg(client.sent().size())
                                .arg(client.received().size())
                                .arg(client.error())));
        QCOMPARE(client.sent(), request);
        QCOMPARE(client.received(), response);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(executionThreadId.load(std::memory_order_relaxed) == mainThreadId, false);
        bool observedLoopbackBind = false;
        for (const LogEntry& entry : Logger::instance().logs(QStringLiteral("Communication"))) {
            if (entry.message.startsWith(QStringLiteral("TCPServer: Listening on 127.0.0.1:")) &&
                entry.message.endsWith(QString::number(approvedPort)))
                observedLoopbackBind = true;
            QVERIFY(!entry.message.startsWith(QStringLiteral("TCPServer: Listening on 0.0.0.0:")));
        }
        QVERIFY(observedLoopbackBind);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject value = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_request")}});
        QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(value.value(QStringLiteral("runId")).toString(), runId);
        QCOMPARE(value.value(QStringLiteral("value")).toString(), requestText);
        const QJsonObject byteCount =
            controller.handleToolCall(QStringLiteral("get_module_output"),
                                      QJsonObject{{QStringLiteral("runId"), runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("agent_request_bytes")}});
        QCOMPARE(byteCount.value(QStringLiteral("value")).toInt(), request.size());
        disconnect(threadConnection);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testApprovedListenerTimeoutReportsWaitFailure() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const quint16 port = reserveLocalPort();
        QVERIFY(port != 0);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice49_tcpserver_wait_timeout");
        project->addModule(serverModule(instanceId, port, 100, QStringLiteral("LocalHost"), QStringLiteral("unused")));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        const QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QString runId = toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);

        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY(!result.success);
        QCOMPARE(result.failedModule, instanceId);
        QVERIFY(result.errorMessage.contains(QStringLiteral("等待客户端连接超时")));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        const QJsonObject status =
            controller.handleToolCall(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(status.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(status.value(QStringLiteral("lastError"))
                    .toObject()
                    .value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("等待客户端连接超时")));
        QVERIFY(!canConnectLocal(port));
        engine.clearModules();
        ProjectManager::instance().closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testSuccessfulBoundedCycleUsesThreeExactExchanges() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const quint16 port = reserveLocalPort();
        QVERIFY(port != 0);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice130_tcpserver_cycle_success");
        const QByteArray response = QByteArrayLiteral("slice130-configured-response");
        project->addModule(
            serverModule(instanceId, port, 2000, QStringLiteral("LocalHost"), QString::fromUtf8(response)));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QString sessionId;
        QMutex observedOutputMutex;
        QList<QByteArray> observedRequests;
        QList<int> observedRequestBytes;
        QList<bool> outputSnapshotsAvailable;
        QObject resultContext;
        connect(
            &engine, &RunEngine::runFinished, &resultContext,
            [&engine, &instanceId, &observedOutputMutex, &observedRequests, &observedRequestBytes,
             &outputSnapshotsAvailable](const RunResult& result) {
                ImageData image;
                QMap<QString, QVariant> values;
                PortValueMap ports;
                const bool available = engine.runOutputSnapshot(result.runId, instanceId, image, values, ports);
                QMutexLocker locker(&observedOutputMutex);
                outputSnapshotsAvailable.append(available);
                observedRequests.append(values.value(QStringLiteral("agent_request")).toByteArray());
                observedRequestBytes.append(values.value(QStringLiteral("agent_request_bytes")).toInt());
            },
            Qt::DirectConnection);
        connect(controller.actor(), &AgentActor::toolExecuted, &resultContext,
                [&sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                });

        const QJsonObject cycleParams{{QStringLiteral("mode"), QStringLiteral("cycle")},
                                      {QStringLiteral("maxCycles"), 3}};
        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!canConnectLocal(port));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QVERIFY(!canConnectLocal(port));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(audit.count(), 1);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        const QList<QByteArray> requests{QByteArrayLiteral("slice130-request-1"),
                                         QByteArrayLiteral("slice130-request-2"),
                                         QByteArrayLiteral("slice130-request-3")};
        ReconnectingTcpCycleClient client(requests, response);
        client.start(port);
        const int toolCountBeforeApprove = toolExecuted.count();
        QVERIFY(controller.confirmPendingTools(approvedId));

        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 12000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 12000);
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), toolCountBeforeApprove + 1, 5000);
        QVERIFY2(client.waitForFinished(5000), qPrintable(client.error()));
        QCOMPARE(client.connectionCount(), 3);
        QCOMPARE(client.sent(), requests);
        QCOMPARE(client.received(), (QList<QByteArray>{response, response, response}));
        QVERIFY(client.error().isEmpty());
        {
            QMutexLocker locker(&observedOutputMutex);
            QCOMPARE(outputSnapshotsAvailable, (QList<bool>{true, true, true}));
            QCOMPARE(observedRequests, requests);
            QCOMPARE(observedRequestBytes,
                     (QList<int>{requests.at(0).size(), requests.at(1).size(), requests.at(2).size()}));
        }
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 3);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 3);
        for (const QList<QVariant>& args : moduleFinished)
            QVERIFY(args.at(1).toBool());
        for (const QList<QVariant>& args : runFinished)
            QVERIFY(qvariant_cast<RunResult>(args.at(0)).success);
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(toolExecuted.count(), 1);
        QVERIFY(!sessionId.isEmpty());
        QCOMPARE(audit.count(), 2);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        const QString lastRunId = qvariant_cast<RunResult>(runFinished.last().at(0)).runId;
        QVERIFY(!lastRunId.isEmpty());
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject finalRequest = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), lastRunId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("agent_request")}});
        QCOMPARE(finalRequest.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(finalRequest.value(QStringLiteral("value")).toString(), QString::fromUtf8(requests.last()));
        const QJsonObject finalRequestBytes =
            controller.handleToolCall(QStringLiteral("get_module_output"),
                                      QJsonObject{{QStringLiteral("runId"), lastRunId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("agent_request_bytes")}});
        QCOMPARE(finalRequestBytes.value(QStringLiteral("value")).toInt(), requests.last().size());
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        QJsonObject session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                        QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), 3);
        QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), 3);
        QVERIFY(!session.value(QStringLiteral("isBusy")).toBool());
        QCOMPARE(session.value(QStringLiteral("lastSuccessfulRunId")).toString(), lastRunId);
        QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(), lastRunId);

        const RunObservation observationAtQuiet = engine.runObservation(sessionId);
        QVERIFY(observationAtQuiet.finished);
        QCOMPARE(observationAtQuiet.completedCycles, 3);
        QCOMPARE(observationAtQuiet.lastSuccessfulRunId, lastRunId);
        QElapsedTimer quiet;
        quiet.start();
        const int connectionsAtQuiet = client.connectionCount();
        const QList<QByteArray> sentAtQuiet = client.sent();
        const QList<QByteArray> receivedAtQuiet = client.received();
        const int startedAtQuiet = moduleSignalCount(moduleStarted, instanceId);
        const int finishedAtQuiet = moduleSignalCount(moduleFinished, instanceId);
        const int runsAtQuiet = runFinished.count();
        const int stopsAtQuiet = cycleStopped.count();
        const int toolsAtQuiet = toolExecuted.count();
        const int auditsAtQuiet = audit.count();
        while (quiet.elapsed() < 350) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QTest::qWait(5);
        }
        const RunObservation observationAfterQuiet = engine.runObservation(sessionId);
        QVERIFY(observationAfterQuiet.finished);
        QCOMPARE(observationAfterQuiet.state, observationAtQuiet.state);
        QCOMPARE(observationAfterQuiet.completedCycles, observationAtQuiet.completedCycles);
        QCOMPARE(observationAfterQuiet.lastSuccessfulRunId, observationAtQuiet.lastSuccessfulRunId);
        QCOMPARE(observationAfterQuiet.lastCompletedRunId, observationAtQuiet.lastCompletedRunId);
        QCOMPARE(client.connectionCount(), connectionsAtQuiet);
        QCOMPARE(client.sent(), sentAtQuiet);
        QCOMPARE(client.received(), receivedAtQuiet);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), startedAtQuiet);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), finishedAtQuiet);
        QCOMPARE(runFinished.count(), runsAtQuiet);
        QCOMPARE(cycleStopped.count(), stopsAtQuiet);
        QCOMPARE(toolExecuted.count(), toolsAtQuiet);
        QCOMPARE(audit.count(), auditsAtQuiet);
        QVERIFY(!canConnectLocal(port));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testResponseWriteUncertainStopsCycleAndResetsPerExecutionState() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const quint16 port = reserveLocalPort();
        QVERIFY(port != 0);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice117_tcpserver_partial_response");
        const QByteArray request = QByteArrayLiteral("slice117-request");
        project->addModule(
            serverModule(instanceId, port, 1000, QStringLiteral("LocalHost"), QStringLiteral("private-response")));

        std::atomic_int responseAttempts{0};
        struct ResponseHookReset {
            ~ResponseHookReset() {
                TCPServerPlugin::setResponseWriterTestHook({});
            }
        } responseHookReset;
        TCPServerPlugin::setResponseWriterTestHook([&responseAttempts](QTcpSocket&, const QByteArray& bytes, int) {
            const int attempt = responseAttempts.fetch_add(1, std::memory_order_relaxed);
            TCPServerPlugin::ResponseWriteOutcome outcome;
            if (attempt == 0) {
                outcome.acceptedBytes = qMin<qint64>(1, bytes.size());
                outcome.failureStage = QStringLiteral("injected_drain_failure");
                outcome.errorMessage = QStringLiteral("injected response drain failure");
            } else {
                outcome.failureStage = QStringLiteral("injected_zero_byte_failure");
                outcome.errorMessage = QStringLiteral("injected response write failure");
            }
            return outcome;
        });

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QString sessionId;
        QObject sessionContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &sessionContext,
                [&sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                });
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject cycleParams{{QStringLiteral("mode"), QStringLiteral("cycle")},
                                      {QStringLiteral("maxCycles"), 3}};
        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(responseAttempts.load(std::memory_order_relaxed), 0);
        QCOMPARE(toolExecuted.count(), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(responseAttempts.load(std::memory_order_relaxed), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);

        pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        const int toolCountBeforeApprove = toolExecuted.count();
        {
            ThreadedTcpClient client(request, QByteArray());
            client.start(port);
            QVERIFY(client.waitUntilArmed(2000));
            QVERIFY(controller.confirmPendingTools(approvedId));
            QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(toolExecuted.count() > toolCountBeforeApprove, 5000);
            QCOMPARE(client.connectionCount(), 1);
            QCOMPARE(client.sent(), request);
            QVERIFY(client.received().isEmpty());

            const RunResult uncertainRun = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY(!uncertainRun.success);
            QCOMPARE(uncertainRun.errorCode, ExecError::SideEffectUncertain);
            QCOMPARE(uncertainRun.failedModule, instanceId);
            QVERIFY(!uncertainRun.diagnostics.contains("private-response"));
            QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
            QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
            QVERIFY(!moduleFinished.last().at(1).toBool());
            QCOMPARE(responseAttempts.load(std::memory_order_relaxed), 1);
            QVERIFY(!sessionId.isEmpty());

            QJsonObject session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                            QJsonObject{{QStringLiteral("runId"), sessionId}});
            QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QCOMPARE(session.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!session.value(QStringLiteral("retryable")).toBool());
            QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), 0);
            const QJsonObject lastError = session.value(QStringLiteral("lastError")).toObject();
            QCOMPARE(lastError.value(QStringLiteral("code")).toInt(), ExecError::SideEffectUncertain);
            QVERIFY(!lastError.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!lastError.value(QStringLiteral("diagnostics")).toString().contains("private-response"));

            const int toolCountBeforeQuiet = toolExecuted.count();
            QElapsedTimer quiet;
            quiet.start();
            while (quiet.elapsed() < 250) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                QTest::qWait(5);
            }
            QCOMPARE(client.connectionCount(), 1);
            QCOMPARE(responseAttempts.load(std::memory_order_relaxed), 1);
            QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
            QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
            QCOMPARE(runFinished.count(), 1);
            QCOMPARE(cycleStopped.count(), 1);
            QCOMPARE(toolExecuted.count(), toolCountBeforeQuiet);
            session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                QJsonObject{{QStringLiteral("runId"), sessionId}});
            QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), 0);
        }

        // A later call on the same runtime module reports a zero-byte write failure
        // as definite failure, proving the previous uncertain byte count is cleared.
        const int previousToolCount = toolExecuted.count();
        const int previousRunCount = runFinished.count();
        QJsonObject once =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(once.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString zeroByteConfirmation = controller.pendingConfirmationId();
        QVERIFY(!zeroByteConfirmation.isEmpty());
        {
            ThreadedTcpClient client(request, QByteArray());
            client.start(port);
            QVERIFY(client.waitUntilArmed(2000));
            QVERIFY(controller.confirmPendingTools(zeroByteConfirmation));
            QTRY_VERIFY_WITH_TIMEOUT(toolExecuted.count() > previousToolCount, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), previousRunCount + 1, 5000);
            QCOMPARE(responseAttempts.load(std::memory_order_relaxed), 2);
            const RunResult definiteRun = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY(!definiteRun.success);
            QVERIFY(definiteRun.errorCode != ExecError::SideEffectUncertain);
            QCOMPARE(definiteRun.failedModule, instanceId);
            const QJsonObject definiteStatus = controller.handleToolCall(
                QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), definiteRun.runId}});
            QCOMPARE(definiteStatus.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
            QVERIFY(!definiteStatus.contains(QStringLiteral("sideEffectStatus")));
            QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 2);
            QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 2);
        }

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testStepOnceConfirmsTCPServerSingleExchange() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const quint16 port = reserveLocalPort();
        QVERIFY(port != 0);
        QVERIFY(!canConnectLocal(port));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice74_tcpserver_step");
        const QByteArray request = QByteArrayLiteral("slice74-step-request");
        const QByteArray response = QByteArrayLiteral("slice74-step-response");
        project->addModule(
            serverModule(instanceId, port, 5000, QStringLiteral("LocalHost"), QString::fromUtf8(response)));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QString endpointEffect =
            QStringLiteral("tcp_server:%1@127.0.0.1/loopback:%2 timeout=5000ms operation=WriteRead")
                .arg(instanceId)
                .arg(port);

        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(endpointEffect));
        QVERIFY(!canConnectLocal(port));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(!canConnectLocal(port));
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
        const bool confirmed = controller.confirmPendingTools(approvedId);
        std::atomic_bool connected{false};
        std::atomic_bool clientFinished{false};
        QByteArray clientReceived;
        QString clientError;
        std::thread clientThread([&] {
            struct FinishedSignal {
                std::atomic_bool& finished;
                ~FinishedSignal() {
                    finished.store(true, std::memory_order_release);
                }
            } finishedSignal{clientFinished};
            QTcpSocket socket;
            QElapsedTimer deadline;
            deadline.start();
            while (deadline.elapsed() < 6000) {
                socket.connectToHost(QHostAddress::LocalHost, port);
                if (socket.waitForConnected(50)) {
                    connected.store(true, std::memory_order_release);
                    break;
                }
                socket.abort();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!connected.load(std::memory_order_acquire)) {
                clientError = socket.errorString();
                return;
            }
            if (socket.write(request) != request.size() || !socket.waitForBytesWritten(1000)) {
                clientError = socket.errorString();
                return;
            }
            if (!socket.waitForReadyRead(3000)) {
                clientError = socket.errorString();
                return;
            }
            clientReceived = socket.readAll();
        });
        QElapsedTimer clientDeadline;
        clientDeadline.start();
        while (!clientFinished.load(std::memory_order_acquire) && clientDeadline.elapsed() < 8000)
            QTest::qWait(10);
        clientThread.join();
        QVERIFY(confirmed);
        QVERIFY2(connected.load(std::memory_order_acquire), qPrintable(clientError));
        QCOMPARE(clientReceived, response);
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const QJsonObject stepResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = qvariant_cast<RunResult>(runFinished.last().at(0)).runId;
        QVERIFY(!runId.isEmpty());
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

    void testResumeFlowConfirmsTCPServerSingleExchange() {
        resetAgentFlow();
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const quint16 port = reserveLocalPort();
        QVERIFY(port != 0);
        QVERIFY(!canConnectLocal(port));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice74_tcpserver_resume");
        const QByteArray request = QByteArrayLiteral("slice74-resume-request");
        const QByteArray response = QByteArrayLiteral("slice74-resume-response");
        ModuleInstance server =
            serverModule(instanceId, port, 5000, QStringLiteral("LocalHost"), QString::fromUtf8(response));
        server.breakpoint = true;
        project->addModule(server);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QVERIFY(!canConnectLocal(port));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        const QString endpointEffect =
            QStringLiteral("tcp_server:%1@127.0.0.1/loopback:%2 timeout=5000ms operation=WriteRead")
                .arg(instanceId)
                .arg(port);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(endpointEffect));
        QVERIFY(!canConnectLocal(port));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, runId);
        QVERIFY(!canConnectLocal(port));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleFinished.count(), 0);
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
        std::atomic_bool connected{false};
        std::atomic_bool clientFinished{false};
        QByteArray clientReceived;
        QString clientError;
        std::thread clientThread([&] {
            struct FinishedSignal {
                std::atomic_bool& finished;
                ~FinishedSignal() {
                    finished.store(true, std::memory_order_release);
                }
            } finishedSignal{clientFinished};
            QTcpSocket client;
            QElapsedTimer deadline;
            deadline.start();
            while (deadline.elapsed() < 6000) {
                client.connectToHost(QHostAddress::LocalHost, port);
                if (client.waitForConnected(50)) {
                    connected.store(true, std::memory_order_release);
                    break;
                }
                client.abort();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!connected.load(std::memory_order_acquire)) {
                clientError = client.errorString();
                return;
            }
            if (client.write(request) != request.size() || !client.waitForBytesWritten(1000)) {
                clientError = client.errorString();
                return;
            }
            if (!client.waitForReadyRead(3000)) {
                clientError = client.errorString();
                return;
            }
            clientReceived = client.readAll();
        });
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QElapsedTimer clientDeadline;
        clientDeadline.start();
        while (!clientFinished.load(std::memory_order_acquire) && clientDeadline.elapsed() < 8000)
            QTest::qWait(10);
        clientThread.join();
        QVERIFY2(connected.load(std::memory_order_acquire), qPrintable(clientError));
        QCOMPARE(clientReceived, response);
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
};

QTEST_MAIN(TestAgentTCPServerExecutionG5)
#include "test_agent_tcpserver_execution_g5.moc"
