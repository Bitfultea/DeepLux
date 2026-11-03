#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTableWidget>
#include <QtTest/QtTest>
#include <algorithm>
#include <core/agent/AgentActor.h>
#include <core/agent/ToolSchema.h>
#include <core/common/UserManager.h>
#include <core/communication/CommunicationManager.h>
#include <thread>
#include <ui/views/CommunicationSetView.h>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace DeepLux;

#ifdef Q_OS_UNIX
namespace {
struct PtyScope {
    CommunicationManager* manager = nullptr;
    QString configId;
    int masterFd = -1;
    int slaveFd = -1;

    ~PtyScope() {
        if (manager && !configId.isEmpty()) {
            manager->disconnect(configId);
            manager->removeConfig(configId);
        }
        if (slaveFd >= 0)
            ::close(slaveFd);
        if (masterFd >= 0)
            ::close(masterFd);
    }
};
} // namespace
#endif

class TestCommunicationManager : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void testAddUpdateRemoveConfig();
    void testAgentCommunicationListOmitsEndpointFields();
    void testConnectionStateTransitionsAreStoredAndSignaled();
    void testTcpClientErrorRemainsVisibleUntilExplicitDisconnect();
    void testPlcConfigUsesTcpTransport();
    void testSerialPortPtyRoundTrip();
    void testCommunicationSetViewApplyCreatesConfig();
    void testCommunicationViewAddsAndDeletesConfigThroughGui();
    void testCommunicationViewConnectsAndDisconnectsLoopbackTcp();
    void testCommunicationSetViewTypeSwitchEnablesRelevantFields();
    void testCommunicationSetViewClearsAndBlocksAfterPermissionRevocation();
};

void TestCommunicationManager::initTestCase() {
    ToolSchema::instance().registerDefaultTools();
}

void TestCommunicationManager::init() {
    CommunicationManager::instance().disconnectAll();
    CommunicationManager::instance().clearConfigs();
}

void TestCommunicationManager::cleanup() {
    CommunicationManager::instance().disconnectAll();
    CommunicationManager::instance().clearConfigs();
}

void TestCommunicationManager::testAddUpdateRemoveConfig() {
    CommunicationConfig config;
    config.id = "tcp1";
    config.name = "TCP One";
    config.type = CommunicationType::TCP_Client;
    config.ipAddress = "127.0.0.1";
    config.port = 1234;

    QVERIFY(CommunicationManager::instance().addOrUpdateConfig(config));
    QCOMPARE(CommunicationManager::instance().configs().size(), 1);
    QVERIFY(CommunicationManager::instance().findConfig("tcp1") != nullptr);
    QCOMPARE(CommunicationManager::instance().findConfig("tcp1")->port, 1234);

    config.port = 4321;
    QVERIFY(CommunicationManager::instance().addOrUpdateConfig(config));
    QCOMPARE(CommunicationManager::instance().configs().size(), 1);
    QCOMPARE(CommunicationManager::instance().findConfig("tcp1")->port, 4321);

    QVERIFY(CommunicationManager::instance().removeConfig("tcp1"));
    QCOMPARE(CommunicationManager::instance().configs().size(), 0);
    QVERIFY(CommunicationManager::instance().findConfig("tcp1") == nullptr);
}

void TestCommunicationManager::testAgentCommunicationListOmitsEndpointFields() {
    CommunicationManager& manager = CommunicationManager::instance();
    AgentActor actor;
    const QJsonObject emptyResult = actor.executeTool(QStringLiteral("list_communications"), QJsonObject());
    QVERIFY(emptyResult.value(QStringLiteral("communications")).toArray().isEmpty());

    CommunicationConfig config;
    config.id = QStringLiteral("private-id-127.0.0.1");
    config.name = QStringLiteral("private-name");
    config.type = CommunicationType::TCP_Client;
    config.ipAddress = QStringLiteral("198.51.100.77");
    config.port = 4567;
    config.extraConfig.insert(QStringLiteral("token"), QStringLiteral("private-token"));
    QVERIFY(manager.addOrUpdateConfig(config));
    CommunicationConfig server = config;
    server.id = QStringLiteral("server-id");
    server.name = QStringLiteral("Server");
    server.type = CommunicationType::TCP_Server;
    QVERIFY(manager.addOrUpdateConfig(server));
    CommunicationConfig serial = config;
    serial.id = QStringLiteral("serial-id");
    serial.name = QStringLiteral("Serial");
    serial.type = CommunicationType::SerialPort;
    serial.portName = QStringLiteral("COM-test");
    QVERIFY(manager.addOrUpdateConfig(serial));
    CommunicationConfig plc = config;
    plc.id = QStringLiteral("plc-id");
    plc.name = QStringLiteral("PLC");
    plc.type = CommunicationType::PLC;
    QVERIFY(manager.addOrUpdateConfig(plc));

    const QJsonObject result = actor.executeTool(QStringLiteral("list_communications"), QJsonObject());
    QCOMPARE(result.value(QStringLiteral("source")).toString(), QStringLiteral("CommunicationManager.configs"));
    const QJsonArray rows = result.value(QStringLiteral("communications")).toArray();
    QCOMPARE(rows.size(), 4);
    const QJsonObject row = rows[0].toObject();
    QCOMPARE(row.size(), 4);
    QCOMPARE(row.value(QStringLiteral("id")).toString(), config.id);
    QCOMPARE(row.value(QStringLiteral("name")).toString(), config.name);
    QCOMPARE(row.value(QStringLiteral("type")).toString(), QStringLiteral("tcp_client"));
    QCOMPARE(row.value(QStringLiteral("state")).toString(), QStringLiteral("disconnected"));
    QCOMPARE(rows[1].toObject().value(QStringLiteral("type")).toString(), QStringLiteral("tcp_server"));
    QCOMPARE(rows[2].toObject().value(QStringLiteral("type")).toString(), QStringLiteral("serial_port"));
    QCOMPARE(rows[3].toObject().value(QStringLiteral("type")).toString(), QStringLiteral("plc"));
    const QByteArray encoded = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QVERIFY(!encoded.contains("ipAddress"));
    QVERIFY(!encoded.contains("portName"));
    QVERIFY(!encoded.contains("extraConfig"));
    QVERIFY(!encoded.contains("198.51.100.77"));
    QVERIFY(!encoded.contains("private-token"));
    QCOMPARE(manager.configs().size(), 4);
    QCOMPARE(manager.state(config.id), CommunicationState::Disconnected);

    QJsonObject wrongThread;
    std::thread otherThread([&actor, &wrongThread]() {
        wrongThread = actor.executeTool(QStringLiteral("list_communications"), QJsonObject());
    });
    otherThread.join();
    QCOMPARE(wrongThread.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("COMMUNICATION_THREAD_AFFINITY"));
    QCOMPARE(manager.configs().size(), 4);
    QCOMPARE(manager.state(config.id), CommunicationState::Disconnected);
}

void TestCommunicationManager::testConnectionStateTransitionsAreStoredAndSignaled() {
    CommunicationManager& manager = CommunicationManager::instance();
    CommunicationConfig serialConfig;
    serialConfig.id = QStringLiteral("serial-open-failure");
    serialConfig.name = QStringLiteral("Serial open failure");
    serialConfig.type = CommunicationType::SerialPort;
    serialConfig.portName = QStringLiteral("/dev/deeplux-no-such-serial-port");
    QVERIFY(manager.addOrUpdateConfig(serialConfig));
    QObject serialObserver;
    QList<CommunicationState> serialStates;
    QList<bool> serialSignalStateMatches;
    QObject::connect(&manager, &CommunicationManager::connectionStateChanged, &serialObserver,
                     [&manager, &serialStates, &serialSignalStateMatches, &serialConfig](const QString& id,
                                                                                         CommunicationState state) {
                         if (id == serialConfig.id) {
                             serialStates.append(state);
                             serialSignalStateMatches.append(manager.state(id) == state);
                         }
                     });
    QVERIFY(manager.connect(serialConfig.id));
    QCOMPARE(manager.state(serialConfig.id), CommunicationState::Error);
    QVERIFY(serialStates.contains(CommunicationState::Connecting));
    QVERIFY(serialStates.contains(CommunicationState::Error));
    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    auto findCommunicationState = [&actor](const QString& id) {
        const QJsonArray rows = actor.executeTool(QStringLiteral("list_communications"), QJsonObject())
                                    .value(QStringLiteral("communications"))
                                    .toArray();
        for (const QJsonValue& value : rows) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("id")).toString() == id)
                return row.value(QStringLiteral("state")).toString();
        }
        return QStringLiteral("missing");
    };
    QCOMPARE(findCommunicationState(serialConfig.id), QStringLiteral("error"));
    manager.disconnect(serialConfig.id);
    QCOMPARE(findCommunicationState(serialConfig.id), QStringLiteral("disconnected"));
    QVERIFY(!serialSignalStateMatches.isEmpty());
    QVERIFY(std::all_of(serialSignalStateMatches.cbegin(), serialSignalStateMatches.cend(),
                        [](bool matches) { return matches; }));

    QTcpServer reservedPort;
    if (!reservedPort.listen(QHostAddress::LocalHost, 0)) {
        qWarning() << "Cannot bind a local test server; TCP server bind-failure state is not exercised in this run";
        return;
    }
    CommunicationConfig serverConfig;
    serverConfig.id = QStringLiteral("server-bind-failure");
    serverConfig.name = QStringLiteral("Server bind failure");
    serverConfig.type = CommunicationType::TCP_Server;
    serverConfig.port = reservedPort.serverPort();
    QVERIFY(manager.addOrUpdateConfig(serverConfig));
    QObject serverObserver;
    QList<CommunicationState> serverStates;
    QList<bool> serverSignalStateMatches;
    QObject::connect(&manager, &CommunicationManager::connectionStateChanged, &serverObserver,
                     [&manager, &serverStates, &serverSignalStateMatches, &serverConfig](const QString& id,
                                                                                         CommunicationState state) {
                         if (id == serverConfig.id) {
                             serverStates.append(state);
                             serverSignalStateMatches.append(manager.state(id) == state);
                         }
                     });
    QVERIFY(manager.connect(serverConfig.id));
    QCOMPARE(manager.state(serverConfig.id), CommunicationState::Error);
    QVERIFY(serverStates.contains(CommunicationState::Connecting));
    QVERIFY(serverStates.contains(CommunicationState::Error));
    manager.disconnect(serverConfig.id);
    QVERIFY(std::all_of(serverSignalStateMatches.cbegin(), serverSignalStateMatches.cend(),
                        [](bool matches) { return matches; }));
}

void TestCommunicationManager::testTcpClientErrorRemainsVisibleUntilExplicitDisconnect() {
    CommunicationManager& manager = CommunicationManager::instance();
    CommunicationConfig config;
    config.id = QStringLiteral("tcp-client-error");
    config.name = QStringLiteral("TCP client error");
    config.type = CommunicationType::TCP_Client;
    config.ipAddress = QStringLiteral("127.0.0.1");
    config.port = 1; // loopback closed port: no listener is created or contacted outside the machine
    QVERIFY(manager.addOrUpdateConfig(config));

    AgentActor actor;
    auto listedState = [&actor, &config]() {
        const QJsonArray rows = actor.executeTool(QStringLiteral("list_communications"), QJsonObject())
                                    .value(QStringLiteral("communications"))
                                    .toArray();
        for (const QJsonValue& value : rows) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("id")).toString() == config.id)
                return row.value(QStringLiteral("state")).toString();
        }
        return QStringLiteral("missing");
    };
    QObject stateObserver;
    QList<bool> signalStateMatches;
    QString agentStateAtConnecting;
    QObject::connect(&manager, &CommunicationManager::connectionStateChanged, &stateObserver,
                     [&manager, &signalStateMatches, &agentStateAtConnecting, &listedState,
                      &config](const QString& id, CommunicationState state) {
                         if (id == config.id) {
                             signalStateMatches.append(manager.state(id) == state);
                             if (state == CommunicationState::Connecting)
                                 agentStateAtConnecting = listedState();
                         }
                     });
    QVERIFY(manager.connect(config.id));
    QCOMPARE(agentStateAtConnecting, QStringLiteral("connecting"));
    QVERIFY(!signalStateMatches.isEmpty());
    QVERIFY(std::all_of(signalStateMatches.cbegin(), signalStateMatches.cend(), [](bool matches) { return matches; }));
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(config.id), CommunicationState::Error, 1000);
    QCOMPARE(listedState(), QStringLiteral("error"));

    manager.disconnect(config.id);
    QCOMPARE(manager.state(config.id), CommunicationState::Disconnected);
    QCOMPARE(listedState(), QStringLiteral("disconnected"));
    QVERIFY(!signalStateMatches.isEmpty());
    QVERIFY(std::all_of(signalStateMatches.cbegin(), signalStateMatches.cend(), [](bool matches) { return matches; }));
}

void TestCommunicationManager::testPlcConfigUsesTcpTransport() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        QSKIP(qPrintable(QString("Local TCP listener unavailable: %1").arg(server.errorString())));
    }

    CommunicationConfig config;
    config.id = "plc1";
    config.name = "PLC One";
    config.type = CommunicationType::PLC;
    config.ipAddress = "127.0.0.1";
    config.port = server.serverPort();

    QVERIFY(CommunicationManager::instance().addOrUpdateConfig(config));

    QVERIFY(CommunicationManager::instance().connect("plc1"));
    QCOMPARE(CommunicationManager::instance().state("plc1"), CommunicationState::Connecting);
    const QJsonArray connectingRows = AgentActor()
                                          .executeTool(QStringLiteral("list_communications"), QJsonObject())
                                          .value(QStringLiteral("communications"))
                                          .toArray();
    QCOMPARE(connectingRows.first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("connecting"));
    QTRY_VERIFY_WITH_TIMEOUT(CommunicationManager::instance().isConnected("plc1"), 1000);
    QCOMPARE(CommunicationManager::instance().state("plc1"), CommunicationState::Connected);
    const QJsonArray connectedRows = AgentActor()
                                         .executeTool(QStringLiteral("list_communications"), QJsonObject())
                                         .value(QStringLiteral("communications"))
                                         .toArray();
    QCOMPARE(connectedRows.first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("connected"));
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);

    QTcpSocket* serverSocket = server.nextPendingConnection();
    QVERIFY(serverSocket != nullptr);

    QVERIFY(CommunicationManager::instance().sendData("plc1", QByteArray("abc")));
    QTRY_VERIFY_WITH_TIMEOUT(serverSocket->bytesAvailable() >= 3, 1000);
    QCOMPARE(serverSocket->readAll(), QByteArray("abc"));

    CommunicationManager::instance().disconnect("plc1");
    serverSocket->disconnectFromHost();
    serverSocket->deleteLater();
}

void TestCommunicationManager::testSerialPortPtyRoundTrip() {
#ifndef Q_OS_UNIX
    QSKIP("POSIX PTY loopback is unavailable on this platform");
#else
    CommunicationManager& manager = CommunicationManager::instance();
    PtyScope pty;
    pty.manager = &manager;
    pty.configId = QStringLiteral("pty-serial-roundtrip");
    pty.masterFd = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    QVERIFY2(pty.masterFd >= 0,
             qPrintable(QStringLiteral("posix_openpt failed: %1").arg(QString::fromLocal8Bit(strerror(errno)))));
    QVERIFY2(::grantpt(pty.masterFd) == 0,
             qPrintable(QStringLiteral("grantpt failed: %1").arg(QString::fromLocal8Bit(strerror(errno)))));
    QVERIFY2(::unlockpt(pty.masterFd) == 0,
             qPrintable(QStringLiteral("unlockpt failed: %1").arg(QString::fromLocal8Bit(strerror(errno)))));
    const char* slaveName = ::ptsname(pty.masterFd);
    QVERIFY2(slaveName != nullptr,
             qPrintable(QStringLiteral("ptsname failed: %1").arg(QString::fromLocal8Bit(strerror(errno)))));
    const QString portName = QString::fromLocal8Bit(slaveName);

    pty.slaveFd = ::open(slaveName, O_RDWR | O_NOCTTY | O_NONBLOCK);
    QVERIFY2(pty.slaveFd >= 0,
             qPrintable(QStringLiteral("opening PTY slave failed: %1").arg(QString::fromLocal8Bit(strerror(errno)))));
    termios settings{};
    QVERIFY2(::tcgetattr(pty.slaveFd, &settings) == 0,
             qPrintable(QStringLiteral("tcgetattr failed: %1").arg(QString::fromLocal8Bit(strerror(errno)))));
    ::cfmakeraw(&settings);
    QVERIFY2(::tcsetattr(pty.slaveFd, TCSANOW, &settings) == 0,
             qPrintable(QStringLiteral("tcsetattr failed: %1").arg(QString::fromLocal8Bit(strerror(errno)))));
    ::close(pty.slaveFd);
    pty.slaveFd = -1;

    CommunicationConfig config;
    config.id = pty.configId;
    config.name = QStringLiteral("PTY loopback");
    config.type = CommunicationType::SerialPort;
    config.portName = portName;
    config.baudRate = 115200;
    QVERIFY(manager.addOrUpdateConfig(config));

    QSignalSpy dataSpy(&manager, &CommunicationManager::dataReceived);
    QSignalSpy errorSpy(&manager, &CommunicationManager::errorOccurred);
    QCOMPARE(manager.state(pty.configId), CommunicationState::Disconnected);
    QVERIFY(manager.connect(pty.configId));
    QTRY_VERIFY_WITH_TIMEOUT(manager.isConnected(pty.configId), 1000);
    QCOMPARE(manager.state(pty.configId), CommunicationState::Connected);
    QSerialPort* serial = nullptr;
    const QList<QSerialPort*> serialPorts = manager.findChildren<QSerialPort*>();
    QCOMPARE(serialPorts.size(), 1);
    serial = serialPorts.first();
    QVERIFY(portName.endsWith(serial->portName()));
    QVERIFY(serial->isOpen());
    QVERIFY(serial);
    QPointer<QSerialPort> serialGuard(serial);

    const QByteArray outbound = QByteArray::fromHex("747800017fff");
    QVERIFY(manager.sendData(pty.configId, outbound));
    QByteArray fromSerial;
    QElapsedTimer readDeadline;
    readDeadline.start();
    while (fromSerial.size() < outbound.size() && readDeadline.elapsed() < 1500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        pollfd descriptor{pty.masterFd, POLLIN, 0};
        const int pollResult = ::poll(&descriptor, 1, 10);
        if (pollResult < 0 && errno != EINTR)
            break;
        if (pollResult <= 0 || !(descriptor.revents & POLLIN))
            continue;
        char buffer[64];
        const ssize_t count = ::read(pty.masterFd, buffer, sizeof(buffer));
        if (count > 0)
            fromSerial.append(buffer, static_cast<int>(count));
        else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            break;
    }
    QCOMPARE(fromSerial, outbound);

    const QByteArray inbound = QByteArray::fromHex("7278000280");
    int written = 0;
    QElapsedTimer writeDeadline;
    writeDeadline.start();
    while (written < inbound.size() && writeDeadline.elapsed() < 1500) {
        const ssize_t count =
            ::write(pty.masterFd, inbound.constData() + written, static_cast<size_t>(inbound.size() - written));
        if (count > 0) {
            written += static_cast<int>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{pty.masterFd, POLLOUT, 0};
            ::poll(&descriptor, 1, 10);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            continue;
        }
        break;
    }
    QCOMPARE(written, inbound.size());

    QByteArray fromManager;
    int consumedSignals = 0;
    bool wrongConfig = false;
    const auto collectSerialChunks = [&]() {
        while (consumedSignals < dataSpy.count()) {
            const QList<QVariant> event = dataSpy.at(consumedSignals++);
            if (event.at(0).toString() != pty.configId) {
                wrongConfig = true;
                continue;
            }
            fromManager += event.at(1).toByteArray();
        }
        return fromManager.size() >= inbound.size();
    };
    QTRY_VERIFY_WITH_TIMEOUT(collectSerialChunks(), 1500);
    QVERIFY(!wrongConfig);
    QCOMPARE(fromManager, inbound);
    QCOMPARE(errorSpy.count(), 0);
    manager.disconnect(pty.configId);
    QTRY_VERIFY_WITH_TIMEOUT(!manager.isConnected(pty.configId), 1000);
    QCOMPARE(manager.state(pty.configId), CommunicationState::Disconnected);
    QTRY_VERIFY_WITH_TIMEOUT(serialGuard.isNull(), 1000);
    QCOMPARE(errorSpy.count(), 0);
#endif
}

void TestCommunicationManager::testCommunicationSetViewApplyCreatesConfig() {
    QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
    CommunicationSetView view;

    QLineEdit* nameEdit = view.findChild<QLineEdit*>("CommunicationNameEdit");
    QComboBox* typeCombo = view.findChild<QComboBox*>("CommunicationTypeCombo");
    QLineEdit* ipEdit = view.findChild<QLineEdit*>("CommunicationIpEdit");
    QSpinBox* portSpin = view.findChild<QSpinBox*>("CommunicationPortSpin");

    QVERIFY(nameEdit != nullptr);
    QVERIFY(typeCombo != nullptr);
    QVERIFY(ipEdit != nullptr);
    QVERIFY(portSpin != nullptr);

    nameEdit->setText("Local TCP");
    typeCombo->setCurrentIndex(typeCombo->findData(static_cast<int>(CommunicationType::TCP_Client)));
    ipEdit->setText("127.0.0.1");
    portSpin->setValue(4567);

    QVERIFY(QMetaObject::invokeMethod(&view, "onApplyClicked", Qt::DirectConnection));

    CommunicationConfig* config = CommunicationManager::instance().findConfig("Local TCP");
    QVERIFY(config != nullptr);
    QCOMPARE(config->name, QString("Local TCP"));
    QCOMPARE(config->type, CommunicationType::TCP_Client);
    QCOMPARE(config->ipAddress, QString("127.0.0.1"));
    QCOMPARE(config->port, 4567);
}

void TestCommunicationManager::testCommunicationViewAddsAndDeletesConfigThroughGui() {
    QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
    CommunicationManager& manager = CommunicationManager::instance();
    const QString configName = QStringLiteral("slice26-local-tcp-config");
    {
        CommunicationSetView view;
        auto* table = view.findChild<QTableWidget*>(QStringLiteral("CommunicationTable"));
        auto* nameEdit = view.findChild<QLineEdit*>(QStringLiteral("CommunicationNameEdit"));
        auto* typeCombo = view.findChild<QComboBox*>(QStringLiteral("CommunicationTypeCombo"));
        auto* ipEdit = view.findChild<QLineEdit*>(QStringLiteral("CommunicationIpEdit"));
        auto* portSpin = view.findChild<QSpinBox*>(QStringLiteral("CommunicationPortSpin"));
        auto* addButton = view.findChild<QPushButton*>(QStringLiteral("CommunicationAddButton"));
        auto* applyButton = view.findChild<QPushButton*>(QStringLiteral("CommunicationApplyButton"));
        QVERIFY(table && nameEdit && typeCombo && ipEdit && portSpin && addButton && applyButton);
        QCOMPARE(table->rowCount(), 0);

        view.show();
        addButton->click();
        nameEdit->setText(configName);
        typeCombo->setCurrentIndex(typeCombo->findData(static_cast<int>(CommunicationType::TCP_Client)));
        ipEdit->setText(QStringLiteral("127.0.0.1"));
        portSpin->setValue(4567);
        applyButton->click();

        CommunicationConfig* config = manager.findConfig(configName);
        QVERIFY(config != nullptr);
        QCOMPARE(config->type, CommunicationType::TCP_Client);
        QCOMPARE(config->ipAddress, QStringLiteral("127.0.0.1"));
        QCOMPARE(config->port, 4567);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), configName);
    }

    QVERIFY(manager.findConfig(configName) != nullptr);
    CommunicationSetView reopenedView;
    auto* reopenedTable = reopenedView.findChild<QTableWidget*>(QStringLiteral("CommunicationTable"));
    auto* deleteButton = reopenedView.findChild<QPushButton*>(QStringLiteral("CommunicationDeleteButton"));
    QVERIFY(reopenedTable && deleteButton);
    QCOMPARE(reopenedTable->rowCount(), 1);
    QCOMPARE(reopenedTable->item(0, 0)->text(), configName);

    reopenedView.show();
    reopenedTable->selectRow(0);
    QVERIFY(deleteButton->isEnabled());
    deleteButton->click();

    QVERIFY(manager.findConfig(configName) == nullptr);
    QCOMPARE(reopenedTable->rowCount(), 0);
}

void TestCommunicationManager::testCommunicationViewConnectsAndDisconnectsLoopbackTcp() {
    QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
    QTcpServer loopbackServer;
    QVERIFY(loopbackServer.listen(QHostAddress::LocalHost, 0));

    CommunicationManager& manager = CommunicationManager::instance();
    CommunicationConfig config;
    config.id = QStringLiteral("slice29-loopback-tcp-client");
    config.name = QStringLiteral("Slice 29 loopback TCP client");
    config.type = CommunicationType::TCP_Client;
    config.ipAddress = QStringLiteral("127.0.0.1");
    config.port = loopbackServer.serverPort();
    QVERIFY(manager.addOrUpdateConfig(config));

    CommunicationSetView view;
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("CommunicationTable"));
    auto* connectButton = view.findChild<QPushButton*>(QStringLiteral("CommunicationConnectButton"));
    auto* disconnectButton = view.findChild<QPushButton*>(QStringLiteral("CommunicationDisconnectButton"));
    QVERIFY(table && connectButton && disconnectButton);
    QCOMPARE(table->rowCount(), 1);
    table->selectRow(0);
    QVERIFY(connectButton->isEnabled());
    QVERIFY(!disconnectButton->isEnabled());

    QList<CommunicationState> observedStates;
    QObject stateObserver;
    connect(&manager, &CommunicationManager::connectionStateChanged, &stateObserver,
            [&observedStates, &config](const QString& id, CommunicationState state) {
                if (id == config.id)
                    observedStates.append(state);
            });
    connectButton->click();
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("Connecting"));
    QVERIFY(!connectButton->isEnabled());
    QVERIFY(disconnectButton->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(config.id), CommunicationState::Connected, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 4)->text(), QStringLiteral("Connected"), 3000);
    QVERIFY(!connectButton->isEnabled());
    QVERIFY(disconnectButton->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(loopbackServer.hasPendingConnections(), 1000);
    QTcpSocket* acceptedPeer = loopbackServer.nextPendingConnection();
    QVERIFY(acceptedPeer);

    disconnectButton->click();
    QCOMPARE(manager.state(config.id), CommunicationState::Disconnected);
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("Disconnected"));
    QVERIFY(connectButton->isEnabled());
    QVERIFY(!disconnectButton->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(acceptedPeer->state(), QAbstractSocket::UnconnectedState, 1000);

    QVERIFY(observedStates.contains(CommunicationState::Connecting));
    QVERIFY(observedStates.contains(CommunicationState::Connected));
    QVERIFY(observedStates.contains(CommunicationState::Disconnected));
}

void TestCommunicationManager::testCommunicationSetViewTypeSwitchEnablesRelevantFields() {
    QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
    CommunicationSetView view;

    QComboBox* typeCombo = view.findChild<QComboBox*>("CommunicationTypeCombo");
    QLineEdit* ipEdit = view.findChild<QLineEdit*>("CommunicationIpEdit");
    QSpinBox* portSpin = view.findChild<QSpinBox*>("CommunicationPortSpin");
    QComboBox* portNameCombo = view.findChild<QComboBox*>("CommunicationPortNameCombo");
    QSpinBox* baudRateSpin = view.findChild<QSpinBox*>("CommunicationBaudRateSpin");
    QComboBox* parityCombo = view.findChild<QComboBox*>("CommunicationParityCombo");

    QVERIFY(typeCombo != nullptr);
    QVERIFY(ipEdit != nullptr);
    QVERIFY(portSpin != nullptr);
    QVERIFY(portNameCombo != nullptr);
    QVERIFY(baudRateSpin != nullptr);
    QVERIFY(parityCombo != nullptr);

    typeCombo->setCurrentIndex(typeCombo->findData(static_cast<int>(CommunicationType::TCP_Client)));
    QVERIFY(ipEdit->isEnabled());
    QVERIFY(portSpin->isEnabled());
    QVERIFY(!portNameCombo->isEnabled());
    QVERIFY(!baudRateSpin->isEnabled());
    QVERIFY(!parityCombo->isEnabled());

    typeCombo->setCurrentIndex(typeCombo->findData(static_cast<int>(CommunicationType::SerialPort)));
    QVERIFY(!ipEdit->isEnabled());
    QVERIFY(!portSpin->isEnabled());
    QVERIFY(portNameCombo->isEnabled());
    QVERIFY(baudRateSpin->isEnabled());
    QVERIFY(parityCombo->isEnabled());
}

void TestCommunicationManager::testCommunicationSetViewClearsAndBlocksAfterPermissionRevocation() {
    CommunicationManager& manager = CommunicationManager::instance();
    CommunicationConfig config;
    config.id = QStringLiteral("permission-revocation-config");
    config.name = QStringLiteral("Permission revocation endpoint");
    config.type = CommunicationType::TCP_Client;
    config.ipAddress = QStringLiteral("127.0.0.1");
    config.port = 4567;
    QVERIFY(manager.addOrUpdateConfig(config));

    QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
    CommunicationSetView view;
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("CommunicationTable"));
    auto* nameEdit = view.findChild<QLineEdit*>(QStringLiteral("CommunicationNameEdit"));
    auto* ipEdit = view.findChild<QLineEdit*>(QStringLiteral("CommunicationIpEdit"));
    auto* portSpin = view.findChild<QSpinBox*>(QStringLiteral("CommunicationPortSpin"));
    auto* addButton = view.findChild<QPushButton*>(QStringLiteral("CommunicationAddButton"));
    auto* deleteButton = view.findChild<QPushButton*>(QStringLiteral("CommunicationDeleteButton"));
    auto* connectButton = view.findChild<QPushButton*>(QStringLiteral("CommunicationConnectButton"));
    auto* disconnectButton = view.findChild<QPushButton*>(QStringLiteral("CommunicationDisconnectButton"));
    auto* applyButton = view.findChild<QPushButton*>(QStringLiteral("CommunicationApplyButton"));
    QVERIFY(table);
    QVERIFY(nameEdit);
    QVERIFY(ipEdit);
    QVERIFY(portSpin);
    QVERIFY(addButton);
    QVERIFY(deleteButton);
    QVERIFY(connectButton);
    QVERIFY(disconnectButton);
    QVERIFY(applyButton);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->text(), config.name);
    table->selectRow(0);
    QCOMPARE(portSpin->value(), config.port);
    QVERIFY(manager.state(config.id) == CommunicationState::Disconnected);

    QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(nameEdit->text().isEmpty());
    QVERIFY(ipEdit->text().isEmpty());
    QCOMPARE(portSpin->value(), 0);
    QCOMPARE(portSpin->text(), QStringLiteral("—"));
    QVERIFY(!table->isEnabled());
    QVERIFY(!nameEdit->isEnabled());
    QVERIFY(!ipEdit->isEnabled());
    QVERIFY(!portSpin->isEnabled());
    QVERIFY(!addButton->isEnabled());
    QVERIFY(!deleteButton->isEnabled());
    QVERIFY(!connectButton->isEnabled());
    QVERIFY(!disconnectButton->isEnabled());
    QVERIFY(!applyButton->isEnabled());

    nameEdit->setText(QStringLiteral("must-not-apply"));
    QVERIFY(QMetaObject::invokeMethod(&view, "onApplyClicked", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(&view, "onDeleteClicked", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(&view, "onConnectClicked", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(&view, "onDisconnectClicked", Qt::DirectConnection));
    const CommunicationConfig* retained = manager.findConfig(config.id);
    QVERIFY(retained);
    QCOMPARE(retained->name, config.name);
    QCOMPARE(retained->ipAddress, config.ipAddress);
    QCOMPARE(retained->port, config.port);
    QCOMPARE(manager.state(config.id), CommunicationState::Disconnected);

    QVERIFY(manager.removeConfig(config.id));
    for (const QString& user :
         {QStringLiteral("administrator"), QStringLiteral("operator"), QStringLiteral("guest-session")}) {
        QVERIFY(UserManager::instance().login(user, QString()));
        CommunicationSetView deniedView;
        auto* deniedTable = deniedView.findChild<QTableWidget*>(QStringLiteral("CommunicationTable"));
        auto* permissionMessage = deniedView.findChild<QLabel*>(QStringLiteral("CommunicationPermissionMessage"));
        QVERIFY(deniedTable);
        QVERIFY(permissionMessage);
        QCOMPARE(deniedTable->rowCount(), 0);
        QVERIFY(!deniedTable->isEnabled());
        QVERIFY(!permissionMessage->isHidden());
    }
    UserManager::instance().logout();
    CommunicationSetView loggedOutView;
    QVERIFY(!loggedOutView.findChild<QTableWidget*>(QStringLiteral("CommunicationTable"))->isEnabled());
}

QTEST_MAIN(TestCommunicationManager)
#include "test_communicationmanager.moc"
