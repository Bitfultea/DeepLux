#include "TCPClientPlugin.h"

#include "core/common/Logger.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QTcpSocket>
#include <QThread>
#include <QVBoxLayout>

namespace DeepLux {

TCPClientPlugin::TCPClientPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{
        {"host", "127.0.0.1"},     {"port", 8080}, {"timeout", 3000}, {"writeData", ""}, {"readVariable", "tcp_data"},
        {"operation", "WriteRead"}};
    m_params = m_defaultParams;
}

bool TCPClientPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "TCPClientPlugin initialized";
    return true;
}

ExecutionResult TCPClientPlugin::execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) {
    m_writeMayHaveOccurred = false;
    m_lastFailureMessage.clear();
    const ExecutionResult result = ModuleBase::execute(inputs, outputs, context);
    if (!result.success && m_writeMayHaveOccurred) {
        const QString message = m_lastFailureMessage.isEmpty()
                                    ? tr("TCP 写入结果无法确认：请求字节已提交但未收到有效响应")
                                    : m_lastFailureMessage;
        return ExecutionResult::fail(ExecError::SideEffectUncertain, message, result.diagnostics);
    }
    return result;
}

bool TCPClientPlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    const QJsonObject params = currentParams();
    const QString host = params["host"].toString();
    const quint16 port = static_cast<quint16>(params["port"].toInt(8080));
    const int timeout = params["timeout"].toInt(3000);
    const QString writeData = params["writeData"].toString();
    const QString readVariable = params["readVariable"].toString("tcp_data");
    const QString operation = params["operation"].toString("WriteRead");

    if (host.isEmpty()) {
        emit errorOccurred(tr("主机地址不能为空"));
        return false;
    }

    QTcpSocket socket;
    Q_ASSERT_X(socket.thread() == QThread::currentThread(), "TCPClientPlugin::process",
               "The TCP socket must be created and used in the module execution thread");
    QElapsedTimer waitTimer;
    socket.connectToHost(host, port);
    waitTimer.start();
    if (!socket.waitForConnected(timeout)) {
        m_lastWaitMs = static_cast<int>(waitTimer.elapsed());
        if (socket.error() == QAbstractSocket::SocketTimeoutError) {
            m_lastFailureMessage = tr("连接到 %1:%2 超时 (%3ms)").arg(host).arg(port).arg(timeout);
            emit errorOccurred(m_lastFailureMessage);
        } else {
            m_lastFailureMessage =
                tr("TCP错误: %1 (目标 %2:%3, timeout=%4ms)").arg(socket.errorString(), host).arg(port).arg(timeout);
            emit errorOccurred(m_lastFailureMessage);
        }
        return false;
    }
    m_lastWaitMs = static_cast<int>(waitTimer.elapsed());
    Logger::instance().info(QString("TCPClient: Connected to %1:%2").arg(host).arg(port), "Communication");

    bool success = true;

    if (operation == "Write" || operation == "WriteRead") {
        QString dataToWrite = writeData;
        if (dataToWrite.isEmpty()) {
            dataToWrite = input.data("send_data").toString();
        }
        if (!dataToWrite.isEmpty()) {
            const QByteArray bytes = dataToWrite.toUtf8();
            const qint64 acceptedBytes = socket.write(bytes);
            m_writeMayHaveOccurred = acceptedBytes > 0;
            success = acceptedBytes == bytes.size();
            if (success) {
                socket.flush();
                Logger::instance().debug(QString("TCPClient: Wrote '%1'").arg(dataToWrite), "Communication");
            } else {
                m_lastFailureMessage = tr("写入数据失败（目标 %1:%2）").arg(host).arg(port);
                emit errorOccurred(m_lastFailureMessage);
            }
        }
    }

    if (success && (operation == "Read" || operation == "WriteRead")) {
        waitTimer.restart();
        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(timeout)) {
            m_lastWaitMs = static_cast<int>(waitTimer.elapsed());
            m_lastFailureMessage = tr("读取 %1:%2 响应超时 (%3ms)").arg(host).arg(port).arg(timeout);
            emit errorOccurred(m_lastFailureMessage);
            return false;
        }
        m_lastWaitMs = static_cast<int>(waitTimer.elapsed());
        const QString readData = QString::fromUtf8(socket.readAll());
        if (!readData.isEmpty()) {
            output.setData(readVariable, readData);
            output.setData(readVariable + "_bytes", readData.toUtf8().size());
            Logger::instance().debug(QString("TCPClient: Read '%1'").arg(readData), "Communication");
        }
    }

    socket.disconnectFromHost();
    return success;
}

bool TCPClientPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
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

QWidget* TCPClientPlugin::createConfigWidget() {
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
    if (idx >= 0)
        operationCombo->setCurrentIndex(idx);

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
    connect(hostEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("host", text); });

    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) { setParam("port", value); });

    connect(timeoutSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { setParam("timeout", value); });

    connect(operationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { setParam("operation", operationCombo->currentData().toString()); });

    connect(writeEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("writeData", text); });

    connect(readVarEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("readVariable", text); });

    return widget;
}

IModule* TCPClientPlugin::cloneImpl() const {
    return new TCPClientPlugin();
}

} // namespace DeepLux
