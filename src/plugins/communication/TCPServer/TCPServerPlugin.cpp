#include "TCPServerPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>

namespace DeepLux {

TCPServerPlugin::TCPServerPlugin(QObject* parent)
    : ModuleBase(parent)
    , m_server(new QTcpServer(this))
    , m_clientSocket(nullptr)
{
    m_defaultParams = QJsonObject{
        {"port", 8080},
        {"timeout", 5000},
        {"writeData", ""},
        {"readVariable", "tcp_data"},
        {"operation", "WriteRead"}
    };
    m_params = m_defaultParams;

    connect(m_server, &QTcpServer::newConnection, this, &TCPServerPlugin::onNewConnection);
}

TCPServerPlugin::~TCPServerPlugin()
{
    stopServer();
}

bool TCPServerPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "TCPServerPlugin initialized";
    return true;
}

void TCPServerPlugin::onNewConnection()
{
    m_clientSocket = m_server->nextPendingConnection();
    if (m_clientSocket) {
        connect(m_clientSocket, &QTcpSocket::disconnected, this, &TCPServerPlugin::onDisconnected);
        connect(m_clientSocket, &QTcpSocket::readyRead, this, &TCPServerPlugin::onReadyRead);
        Logger::instance().debug("TCPServer: Client connected", "Communication");
    }
}

void TCPServerPlugin::onDisconnected()
{
    Logger::instance().debug("TCPServer: Client disconnected", "Communication");
    if (m_clientSocket) {
        m_clientSocket->deleteLater();
        m_clientSocket = nullptr;
    }
}

void TCPServerPlugin::onReadyRead()
{
    // Data will be read in process()
}

bool TCPServerPlugin::startServer()
{
    if (m_server->isListening()) {
        m_server->close();
    }

    if (!m_server->listen(QHostAddress::Any, m_port)) {
        emit errorOccurred(tr("无法启动服务器: %1").arg(m_server->errorString()));
        return false;
    }

    Logger::instance().info(QString("TCPServer: Listening on port %1").arg(m_port), "Communication");

    // Wait for client connection
    if (!m_server->waitForNewConnection(m_timeout)) {
        emit errorOccurred(tr("等待客户端连接超时"));
        m_server->close();
        return false;
    }

    m_clientSocket = m_server->nextPendingConnection();
    if (!m_clientSocket) {
        emit errorOccurred(tr("获取客户端连接失败"));
        m_server->close();
        return false;
    }

    connect(m_clientSocket, &QTcpSocket::disconnected, this, &TCPServerPlugin::onDisconnected);
    connect(m_clientSocket, &QTcpSocket::readyRead, this, &TCPServerPlugin::onReadyRead);

    Logger::instance().info("TCPServer: Client connected", "Communication");
    return true;
}

void TCPServerPlugin::stopServer()
{
    if (m_clientSocket) {
        m_clientSocket->disconnectFromHost();
        m_clientSocket->deleteLater();
        m_clientSocket = nullptr;
    }
    if (m_server->isListening()) {
        m_server->close();
        Logger::instance().info(QString("TCPServer: Stopped").arg(m_port), "Communication");
    }
}

QString TCPServerPlugin::readFromClient()
{
    if (!m_clientSocket || !m_clientSocket->waitForReadyRead(m_timeout)) {
        return QString();
    }

    QByteArray data = m_clientSocket->readAll();
    while (m_clientSocket->waitForReadyRead(10)) {
        data += m_clientSocket->readAll();
    }

    return QString::fromUtf8(data);
}

bool TCPServerPlugin::writeToClient(const QString& data)
{
    if (!m_clientSocket || m_clientSocket->state() != QAbstractSocket::ConnectedState) {
        emit errorOccurred(tr("客户端未连接"));
        return false;
    }

    QByteArray bytes = data.toUtf8();
    qint64 written = m_clientSocket->write(bytes);
    if (written != bytes.size()) {
        emit errorOccurred(tr("写入数据失败"));
        return false;
    }

    m_clientSocket->flush();
    return true;
}

bool TCPServerPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_port = m_params["port"].toInt(8080);
    m_timeout = m_params["timeout"].toInt(5000);
    m_writeData = m_params["writeData"].toString();
    m_readVariable = m_params["readVariable"].toString("tcp_data");
    QString operation = m_params["operation"].toString("WriteRead");

    if (!startServer()) {
        return false;
    }

    bool success = true;

    if (operation == "Read" || operation == "WriteRead") {
        QString readData = readFromClient();
        if (!readData.isEmpty()) {
            output.setData(m_readVariable, readData);
            output.setData(m_readVariable + "_bytes", readData.toUtf8().size());
            Logger::instance().debug(QString("TCPServer: Read '%1'").arg(readData), "Communication");
        }
    }

    if (success && (operation == "Write" || operation == "WriteRead")) {
        QString dataToWrite = m_writeData;
        if (dataToWrite.isEmpty()) {
            dataToWrite = input.data("send_data").toString();
        }
        if (!dataToWrite.isEmpty()) {
            success = writeToClient(dataToWrite);
            if (success) {
                Logger::instance().debug(QString("TCPServer: Wrote '%1'").arg(dataToWrite), "Communication");
            }
        }
    }

    stopServer();
    return success;
}

bool TCPServerPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["port"].toInt() <= 0 || params["port"].toInt() > 65535) {
        error = QString("Port must be between 1 and 65535");
        return false;
    }
    return true;
}

QWidget* TCPServerPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    // Port
    QSpinBox* portSpin = new QSpinBox();
    portSpin->setRange(1, 65535);
    portSpin->setValue(m_params["port"].toInt(8080));
    portSpin->setPrefix(tr("端口: "));

    // Timeout
    QSpinBox* timeoutSpin = new QSpinBox();
    timeoutSpin->setRange(100, 60000);
    timeoutSpin->setValue(m_params["timeout"].toInt(5000));
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

    formLayout->addRow(tr("端口:"), portSpin);
    formLayout->addRow(tr("超时:"), timeoutSpin);
    formLayout->addRow(tr("操作模式:"), operationCombo);
    formLayout->addRow(tr("发送数据:"), writeEdit);
    formLayout->addRow(tr("读取变量:"), readVarEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    // Connections
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
