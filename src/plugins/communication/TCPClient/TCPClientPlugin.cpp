#include "TCPClientPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>

namespace DeepLux {

TCPClientPlugin::TCPClientPlugin(QObject* parent)
    : ModuleBase(parent)
    , m_socket(new QTcpSocket(this))
    , m_timeoutTimer(new QTimer(this))
    , m_connectLoop(nullptr)
    , m_readLoop(nullptr)
{
    m_defaultParams = QJsonObject{
        {"host", "127.0.0.1"},
        {"port", 8080},
        {"timeout", 3000},
        {"writeData", ""},
        {"readVariable", "tcp_data"},
        {"operation", "WriteRead"}
    };
    m_params = m_defaultParams;

    // Timeout timer fires only once per use; we recreate it per-operation
    m_timeoutTimer->setSingleShot(true);

    connect(m_socket, &QTcpSocket::connected, this, &TCPClientPlugin::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &TCPClientPlugin::onDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &TCPClientPlugin::onError);
    connect(m_socket, &QTcpSocket::readyRead, this, &TCPClientPlugin::onReadyRead);
}

TCPClientPlugin::~TCPClientPlugin()
{
    disconnectFromServer();
}

bool TCPClientPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "TCPClientPlugin initialized";
    return true;
}

void TCPClientPlugin::onConnected()
{
    Logger::instance().debug("TCPClient: Connected to server", "Communication");
    if (m_connectLoop) {
        m_connectLoop->quit();
    }
}

void TCPClientPlugin::onDisconnected()
{
    Logger::instance().debug("TCPClient: Disconnected from server", "Communication");
}

void TCPClientPlugin::onError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    emit errorOccurred(tr("TCP错误: %1").arg(m_socket->errorString()));
    // Quit any running event loops so we don't stay stuck
    if (m_connectLoop && m_connectLoop->isRunning()) {
        m_connectLoop->quit();
    }
    if (m_readLoop && m_readLoop->isRunning()) {
        m_readLoop->quit();
    }
}

void TCPClientPlugin::onReadyRead()
{
    m_readBuffer += m_socket->readAll();
    if (m_readLoop) {
        m_readLoop->quit();
    }
}

void TCPClientPlugin::onConnectTimeout()
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        m_socket->abort();
        emit errorOccurred(tr("连接到 %1:%2 超时 (%3ms)")
            .arg(m_host).arg(m_port).arg(m_timeout));
    }
    if (m_connectLoop) {
        m_connectLoop->quit();
    }
}

void TCPClientPlugin::onReadTimeout()
{
    m_readTimedOut = true;
    m_socket->disconnectFromHost();
    if (m_readLoop) {
        m_readLoop->quit();
    }
}

bool TCPClientPlugin::connectToServerAsync()
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
    connect(&timer, &QTimer::timeout, this, &TCPClientPlugin::onConnectTimeout);

    loop.exec();

    m_connectLoop = nullptr;
    m_lastWaitMs = m_timeout - timer.remainingTime();

    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    Logger::instance().info(QString("TCPClient: Connected to %1:%2").arg(m_host).arg(m_port), "Communication");
    return true;
}

void TCPClientPlugin::disconnectFromServer()
{
    if (m_socket->state() == QAbstractSocket::ConnectedState ||
        m_socket->state() == QAbstractSocket::ConnectingState) {
        m_socket->abort();
    }
}

QString TCPClientPlugin::readFromServerAsync()
{
    m_readBuffer.clear();
    m_readTimedOut = false;

    // If data already available, read it
    if (m_socket->bytesAvailable() > 0) {
        m_readBuffer = m_socket->readAll();
        return QString::fromUtf8(m_readBuffer);
    }

    QEventLoop loop;
    m_readLoop = &loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(m_timeout);
    connect(&timer, &QTimer::timeout, this, &TCPClientPlugin::onReadTimeout);

    loop.exec();

    m_readLoop = nullptr;
    m_lastWaitMs = m_timeout - timer.remainingTime();

    // Drain any accumulated data
    if (!m_readTimedOut && m_socket->state() == QAbstractSocket::ConnectedState) {
        m_readBuffer += m_socket->readAll();
    }

    return QString::fromUtf8(m_readBuffer);
}

bool TCPClientPlugin::writeToServer(const QString& data)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        emit errorOccurred(tr("未连接到服务器"));
        return false;
    }

    QByteArray bytes = data.toUtf8();
    qint64 written = m_socket->write(bytes);
    if (written != bytes.size()) {
        emit errorOccurred(tr("写入数据失败"));
        return false;
    }

    m_socket->flush();
    return true;
}

bool TCPClientPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_host = m_params["host"].toString();
    m_port = m_params["port"].toInt(8080);
    m_timeout = m_params["timeout"].toInt(3000);
    m_writeData = m_params["writeData"].toString();
    m_readVariable = m_params["readVariable"].toString("tcp_data");
    QString operation = m_params["operation"].toString("WriteRead");

    if (m_host.isEmpty()) {
        emit errorOccurred(tr("主机地址不能为空"));
        return false;
    }

    if (!connectToServerAsync()) {
        return false;
    }

    bool success = true;

    if (operation == "Write" || operation == "WriteRead") {
        QString dataToWrite = m_writeData;
        if (dataToWrite.isEmpty()) {
            dataToWrite = input.data("send_data").toString();
        }
        if (!dataToWrite.isEmpty()) {
            success = writeToServer(dataToWrite);
            if (success) {
                Logger::instance().debug(QString("TCPClient: Wrote '%1'").arg(dataToWrite), "Communication");
            }
        }
    }

    if (success && (operation == "Read" || operation == "WriteRead")) {
        QString readData = readFromServerAsync();
        if (m_readTimedOut) {
            emit errorOccurred(tr("读取数据超时"));
            disconnectFromServer();
            return false;
        }
        if (!readData.isEmpty()) {
            output.setData(m_readVariable, readData);
            output.setData(m_readVariable + "_bytes", readData.toUtf8().size());
            Logger::instance().debug(QString("TCPClient: Read '%1'").arg(readData), "Communication");
        }
    }

    disconnectFromServer();
    return success;
}

bool TCPClientPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["host"].toString().isEmpty()) {
        error = QString("Host cannot be empty");
        return false;
    }
    if (params["port"].toInt() <= 0 || params["port"].toInt() > 65535) {
        error = QString("Port must be between 1 and 65535");
        return false;
    }
    return true;
}

QWidget* TCPClientPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    // Host
    QLineEdit* hostEdit = new QLineEdit(m_params["host"].toString());

    // Port
    QSpinBox* portSpin = new QSpinBox();
    portSpin->setRange(1, 65535);
    portSpin->setValue(m_params["port"].toInt(8080));
    portSpin->setPrefix(tr("端口: "));

    // Timeout
    QSpinBox* timeoutSpin = new QSpinBox();
    timeoutSpin->setRange(100, 60000);
    timeoutSpin->setValue(m_params["timeout"].toInt(3000));
    timeoutSpin->setPrefix(tr("超时(ms): "));

    // Operation mode
    QComboBox* operationCombo = new QComboBox();
    operationCombo->addItem(tr("写入"), "Write");
    operationCombo->addItem(tr("读取"), "Read");
    operationCombo->addItem(tr("写入并读取"), "WriteRead");
    int idx = operationCombo->findData(m_params["operation"].toString("WriteRead"));
    if (idx >= 0) operationCombo->setCurrentIndex(idx);

    // Write data
    QLineEdit* writeEdit = new QLineEdit(m_params["writeData"].toString());

    // Read variable
    QLineEdit* readVarEdit = new QLineEdit(m_params["readVariable"].toString("tcp_data"));

    formLayout->addRow(tr("主机地址:"), hostEdit);
    formLayout->addRow(tr("端口:"), portSpin);
    formLayout->addRow(tr("超时:"), timeoutSpin);
    formLayout->addRow(tr("操作模式:"), operationCombo);
    formLayout->addRow(tr("发送数据:"), writeEdit);
    formLayout->addRow(tr("读取变量:"), readVarEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    // Connections
    connect(hostEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["host"] = text;
    });

    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["port"] = value;
    });

    connect(timeoutSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["timeout"] = value;
    });

    connect(operationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["operation"] = operationCombo->currentData().toString();
    });

    connect(writeEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["writeData"] = text;
    });

    connect(readVarEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["readVariable"] = text;
    });

    return widget;
}

} // namespace DeepLux
