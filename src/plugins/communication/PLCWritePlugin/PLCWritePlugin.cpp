#include "PLCWritePlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QJsonArray>

namespace DeepLux {

PLCWritePlugin::PLCWritePlugin(QObject* parent)
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
        {"values", QJsonArray{0}},
        {"valueSource", "immediate"},
        {"timeout", 3000}
    };
    m_params = m_defaultParams;

    connect(m_socket, &QTcpSocket::connected, this, &PLCWritePlugin::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &PLCWritePlugin::onDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &PLCWritePlugin::onError);
    connect(m_socket, &QTcpSocket::readyRead, this, &PLCWritePlugin::onReadyRead);
}

PLCWritePlugin::~PLCWritePlugin()
{
    disconnectFromHost();
}

bool PLCWritePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "PLCWritePlugin initialized";
    return true;
}

void PLCWritePlugin::onConnected()
{
    Logger::instance().debug("PLCWrite: Connected to PLC", "Communication");
    if (m_connectLoop) {
        m_connectLoop->quit();
    }
}

void PLCWritePlugin::onDisconnected()
{
    Logger::instance().debug("PLCWrite: Disconnected from PLC", "Communication");
}

void PLCWritePlugin::onError(QAbstractSocket::SocketError error)
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

void PLCWritePlugin::onReadyRead()
{
    m_readBuffer += m_socket->readAll();
    if (m_readLoop) {
        m_readLoop->quit();
    }
}

void PLCWritePlugin::onConnectTimeout()
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        m_socket->abort();
        emit errorOccurred(tr("连接PLC %1:%2 超时").arg(m_host).arg(m_port));
    }
    if (m_connectLoop) {
        m_connectLoop->quit();
    }
}

void PLCWritePlugin::onReadTimeout()
{
    m_readTimedOut = true;
    m_socket->disconnectFromHost();
    if (m_readLoop) {
        m_readLoop->quit();
    }
}

bool PLCWritePlugin::connectToHost()
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
    connect(&timer, &QTimer::timeout, this, &PLCWritePlugin::onConnectTimeout);

    loop.exec();
    m_connectLoop = nullptr;
    m_lastWaitMs = m_timeout - timer.remainingTime();

    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    Logger::instance().info(QString("PLCWrite: Connected to %1:%2").arg(m_host).arg(m_port), "Communication");
    return true;
}

void PLCWritePlugin::disconnectFromHost()
{
    if (m_socket->state() == QAbstractSocket::ConnectedState ||
        m_socket->state() == QAbstractSocket::ConnectingState) {
        m_socket->abort();
    }
}

QByteArray PLCWritePlugin::buildWriteRequest(quint16 transactionId, quint8 unitId,
                                              quint16 startAddress, const QVector<quint16>& values)
{
    QByteArray request;
    QDataStream stream(&request, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    quint8 byteCount = static_cast<quint8>(values.size() * 2);
    quint16 length = 7 + byteCount; // 1 (unit) + 1 (func) + 2 (addr) + 2 (qty) + 1 (bytecount) + byteCount

    // MBAP Header
    stream << transactionId;        // Transaction ID
    stream << quint16(0);           // Protocol ID
    stream << length;               // Length
    stream << unitId;               // Unit ID

    // Function Code 16: Write Multiple Registers
    stream << quint8(0x10);         // Function Code
    stream << startAddress;         // Starting Address
    stream << static_cast<quint16>(values.size()); // Quantity of Registers
    stream << byteCount;            // Byte Count

    // Register values
    for (quint16 value : values) {
        stream << value;
    }

    return request;
}

bool PLCWritePlugin::parseWriteResponse(const QByteArray& data)
{
    if (data.size() < 12) {
        return false;
    }

    // Check function code response (should echo the request)
    // MBAP (7) + Function Code (1) + Starting Address (2) + Quantity (2) = 12 bytes
    if (static_cast<quint8>(data[7]) != 0x10) {
        return false;
    }

    return true;
}

bool PLCWritePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_host = m_params["ipAddress"].toString();
    m_port = m_params["port"].toInt(502);
    m_slaveId = static_cast<quint8>(m_params["slaveId"].toInt(1));
    m_startAddress = static_cast<quint16>(m_params["startAddress"].toInt(0));
    m_valueSource = m_params["valueSource"].toString("immediate");
    m_timeout = m_params["timeout"].toInt(3000);

    // Parse values
    m_values.clear();
    QJsonArray valuesArray = m_params["values"].toArray();
    if (valuesArray.isEmpty()) {
        valuesArray = QJsonArray{0};
    }

    if (m_valueSource == "variable") {
        QString varName = m_params["values"].toString();
        if (!varName.isEmpty() && varName != "values") {
            QVariant varVal = input.data(varName);
            if (varVal.isValid()) {
                if (varVal.canConvert<QJsonArray>()) {
                    QJsonArray arr = varVal.toJsonArray();
                    for (const QJsonValue& v : arr) {
                        m_values.append(static_cast<quint16>(v.toInt(0)));
                    }
                } else {
                    m_values.append(static_cast<quint16>(varVal.toInt(0)));
                }
            }
        }
    }

    if (m_values.isEmpty()) {
        for (const QJsonValue& v : valuesArray) {
            m_values.append(static_cast<quint16>(v.toInt(0)));
        }
    }

    if (m_host.isEmpty()) {
        emit errorOccurred(tr("IP地址不能为空"));
        return false;
    }

    if (m_values.isEmpty()) {
        emit errorOccurred(tr("写入值不能为空"));
        return false;
    }

    if (!connectToHost()) {
        return false;
    }

    // Build and send Modbus request
    QByteArray request = buildWriteRequest(++m_transactionId, m_slaveId, m_startAddress, m_values);
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
    connect(&timer, &QTimer::timeout, this, &PLCWritePlugin::onReadTimeout);

    loop.exec();
    m_readLoop = nullptr;
    m_lastWaitMs = m_timeout - timer.remainingTime();

    disconnectFromHost();

    if (m_readTimedOut) {
        emit errorOccurred(tr("写入PLC数据超时"));
        return false;
    }

    if (m_readBuffer.isEmpty()) {
        emit errorOccurred(tr("PLC无响应数据"));
        return false;
    }

    if (!parseWriteResponse(m_readBuffer)) {
        emit errorOccurred(tr("解析PLC响应失败"));
        return false;
    }

    output.setData("plc_write_success", true);
    output.setData("plc_write_count", m_values.size());

    Logger::instance().debug(QString("PLCWrite: Wrote %1 registers to address %2")
        .arg(m_values.size()).arg(m_startAddress), "Communication");

    return true;
}

bool PLCWritePlugin::doValidateParams(const QJsonObject& params, QString& error) const
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
    if (params["startAddress"].toInt() < 0 || params["startAddress"].toInt() > 65535) {
        error = QString("起始地址必须在0-65535之间");
        return false;
    }
    return true;
}

QWidget* PLCWritePlugin::createConfigWidget()
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

    QComboBox* sourceCombo = new QComboBox();
    sourceCombo->addItem(tr("立即值"), "immediate");
    sourceCombo->addItem(tr("变量"), "variable");

    QLineEdit* valuesEdit = new QLineEdit();
    QJsonArray valuesArray = m_params["values"].toArray();
    QStringList valueStrList;
    for (const QJsonValue& v : valuesArray) {
        valueStrList.append(QString::number(v.toInt(0)));
    }
    valuesEdit->setPlaceholderText("例如: 100,200,300 (逗号分隔)");
    valuesEdit->setText(valueStrList.join(","));

    formLayout->addRow(tr("IP地址:"), ipEdit);
    formLayout->addRow(tr("端口:"), portSpin);
    formLayout->addRow(tr("从站ID:"), slaveSpin);
    formLayout->addRow(tr("起始地址:"), addrSpin);
    formLayout->addRow(tr("值来源:"), sourceCombo);
    formLayout->addRow(tr("写入值:"), valuesEdit);

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

    connect(sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["valueSource"] = sourceCombo->currentData().toString();
    });

    connect(valuesEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        if (sourceCombo->currentData().toString() == "immediate") {
            QStringList parts = text.split(",", Qt::SkipEmptyParts);
            QJsonArray arr;
            for (const QString& p : parts) {
                arr.append(p.trimmed().toInt());
            }
            m_params["values"] = arr;
        } else {
            m_params["values"] = text.trimmed();
        }
    });

    return widget;
}

} // namespace DeepLux
