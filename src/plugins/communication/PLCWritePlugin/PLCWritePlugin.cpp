#include "PLCWritePlugin.h"

#include "core/common/Logger.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFormLayout>
#include <QJsonArray>
#include <QLineEdit>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QtEndian>
#include <cmath>

namespace DeepLux {

PLCWritePlugin::PLCWritePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"ipAddress", "192.168.1.100"},
                                  {"port", 502},
                                  {"slaveId", 1},
                                  {"startAddress", 0},
                                  {"values", QJsonArray{0}},
                                  {"valueSource", "immediate"},
                                  {"timeout", 3000}};
    m_params = m_defaultParams;
}

PLCWritePlugin::~PLCWritePlugin() = default;

bool PLCWritePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "PLCWritePlugin initialized";
    return true;
}

bool PLCWritePlugin::connectToHost(QTcpSocket& socket) {
    Q_ASSERT(socket.thread() == QThread::currentThread());
    QElapsedTimer connectTimer;
    connectTimer.start();
    socket.connectToHost(m_host, m_port);
    if (socket.state() != QAbstractSocket::ConnectedState) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&socket, &QTcpSocket::connected, &loop, &QEventLoop::quit);
        connect(&socket, &QTcpSocket::errorOccurred, &loop, &QEventLoop::quit);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(m_timeout);
        loop.exec();
    }
    m_lastWaitMs = static_cast<int>(connectTimer.elapsed());
    if (socket.state() != QAbstractSocket::ConnectedState) {
        emit errorOccurred(tr("连接PLC %1:%2 失败: %3").arg(m_host).arg(m_port).arg(socket.errorString()));
        socket.abort();
        return false;
    }
    Logger::instance().info(QString("PLCWrite: Connected to %1:%2").arg(m_host).arg(m_port), "Communication");
    return true;
}

void PLCWritePlugin::disconnectFromHost(QTcpSocket& socket) {
    Q_ASSERT(socket.thread() == QThread::currentThread());
    if (socket.state() == QAbstractSocket::ConnectedState || socket.state() == QAbstractSocket::ConnectingState)
        socket.abort();
}

QByteArray PLCWritePlugin::buildWriteRequest(quint16 transactionId, quint8 unitId, quint16 startAddress,
                                             const QVector<quint16>& values) {
    QByteArray request;
    QDataStream stream(&request, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    quint8 byteCount = static_cast<quint8>(values.size() * 2);
    quint16 length = 7 + byteCount; // 1 (unit) + 1 (func) + 2 (addr) + 2 (qty) + 1 (bytecount) + byteCount

    // MBAP Header
    stream << transactionId; // Transaction ID
    stream << quint16(0);    // Protocol ID
    stream << length;        // Length
    stream << unitId;        // Unit ID

    // Function Code 16: Write Multiple Registers
    stream << quint8(0x10);                        // Function Code
    stream << startAddress;                        // Starting Address
    stream << static_cast<quint16>(values.size()); // Quantity of Registers
    stream << byteCount;                           // Byte Count

    // Register values
    for (quint16 value : values) {
        stream << value;
    }

    return request;
}

ExecutionResult PLCWritePlugin::execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) {
    m_writeRequestAccepted = false;
    const ExecutionResult result = ModuleBase::execute(inputs, outputs, context);
    if (!result.success && m_writeRequestAccepted) {
        return ExecutionResult::fail(ExecError::SideEffectUncertain,
                                     tr("PLC 写入结果无法确认：请求已交给 socket，但未收到有效成功回显"),
                                     result.diagnostics);
    }
    return result;
}

bool PLCWritePlugin::parseWriteResponse(const QByteArray& data) {
    if (data.size() != 12) {
        return false;
    }

    QDataStream stream(data);
    stream.setByteOrder(QDataStream::BigEndian);
    quint16 transactionId = 0;
    quint16 protocolId = 0;
    quint16 length = 0;
    quint8 unitId = 0;
    quint8 function = 0;
    quint16 startAddress = 0;
    quint16 quantity = 0;
    stream >> transactionId >> protocolId >> length >> unitId >> function >> startAddress >> quantity;

    return stream.status() == QDataStream::Ok && transactionId == m_transactionId && protocolId == 0 && length == 6 &&
           unitId == m_slaveId && function == 0x10 && startAddress == m_startAddress && quantity == m_values.size();
}

bool PLCWritePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    m_host = m_params["ipAddress"].toString();
    m_port = m_params["port"].toInt(502);
    m_slaveId = static_cast<quint8>(m_params["slaveId"].toInt(1));
    const QJsonValue startAddress = m_params.value(QStringLiteral("startAddress"));
    const double startAddressNumber = startAddress.toDouble();
    if (!startAddress.isDouble() || !std::isfinite(startAddressNumber) ||
        std::floor(startAddressNumber) != startAddressNumber || startAddressNumber < 0 || startAddressNumber > 65535) {
        emit errorOccurred(tr("起始地址必须是 0 到 65535 的整数"));
        return false;
    }
    m_startAddress = static_cast<quint16>(startAddressNumber);
    m_valueSource = m_params["valueSource"].toString("immediate");
    m_timeout = m_params["timeout"].toInt(3000);

    // Parse values
    m_values.clear();
    if (m_valueSource == "variable") {
        const QString varName = m_params["values"].toString().trimmed();
        const QVariant value = input.data(varName);
        if (!value.isValid()) {
            emit errorOccurred(tr("写入变量不存在: %1").arg(varName));
            return false;
        }

        const auto appendRegister = [this](double number) {
            if (!std::isfinite(number) || std::floor(number) != number || number < 0 || number > 65535)
                return false;
            m_values.append(static_cast<quint16>(number));
            return true;
        };
        if (value.userType() == qMetaTypeId<QJsonArray>()) {
            const QJsonArray values = value.toJsonArray();
            if (values.isEmpty() || values.size() > 123) {
                emit errorOccurred(tr("写入变量数组必须包含 1 到 123 个寄存器"));
                return false;
            }
            for (const QJsonValue& item : values) {
                if (!item.isDouble() || !appendRegister(item.toDouble())) {
                    emit errorOccurred(tr("写入变量值必须是 0 到 65535 的有限整数"));
                    return false;
                }
            }
        } else {
            const int type = value.userType();
            const bool numericType = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong ||
                                     type == QMetaType::ULongLong || type == QMetaType::Float ||
                                     type == QMetaType::Double;
            bool ok = false;
            const double number = numericType ? value.toDouble(&ok) : 0;
            if (!numericType || !ok || !appendRegister(number)) {
                emit errorOccurred(tr("写入变量值必须是 0 到 65535 的有限整数"));
                return false;
            }
        }
    } else {
        for (const QJsonValue& value : m_params["values"].toArray())
            m_values.append(static_cast<quint16>(value.toInt()));
    }

    if (m_host.isEmpty()) {
        emit errorOccurred(tr("IP地址不能为空"));
        return false;
    }

    if (m_values.isEmpty() || m_values.size() > 123 || static_cast<int>(m_startAddress) + m_values.size() > 65536) {
        emit errorOccurred(tr("写入寄存器数量或地址范围无效"));
        return false;
    }

    // The RunEngine may execute this module on a worker thread. Construct and keep the
    // event-driven socket on this call's thread rather than using a module-owned socket.
    QTcpSocket socket;
    Q_ASSERT(socket.thread() == QThread::currentThread());
    if (!connectToHost(socket)) {
        return false;
    }

    // Build and send Modbus request
    QByteArray request = buildWriteRequest(++m_transactionId, m_slaveId, m_startAddress, m_values);
    const qint64 written = socket.write(request);
    socket.flush();

    m_writeRequestAccepted = written == request.size();
    if (written != request.size()) {
        emit errorOccurred(tr("发送Modbus请求失败"));
        disconnectFromHost(socket);
        return false;
    }

    // Wait for a complete MBAP frame using a loop on the same thread that owns this socket.
    // This also keeps local fake peers responsive when a synchronous step runs on the GUI thread.
    m_readBuffer.clear();
    QElapsedTimer responseTimer;
    responseTimer.start();
    bool responseTimedOut = false;
    int expectedFrameSize = -1;
    while (expectedFrameSize < 0 || m_readBuffer.size() < expectedFrameSize) {
        if (socket.bytesAvailable() == 0) {
            const int remainingMs = m_timeout - static_cast<int>(responseTimer.elapsed());
            if (remainingMs <= 0) {
                responseTimedOut = responseTimer.elapsed() >= m_timeout;
                break;
            }
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            connect(&socket, &QTcpSocket::readyRead, &loop, &QEventLoop::quit);
            connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
            connect(&socket, &QTcpSocket::errorOccurred, &loop, &QEventLoop::quit);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timer.start(remainingMs);
            loop.exec();
            if (socket.bytesAvailable() == 0) {
                responseTimedOut = responseTimer.elapsed() >= m_timeout;
                break;
            }
        }
        m_readBuffer += socket.readAll();
        if (expectedFrameSize < 0 && m_readBuffer.size() >= 6) {
            const auto* bytes = reinterpret_cast<const uchar*>(m_readBuffer.constData());
            const quint16 mbapLength = qFromBigEndian<quint16>(bytes + 4);
            if (mbapLength == 0 || mbapLength > 260)
                break;
            expectedFrameSize = 6 + mbapLength;
        }
    }
    m_lastWaitMs = static_cast<int>(responseTimer.elapsed());

    disconnectFromHost(socket);

    if (responseTimedOut) {
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

    Logger::instance().debug(
        QString("PLCWrite: Wrote %1 registers to address %2").arg(m_values.size()).arg(m_startAddress),
        "Communication");

    return true;
}

bool PLCWritePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    if (!params["ipAddress"].isString() || params["ipAddress"].toString().trimmed().isEmpty()) {
        error = QString("IP地址不能为空");
        return false;
    }
    const auto integerInRange = [&params, &error](const QString& key, int low, int high) {
        const QJsonValue value = params.value(key);
        const double number = value.toDouble();
        if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number || number < low ||
            number > high) {
            error = QString("%1 must be an integer in [%2,%3]").arg(key).arg(low).arg(high);
            return false;
        }
        return true;
    };
    if (!integerInRange("port", 1, 65535) || !integerInRange("slaveId", 1, 255) ||
        !integerInRange("startAddress", 0, 65535) || !integerInRange("timeout", 1, 120000))
        return false;
    const QString source = params.value("valueSource").toString();
    if (source != QStringLiteral("immediate") && source != QStringLiteral("variable")) {
        error = QStringLiteral("valueSource must be immediate or variable");
        return false;
    }
    const QJsonValue values = params.value("values");
    if (source == QStringLiteral("variable")) {
        if (!values.isString() || values.toString().trimmed().isEmpty()) {
            error = QStringLiteral("values must name a variable in variable mode");
            return false;
        }
    } else {
        if (!values.isArray() || values.toArray().isEmpty() || values.toArray().size() > 123 ||
            params.value("startAddress").toInt() + values.toArray().size() > 65536) {
            error = QStringLiteral("values must contain 1..123 registers within address range");
            return false;
        }
        for (const QJsonValue& value : values.toArray()) {
            const double number = value.toDouble();
            if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number || number < 0 ||
                number > 65535) {
                error = QStringLiteral("values must contain only 16-bit unsigned integers");
                return false;
            }
        }
    }
    return true;
}

QWidget* PLCWritePlugin::createConfigWidget() {
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

    connect(ipEdit, &QLineEdit::textChanged, this, [=](const QString& text) { m_params["ipAddress"] = text; });

    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) { m_params["port"] = value; });

    connect(slaveSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { m_params["slaveId"] = value; });

    connect(addrSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { m_params["startAddress"] = value; });

    connect(sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { m_params["valueSource"] = sourceCombo->currentData().toString(); });

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

IModule* PLCWritePlugin::cloneImpl() const {
    return new PLCWritePlugin();
}

} // namespace DeepLux
