#include "PLCCommunicatePlugin.h"

#include "core/common/Logger.h"

#include <QElapsedTimer>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

namespace DeepLux {

PLCCommunicatePlugin::PLCCommunicatePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"ipAddress", "192.168.1.100"}, {"port", 502}, {"timeout", 3000}};
    m_params = m_defaultParams;
}

PLCCommunicatePlugin::~PLCCommunicatePlugin() = default;

bool PLCCommunicatePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "PLCCommunicatePlugin initialized";
    return true;
}

bool PLCCommunicatePlugin::connectToHost(QTcpSocket& socket, const QString& host, quint16 port, int timeoutMs) {
    Q_ASSERT(socket.thread() == QThread::currentThread());
    QElapsedTimer timer;
    timer.start();
    socket.connectToHost(host, port);
    if (socket.state() != QAbstractSocket::ConnectedState && !socket.waitForConnected(timeoutMs)) {
        m_lastWaitMs = static_cast<int>(timer.elapsed());
        emit errorOccurred(tr("连接PLC %1:%2 失败: %3").arg(host).arg(port).arg(socket.errorString()));
        socket.abort();
        return false;
    }
    m_lastWaitMs = static_cast<int>(timer.elapsed());
    Logger::instance().debug("PLCCommunicate: Connected to PLC", "Communication");
    return true;
}

void PLCCommunicatePlugin::disconnectFromHost(QTcpSocket& socket) {
    Q_ASSERT(socket.thread() == QThread::currentThread());
    if (socket.state() == QAbstractSocket::ConnectedState || socket.state() == QAbstractSocket::ConnectingState)
        socket.abort();
}

QByteArray PLCCommunicatePlugin::buildReadRequest(quint16 transactionId, quint8 unitId, quint16 startAddress,
                                                  quint16 readCount) {
    QByteArray request;
    QDataStream stream(&request, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);

    // MBAP Header
    stream << transactionId;
    stream << quint16(0);
    stream << quint16(6);
    stream << unitId;

    // Function Code 03
    stream << quint8(0x03);
    stream << startAddress;
    stream << readCount;

    return request;
}

bool PLCCommunicatePlugin::parseReadResponse(const QByteArray& data) {
    if (data.size() < 9) {
        return false;
    }
    quint8 byteCount = static_cast<quint8>(data[8]);
    return data.size() >= 9 + byteCount;
}

bool PLCCommunicatePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    m_host = m_params["ipAddress"].toString();
    m_port = static_cast<quint16>(m_params["port"].toInt(502));
    m_timeout = m_params["timeout"].toInt(3000);

    if (m_host.isEmpty()) {
        emit errorOccurred(tr("IP地址不能为空"));
        return false;
    }

    QTcpSocket socket;
    Q_ASSERT_X(socket.thread() == QThread::currentThread(), "PLCCommunicatePlugin::process",
               "PLC communication socket must be created in its execution thread");
    if (!connectToHost(socket, m_host, m_port, m_timeout)) {
        output.setData("plc_connected", false);
        return false;
    }

    // Send a minimal read request to verify Modbus communication
    QByteArray request = buildReadRequest(++m_transactionId, 1, 0, 1);
    qint64 written = socket.write(request);
    const bool flushed = socket.flush();

    bool commOk = false;
    if (written == request.size() && flushed) {
        QByteArray readBuffer;
        QElapsedTimer readTimer;
        readTimer.start();
        while (readTimer.elapsed() < m_timeout) {
            readBuffer += socket.readAll();
            if (parseReadResponse(readBuffer)) {
                commOk = true;
                break;
            }
            const int remaining = qMax(1, m_timeout - static_cast<int>(readTimer.elapsed()));
            if (!socket.waitForReadyRead(remaining))
                break;
        }
    }

    disconnectFromHost(socket);

    output.setData("plc_connected", commOk);
    output.setData("plc_response_time_ms", m_lastWaitMs);

    if (commOk) {
        Logger::instance().info(
            QString("PLCCommunicate: Connection to %1:%2 successful (%3ms)").arg(m_host).arg(m_port).arg(m_lastWaitMs),
            "Communication");
    } else {
        emit errorOccurred(tr("PLC通信测试失败"));
    }

    return commOk;
}

bool PLCCommunicatePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    if (params["ipAddress"].toString().isEmpty()) {
        error = QString("IP地址不能为空");
        return false;
    }
    if (params["port"].toInt() <= 0 || params["port"].toInt() > 65535) {
        error = QString("端口必须在1-65535之间");
        return false;
    }
    return true;
}

QWidget* PLCCommunicatePlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* ipEdit = new QLineEdit(m_params["ipAddress"].toString());

    QSpinBox* portSpin = new QSpinBox();
    portSpin->setRange(1, 65535);
    portSpin->setValue(m_params["port"].toInt(502));
    portSpin->setPrefix(tr("端口: "));

    QSpinBox* timeoutSpin = new QSpinBox();
    timeoutSpin->setRange(100, 60000);
    timeoutSpin->setValue(m_params["timeout"].toInt(3000));
    timeoutSpin->setPrefix(tr("超时(ms): "));

    formLayout->addRow(tr("IP地址:"), ipEdit);
    formLayout->addRow(tr("端口:"), portSpin);
    formLayout->addRow(tr("超时:"), timeoutSpin);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(ipEdit, &QLineEdit::textChanged, this, [=](const QString& text) { m_params["ipAddress"] = text; });

    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) { m_params["port"] = value; });

    connect(timeoutSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { m_params["timeout"] = value; });

    return widget;
}

IModule* PLCCommunicatePlugin::cloneImpl() const {
    return new PLCCommunicatePlugin();
}

} // namespace DeepLux
