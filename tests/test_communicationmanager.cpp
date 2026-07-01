#include <QtTest/QtTest>

#include <core/communication/CommunicationManager.h>
#include <ui/views/CommunicationSetView.h>

#include <QComboBox>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>

using namespace DeepLux;

class TestCommunicationManager : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void testAddUpdateRemoveConfig();
    void testPlcConfigUsesTcpTransport();
    void testCommunicationSetViewApplyCreatesConfig();
    void testCommunicationSetViewTypeSwitchEnablesRelevantFields();
};

void TestCommunicationManager::init()
{
    CommunicationManager::instance().disconnectAll();
    CommunicationManager::instance().clearConfigs();
}

void TestCommunicationManager::cleanup()
{
    CommunicationManager::instance().disconnectAll();
    CommunicationManager::instance().clearConfigs();
}

void TestCommunicationManager::testAddUpdateRemoveConfig()
{
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

void TestCommunicationManager::testPlcConfigUsesTcpTransport()
{
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
    QTRY_VERIFY_WITH_TIMEOUT(CommunicationManager::instance().isConnected("plc1"), 1000);
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

void TestCommunicationManager::testCommunicationSetViewApplyCreatesConfig()
{
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

void TestCommunicationManager::testCommunicationSetViewTypeSwitchEnablesRelevantFields()
{
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

QTEST_MAIN(TestCommunicationManager)
#include "test_communicationmanager.moc"
