#include "PLCReadPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QJsonArray>

namespace DeepLux {

PLCReadPlugin::PLCReadPlugin(QObject* parent)
    : ModuleBase(parent)
    , m_socket(new QTcpSocket(this))
    , m_connectLoop(nullptr)
    , m_readLoop(nullptr)
{
    m_defaultParams = QJsonObject{
        {"ipAddress", "192.168.1.100"},
        {"port", 502},
        {"slaveId", 1},
        {"startAddress", 0},
        {"readCount", 10},
        {"outputVariable", "plc_data"},
        {"timeout", 3000}
    };
    m_params = m_defaultParams;

    connect(m_socket, &QTcpSocket::connected, this, &PLCReadPlugin::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &PLCReadPlugin::onDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &PLCReadPlugin::onError);
    connect(m_socket, &QTcpSocket::readyRead, this, &PLCReadPlugin::onReadyRead);
}

PLCReadPlugin::~PLCReadPlugin()
{
    disconnectFromHost();
}

bool PLCReadPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "PLCReadPlugin initialized";
    return true;
}

void PLCReadPlugin::onConnected()
{
    Logger::instance().debug("PLCRead: Connected to PLC", "Communication");
    if (m_connectLoop) {
        m_connectLoop->quit();
    }
}

void PLCReadPlugin::onDisconnected()
{
    Logger::instance().debug("PLCRead: Disconnected from PLC", "Communication");
}

void PLCReadPlugin::onError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    emit errorOccurred(tr("PLC通信错误: %1").arg(m_socket->errorString()));
    if (m_connectLoop && m_connectLoop->isRunning()) {
        m_connectLoop->quit();
    }
    if (m_readLoop && m_readLoop->isRunning()) {
        m_readLoop->quit();
    }
}

void PLCReadPlugin::onReadyRead()
{
    m_readBuffer += m_socket->readAll();
    if (m_readLoop) {
        m_readLoop->quit();
    }
}

void PLCReadPlugin::onConnectTimeout()
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        m_socket->abort();
        emit errorOccurred(tr("连接PLC %1:%2 超时").arg(m_host).arg(m_port));
    }
    if (m_connectLoop) {
        m_connectLoop->quit();
    }
}

void PLCReadPlugin::onReadTimeout()
{
    m_readTimedOut = true;
    m_socket->disconnectFromHost();
    if (m_readLoop) {
        m_readLoop->quit();
    }
}

bool PLCReadPlugin::connectToHost()
{
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->disconnectFromHost();
        m_socket->waitForDisconnected(100);
    }

    m_socket->connectToHost(m_host, m_port);

    QEventLoop loop;
    m_connectLoop = &loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(m_timeout);
    connect(&timer, &QTimer::timeout, this, &PLCReadPlugin::onConnectTimeout);

    loop.exec();
    m_connectLoop = nullptr;
    m_lastWaitMs = m_timeout - timer.remainingTime();

    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    Logger::instance().info(QString("PLCRead: Connected to %1:%2").arg(m_host).arg(m_port), "Communication");
    return true;
}

void PLCReadPlugin::disconnectFromHost()
{
    if (m_socket->state() == QAbstractSocket::ConnectedState ||
        m_socket->state() == QAbstractSocket::ConnectingState) {
        m_socket->abort();
    }
}

QByteArray PLCReadPlugin::buildReadRequest(quint16 transactionId, quint8 unitId,
                                            quint16 startAddress, quint16 readCount)
{
    QByteArray request;
    QDataStream stream(&request, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    // MBAP Header (7 bytes)
    stream << transactionId;        // Transaction ID (2 bytes)
    stream << quint16(0);           // Protocol ID (2 bytes) - 0 for Modbus
    stream << quint16(6);           // Length (2 bytes) - remaining bytes after this
    stream << unitId;               // Unit ID (1 byte)

    // Function Code 03: Read Holding Registers
    stream << quint8(0x03);         // Function Code
    stream << startAddress;         // Starting Address (2 bytes)
    stream << readCount;            // Quantity of Registers (2 bytes)

    return request;
}

QVector<quint16> PLCReadPlugin::parseReadResponse(const QByteArray& data)
{
    QVector<quint16> registers;

    if (data.size() < 9) {
        return registers;
    }

    // Skip MBAP header (7 bytes) + Function Code (1 byte)
    quint8 byteCount = static_cast<quint8>(data[8]);

    if (data.size() < 9 + byteCount) {
        return registers;
    }

    // Read register values
    for (int i = 0; i < byteCount; i += 2) {
        quint16 value = (static_cast<quint8>(data[9 + i]) << 8) |
                        (static_cast<quint8>(data[10 + i]));
        registers.append(value);
    }

    return registers;
}

bool PLCReadPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_host = m_params["ipAddress"].toString();
    m_port = m_params["port"].toInt(502);
    m_slaveId = static_cast<quint8>(m_params["slaveId"].toInt(1));
    m_startAddress = static_cast<quint16>(m_params["startAddress"].toInt(0));
    m_readCount = static_cast<quint16>(m_params["readCount"].toInt(10));
    m_outputVariable = m_params["outputVariable"].toString("plc_data");
    m_timeout = m_params["timeout"].toInt(3000);

    if (m_host.isEmpty()) {
        emit errorOccurred(tr("IP地址不能为空"));
        return false;
    }

    if (!connectToHost()) {
        return false;
    }

    // Build and send Modbus request
    QByteArray request = buildReadRequest(++m_transactionId, m_slaveId, m_startAddress, m_readCount);
    qint64 written = m_socket->write(request);
    m_socket->flush();

    if (written != request.size()) {
        emit errorOccurred(tr("发送Modbus请求失败"));
        disconnectFromHost();
        return false;
    }

    // Wait for response
    m_readBuffer.clear();
    m_readTimedOut = false;

    QEventLoop loop;
    m_readLoop = &loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(m_timeout);
    connect(&timer, &QTimer::timeout, this, &PLCReadPlugin::onReadTimeout);

    loop.exec();
    m_readLoop = nullptr;
    m_lastWaitMs = m_timeout - timer.remainingTime();

    disconnectFromHost();

    if (m_readTimedOut) {
        emit errorOccurred(tr("读取PLC数据超时"));
        return false;
    }

    if (m_readBuffer.isEmpty()) {
        emit errorOccurred(tr("PLC无响应数据"));
        return false;
    }

    // Parse response
    QVector<quint16> registers = parseReadResponse(m_readBuffer);
    if (registers.isEmpty()) {
        emit errorOccurred(tr("解析PLC响应失败"));
        return false;
    }

    // Store registers as JSON array in output
    QJsonArray registerArray;
    for (quint16 reg : registers) {
        registerArray.append(static_cast<int>(reg));
    }

    output.setData(m_outputVariable, registerArray);
    output.setData(m_outputVariable + "_count", registers.size());

    Logger::instance().debug(QString("PLCRead: Read %1 registers from address %2")
        .arg(registers.size()).arg(m_startAddress), "Communication");

    return true;
}

bool PLCReadPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["ipAddress"].toString().isEmpty()) {
        error = QString("IP地址不能为空");
        return false;
    }
    if (params["port"].toInt() <= 0 || params["port"].toInt() > 65535) {
        error = QString("端口必须在1-65535之间");
        return false;
    }
    if (params["slaveId"].toInt() <= 0 || params["slaveId"].toInt() > 255) {
        error = QString("从站ID必须在1-255之间");
        return false;
    }
    if (params["readCount"].toInt() <= 0 || params["readCount"].toInt() > 125) {
        error = QString("读取数量必须在1-125之间");
        return false;
    }
    return true;
}

QWidget* PLCReadPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* ipEdit = new QLineEdit(m_params["ipAddress"].toString());

    QSpinBox* portSpin = new QSpinBox();
    portSpin->setRange(1, 65535);
    portSpin->setValue(m_params["port"].toInt(502));
    portSpin->setPrefix(tr("端口: "));

    QSpinBox* slaveSpin = new QSpinBox();
    slaveSpin->setRange(1, 255);
    slaveSpin->setValue(m_params["slaveId"].toInt(1));
    slaveSpin->setPrefix(tr("从站ID: "));

    QSpinBox* addrSpin = new QSpinBox();
    addrSpin->setRange(0, 65535);
    addrSpin->setValue(m_params["startAddress"].toInt(0));
    addrSpin->setPrefix(tr("起始地址: "));

    QSpinBox* countSpin = new QSpinBox();
    countSpin->setRange(1, 125);
    countSpin->setValue(m_params["readCount"].toInt(10));
    countSpin->setPrefix(tr("读取数量: "));

    QLineEdit* varEdit = new QLineEdit(m_params["outputVariable"].toString("plc_data"));

    formLayout->addRow(tr("IP地址:"), ipEdit);
    formLayout->addRow(tr("端口:"), portSpin);
    formLayout->addRow(tr("从站ID:"), slaveSpin);
    formLayout->addRow(tr("起始地址:"), addrSpin);
    formLayout->addRow(tr("读取数量:"), countSpin);
    formLayout->addRow(tr("输出变量:"), varEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(ipEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["ipAddress"] = text;
    });

    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["port"] = value;
    });

    connect(slaveSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["slaveId"] = value;
    });

    connect(addrSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["startAddress"] = value;
    });

    connect(countSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["readCount"] = value;
    });

    connect(varEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["outputVariable"] = text;
    });

    return widget;
}

} // namespace DeepLux
