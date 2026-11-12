#include "PLCReadPlugin.h"

#include "core/common/Logger.h"

#include <QElapsedTimer>
#include <QFormLayout>
#include <QJsonArray>
#include <QLineEdit>
#include <QSpinBox>
#include <QTcpSocket>
#include <QThread>
#include <QVBoxLayout>
#include <QtEndian>

namespace DeepLux {

PLCReadPlugin::PLCReadPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{
        {"ipAddress", "192.168.1.100"}, {"port", 502},    {"slaveId", 1}, {"startAddress", 0}, {"readCount", 10},
        {"outputVariable", "plc_data"}, {"timeout", 3000}};
    m_params = m_defaultParams;
}

PLCReadPlugin::~PLCReadPlugin() = default;

bool PLCReadPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "PLCReadPlugin initialized";
    return true;
}

QByteArray PLCReadPlugin::buildReadRequest(quint16 transactionId, quint8 unitId, quint16 startAddress,
                                           quint16 readCount) {
    QByteArray request;
    QDataStream stream(&request, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    // MBAP Header (7 bytes)
    stream << transactionId; // Transaction ID (2 bytes)
    stream << quint16(0);    // Protocol ID (2 bytes) - 0 for Modbus
    stream << quint16(6);    // Length (2 bytes) - remaining bytes after this
    stream << unitId;        // Unit ID (1 byte)

    // Function Code 03: Read Holding Registers
    stream << quint8(0x03); // Function Code
    stream << startAddress; // Starting Address (2 bytes)
    stream << readCount;    // Quantity of Registers (2 bytes)

    return request;
}

QVector<quint16> PLCReadPlugin::parseReadResponse(const QByteArray& data) {
    QVector<quint16> registers;

    if (data.size() < 9) {
        return registers;
    }

    QDataStream stream(data);
    stream.setByteOrder(QDataStream::BigEndian);
    quint16 transactionId = 0;
    quint16 protocolId = 0;
    quint16 length = 0;
    quint8 unitId = 0;
    quint8 function = 0;
    quint8 byteCount = 0;
    stream >> transactionId >> protocolId >> length >> unitId >> function >> byteCount;

    if (stream.status() != QDataStream::Ok || transactionId != m_transactionId || protocolId != 0 ||
        unitId != m_slaveId || function != 0x03 || data.size() != 6 + length || length != 3 + byteCount ||
        byteCount != m_readCount * 2) {
        return registers;
    }

    for (int i = 0; i < byteCount / 2; ++i) {
        quint16 value = 0;
        stream >> value;
        if (stream.status() != QDataStream::Ok)
            return {};
        registers.append(value);
    }

    return registers;
}

bool PLCReadPlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    const QString host = m_params["ipAddress"].toString();
    const quint16 port = static_cast<quint16>(m_params["port"].toInt(502));
    m_slaveId = static_cast<quint8>(m_params["slaveId"].toInt(1));
    const quint16 startAddress = static_cast<quint16>(m_params["startAddress"].toInt(0));
    m_readCount = static_cast<quint16>(m_params["readCount"].toInt(10));
    const QString outputVariable = m_params["outputVariable"].toString("plc_data");
    const int timeout = m_params["timeout"].toInt(3000);

    if (host.isEmpty()) {
        emit errorOccurred(tr("IP地址不能为空"));
        return false;
    }

    QTcpSocket socket;
    Q_ASSERT_X(socket.thread() == QThread::currentThread(), "PLCReadPlugin::process",
               "PLCRead socket must be created in its execution thread");
    QElapsedTimer connectTimer;
    connectTimer.start();
    socket.connectToHost(host, port);
    if (!socket.waitForConnected(timeout)) {
        m_lastWaitMs = static_cast<int>(connectTimer.elapsed());
        if (socket.error() == QAbstractSocket::SocketTimeoutError)
            emit errorOccurred(tr("连接PLC %1:%2 超时").arg(host).arg(port));
        else
            emit errorOccurred(tr("PLC通信错误: %1").arg(socket.errorString()));
        return false;
    }
    m_lastWaitMs = static_cast<int>(connectTimer.elapsed());
    Logger::instance().info(QString("PLCRead: Connected to %1:%2").arg(host).arg(port), "Communication");

    // Build and send Modbus request
    const QByteArray request = buildReadRequest(++m_transactionId, m_slaveId, startAddress, m_readCount);
    if (socket.write(request) != request.size()) {
        emit errorOccurred(tr("发送Modbus请求失败"));
        return false;
    }
    socket.flush();

    // Wait for response
    QElapsedTimer readTimer;
    readTimer.start();
    QByteArray readBuffer;
    bool completeFrame = false;
    while (!completeFrame && readTimer.elapsed() < timeout) {
        const int remaining = qMax(1, timeout - static_cast<int>(readTimer.elapsed()));
        if (!socket.waitForReadyRead(remaining))
            break;
        readBuffer += socket.readAll();
        if (readBuffer.size() >= 6) {
            const auto* bytes = reinterpret_cast<const uchar*>(readBuffer.constData());
            const int frameSize = 6 + qFromBigEndian<quint16>(bytes + 4);
            completeFrame = frameSize <= 6 || readBuffer.size() >= frameSize;
        }
    }
    m_lastWaitMs = static_cast<int>(readTimer.elapsed());

    if (!completeFrame && readBuffer.isEmpty()) {
        emit errorOccurred(tr("读取PLC数据超时"));
        return false;
    }

    // Parse response
    QVector<quint16> registers = parseReadResponse(readBuffer);
    if (registers.isEmpty()) {
        emit errorOccurred(tr("解析PLC响应失败"));
        return false;
    }

    // Store registers as JSON array in output
    QJsonArray registerArray;
    for (quint16 reg : registers) {
        registerArray.append(static_cast<int>(reg));
    }

    output.setData(outputVariable, registerArray);
    output.setData(outputVariable + "_count", registers.size());

    Logger::instance().debug(
        QString("PLCRead: Read %1 registers from address %2").arg(registers.size()).arg(startAddress), "Communication");

    return true;
}

bool PLCReadPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
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

QWidget* PLCReadPlugin::createConfigWidget() {
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

    connect(ipEdit, &QLineEdit::textChanged, this, [=](const QString& text) { m_params["ipAddress"] = text; });

    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) { m_params["port"] = value; });

    connect(slaveSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { m_params["slaveId"] = value; });

    connect(addrSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { m_params["startAddress"] = value; });

    connect(countSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { m_params["readCount"] = value; });

    connect(varEdit, &QLineEdit::textChanged, this, [=](const QString& text) { m_params["outputVariable"] = text; });

    return widget;
}

IModule* PLCReadPlugin::cloneImpl() const {
    return new PLCReadPlugin();
}

} // namespace DeepLux
