#include "PLCCommunicatePlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>

namespace DeepLux {

PLCCommunicatePlugin::PLCCommunicatePlugin(QObject* parent)
    : ModuleBase(parent)
    , m_socket(new QTcpSocket(this))
    , m_connectLoop(nullptr)
    , m_readLoop(nullptr)
{
    m_defaultParams = QJsonObject{
        {"ipAddress", "192.168.1.100"},
        {"port", 502},
        {"timeout", 3000}
    };
    m_params = m_defaultParams;

    connect(m_socket, &QTcpSocket::connected, this, &PLCCommunicatePlugin::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &PLCCommunicatePlugin::onDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &PLCCommunicatePlugin::onError);
    connect(m_socket, &QTcpSocket::readyRead, this, &PLCCommunicatePlugin::onReadyRead);
}

PLCCommunicatePlugin::~PLCCommunicatePlugin()
{
    disconnectFromHost();
}

bool PLCCommunicatePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "PLCCommunicatePlugin initialized";
    return true;
}

void PLCCommunicatePlugin::onConnected()
{
    Logger::instance().debug("PLCCommunicate: Connected to PLC", "Communication");
    if (m_connectLoop) {
        m_connectLoop->quit();
    }
}

void PLCCommunicatePlugin::onDisconnected()
{
    Logger::instance().debug("PLCCommunicate: Disconnected from PLC", "Communication");
}

void PLCCommunicatePlugin::onError(QAbstractSocket::SocketError error)
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

void PLCCommunicatePlugin::onReadyRead()
{
    m_readBuffer += m_socket->readAll();
    if (m_readLoop) {
        m_readLoop->quit();
    }
}

void PLCCommunicatePlugin::onConnectTimeout()
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        m_socket->abort();
        emit errorOccurred(tr("连接PLC %1:%2 超时").arg(m_host).arg(m_port));
    }
    if (m_connectLoop) {
        m_connectLoop->quit();
    }
}

void PLCCommunicatePlugin::onReadTimeout()
{
    m_readTimedOut = true;
    m_socket->disconnectFromHost();
    if (m_readLoop) {
        m_readLoop->quit();
    }
}

bool PLCCommunicatePlugin::connectToHost()
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
    connect(&timer, &QTimer::timeout, this, &PLCCommunicatePlugin::onConnectTimeout);

    loop.exec();
    m_connectLoop = nullptr;
    m_lastWaitMs = m_timeout - timer.remainingTime();

    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    return true;
}

void PLCCommunicatePlugin::disconnectFromHost()
{
    if (m_socket->state() == QAbstractSocket::ConnectedState ||
        m_socket->state() == QAbstractSocket::ConnectingState) {
        m_socket->abort();
    }
}

QByteArray PLCCommunicatePlugin::buildReadRequest(quint16 transactionId, quint8 unitId,
                                                   quint16 startAddress, quint16 readCount)
{
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

bool PLCCommunicatePlugin::parseReadResponse(const QByteArray& data)
{
    if (data.size() < 9) {
        return false;
    }
    quint8 byteCount = static_cast<quint8>(data[8]);
    return data.size() >= 9 + byteCount;
}

bool PLCCommunicatePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_host = m_params["ipAddress"].toString();
    m_port = m_params["port"].toInt(502);
    m_timeout = m_params["timeout"].toInt(3000);

    if (m_host.isEmpty()) {
        emit errorOccurred(tr("IP地址不能为空"));
        return false;
    }

    if (!connectToHost()) {
        output.setData("plc_connected", false);
        return false;
    }

    // Send a minimal read request to verify Modbus communication
    QByteArray request = buildReadRequest(++m_transactionId, 1, 0, 1);
    qint64 written = m_socket->write(request);
    m_socket->flush();

    bool commOk = false;
    if (written == request.size()) {
        m_readBuffer.clear();
        m_readTimedOut = false;

        QEventLoop loop;
        m_readLoop = &loop;
        QTimer timer;
        timer.setSingleShot(true);
        timer.start(m_timeout);
        connect(&timer, &QTimer::timeout, this, &PLCCommunicatePlugin::onReadTimeout);

        loop.exec();
        m_readLoop = nullptr;

        if (!m_readTimedOut && parseReadResponse(m_readBuffer)) {
            commOk = true;
        }
    }

    disconnectFromHost();

    output.setData("plc_connected", commOk);
    output.setData("plc_response_time_ms", m_lastWaitMs);

    if (commOk) {
        Logger::instance().info(QString("PLCCommunicate: Connection to %1:%2 successful (%3ms)")
            .arg(m_host).arg(m_port).arg(m_lastWaitMs), "Communication");
    } else {
        emit errorOccurred(tr("PLC通信测试失败"));
    }

    return commOk;
}

bool PLCCommunicatePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
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

QWidget* PLCCommunicatePlugin::createConfigWidget()
{
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

    connect(ipEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["ipAddress"] = text;
    });

    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["port"] = value;
    });

    connect(timeoutSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["timeout"] = value;
    });

    return widget;
}

} // namespace DeepLux
