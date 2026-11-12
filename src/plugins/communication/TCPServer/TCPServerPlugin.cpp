#include "TCPServerPlugin.h"

#include "core/common/Logger.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QHostAddress>
#include <QLineEdit>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QVBoxLayout>
#include <mutex>

namespace DeepLux {

namespace {
#ifdef DEEPLUX_ENABLE_TEST_HOOKS
std::mutex g_responseWriterTestHookMutex;
TCPServerPlugin::ResponseWriterTestHook g_responseWriterTestHook;
#endif
} // namespace

TCPServerPlugin::TCPServerPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{
        {"bindAddress", "Any"},    {"port", 8080}, {"timeout", 5000}, {"writeData", ""}, {"readVariable", "tcp_data"},
        {"operation", "WriteRead"}};
    m_params = m_defaultParams;
}

TCPServerPlugin::~TCPServerPlugin() = default;

#ifdef DEEPLUX_ENABLE_TEST_HOOKS
void TCPServerPlugin::setResponseWriterTestHook(ResponseWriterTestHook hook) {
    std::lock_guard<std::mutex> lock(g_responseWriterTestHookMutex);
    g_responseWriterTestHook = std::move(hook);
}
#endif

TCPServerPlugin::ResponseWriteOutcome TCPServerPlugin::writeResponse(QTcpSocket& socket, const QByteArray& bytes,
                                                                     int timeoutMs) const {
#ifdef DEEPLUX_ENABLE_TEST_HOOKS
    ResponseWriterTestHook testHook;
    {
        std::lock_guard<std::mutex> lock(g_responseWriterTestHookMutex);
        testHook = g_responseWriterTestHook;
    }
    if (testHook)
        return testHook(socket, bytes, timeoutMs);
#endif

    ResponseWriteOutcome outcome;
    outcome.acceptedBytes = socket.write(bytes);
    if (outcome.acceptedBytes != bytes.size()) {
        outcome.failureStage = QStringLiteral("write");
        outcome.errorMessage = tr("写入响应失败");
        return outcome;
    }

    QElapsedTimer timer;
    timer.start();
    while (socket.bytesToWrite() > 0) {
        const int remainingMs = timeoutMs - static_cast<int>(timer.elapsed());
        if (remainingMs <= 0 || !socket.waitForBytesWritten(qMax(1, remainingMs))) {
            outcome.failureStage = socket.error() == QAbstractSocket::UnknownSocketError
                                       ? QStringLiteral("drain_timeout")
                                       : QStringLiteral("socket_error");
            outcome.errorMessage = tr("等待响应发送完成失败");
            return outcome;
        }
    }
    outcome.completed = true;
    return outcome;
}

ExecutionResult TCPServerPlugin::execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) {
    m_responseAcceptedBytes = 0;
    m_responseExpectedBytes = 0;
    m_responseFailureStage.clear();

    const ExecutionResult result = ModuleBase::execute(inputs, outputs, context);
    if (!result.success && m_responseAcceptedBytes > 0) {
        const QString diagnostics =
            QStringLiteral("stage=%1 acceptedBytes=%2 expectedBytes=%3")
                .arg(m_responseFailureStage.isEmpty() ? QStringLiteral("unknown") : m_responseFailureStage)
                .arg(m_responseAcceptedBytes)
                .arg(m_responseExpectedBytes);
        return ExecutionResult::fail(ExecError::SideEffectUncertain,
                                     tr("TCPServer 响应发送结果无法确认：响应字节已提交但发送未完成"), diagnostics);
    }
    return result;
}

bool TCPServerPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "TCPServerPlugin initialized";
    return true;
}

bool TCPServerPlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    QJsonObject params = currentParams();
    const quint16 port = static_cast<quint16>(params.value("port").toInt(8080));
    const int timeout = params.value("timeout").toInt(5000);
    const QString writeData = params.value("writeData").toString();
    const QString readVariable = params.value("readVariable").toString("tcp_data");
    const QHostAddress bindAddress = params.value("bindAddress").toString("Any") == QLatin1String("LocalHost")
                                         ? QHostAddress(QHostAddress::LocalHost)
                                         : QHostAddress(QHostAddress::Any);
    QString operation = params["operation"].toString("WriteRead");

    QTcpServer server;
    Q_ASSERT_X(server.thread() == QThread::currentThread(), "TCPServerPlugin::process",
               "TCPServer must be created in its execution thread");
    if (!server.listen(bindAddress, port)) {
        emit errorOccurred(tr("无法启动服务器: %1").arg(server.errorString()));
        return false;
    }
    Logger::instance().info(
        QString("TCPServer: Listening on %1:%2").arg(server.serverAddress().toString(), QString::number(port)),
        "Communication");
    if (!server.waitForNewConnection(timeout)) {
        emit errorOccurred(tr("等待客户端连接超时"));
        return false;
    }

    QTcpSocket* clientSocket = server.nextPendingConnection();
    if (!clientSocket) {
        emit errorOccurred(tr("获取客户端连接失败"));
        return false;
    }
    Q_ASSERT_X(clientSocket->thread() == QThread::currentThread(), "TCPServerPlugin::process",
               "accepted client socket must share the server execution thread");
    Logger::instance().info("TCPServer: Client connected", "Communication");

    bool success = true;

    if (operation == "Read" || operation == "WriteRead") {
        if (!clientSocket->waitForReadyRead(timeout)) {
            emit errorOccurred(tr("等待客户端数据超时"));
            return false;
        }
        QByteArray bytes = clientSocket->readAll();
        while (clientSocket->waitForReadyRead(10))
            bytes += clientSocket->readAll();
        const QString readData = QString::fromUtf8(bytes);
        if (!readData.isEmpty()) {
            output.setData(readVariable, readData);
            output.setData(readVariable + "_bytes", bytes.size());
            Logger::instance().debug(QString("TCPServer: Read '%1'").arg(readData), "Communication");
        }
    }

    if (success && (operation == "Write" || operation == "WriteRead")) {
        QString dataToWrite = writeData;
        if (dataToWrite.isEmpty()) {
            dataToWrite = input.data("send_data").toString();
        }
        if (!dataToWrite.isEmpty()) {
            const QByteArray bytes = dataToWrite.toUtf8();
            ResponseWriteOutcome outcome;
            if (clientSocket->state() != QAbstractSocket::ConnectedState) {
                outcome.failureStage = QStringLiteral("not_connected");
                outcome.errorMessage = tr("客户端连接已断开");
            } else {
                outcome = writeResponse(*clientSocket, bytes, timeout);
            }
            m_responseAcceptedBytes = outcome.acceptedBytes;
            m_responseExpectedBytes = bytes.size();
            m_responseFailureStage = outcome.failureStage;
            if (!outcome.completed) {
                emit errorOccurred(outcome.errorMessage.isEmpty() ? tr("写入响应失败") : outcome.errorMessage);
                success = false;
            } else {
                Logger::instance().debug(QString("TCPServer: Wrote '%1'").arg(dataToWrite), "Communication");
            }
        }
    }

    server.close();
    return success;
}

bool TCPServerPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    if (params["port"].toInt() <= 0 || params["port"].toInt() > 65535) {
        error = QString("Port must be between 1 and 65535");
        return false;
    }
    const QString bindAddress = params.value("bindAddress").toString("Any");
    if (bindAddress != QLatin1String("Any") && bindAddress != QLatin1String("LocalHost")) {
        error = QString("Bind address must be Any or LocalHost");
        return false;
    }
    return true;
}

QWidget* TCPServerPlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QComboBox* bindAddressCombo = new QComboBox();
    bindAddressCombo->setObjectName(QStringLiteral("tcpServerBindAddressCombo"));
    bindAddressCombo->addItem(tr("所有网络接口"), QStringLiteral("Any"));
    bindAddressCombo->addItem(tr("仅本机"), QStringLiteral("LocalHost"));
    const int bindAddressIndex = bindAddressCombo->findData(m_params.value("bindAddress").toString("Any"));
    if (bindAddressIndex >= 0)
        bindAddressCombo->setCurrentIndex(bindAddressIndex);

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
    if (idx >= 0)
        operationCombo->setCurrentIndex(idx);

    // Write data
    QLineEdit* writeEdit = new QLineEdit(m_params["writeData"].toString());

    // Read variable
    QLineEdit* readVarEdit = new QLineEdit(m_params["readVariable"].toString("tcp_data"));

    formLayout->addRow(tr("监听地址:"), bindAddressCombo);
    formLayout->addRow(tr("端口:"), portSpin);
    formLayout->addRow(tr("超时:"), timeoutSpin);
    formLayout->addRow(tr("操作模式:"), operationCombo);
    formLayout->addRow(tr("发送数据:"), writeEdit);
    formLayout->addRow(tr("读取变量:"), readVarEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    // Connections
    connect(bindAddressCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { setParam("bindAddress", bindAddressCombo->currentData().toString()); });
    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) { setParam("port", value); });

    connect(timeoutSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { setParam("timeout", value); });

    connect(operationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { setParam("operation", operationCombo->currentData().toString()); });

    connect(writeEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("writeData", text); });

    connect(readVarEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("readVariable", text); });

    return widget;
}

IModule* TCPServerPlugin::cloneImpl() const {
    return new TCPServerPlugin();
}

} // namespace DeepLux
