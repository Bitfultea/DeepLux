#include "SerialPortPlugin.h"

#include "core/common/Logger.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

namespace DeepLux {

SerialPortPlugin::SerialPortPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"portName", ""},          {"baudRate", 9600},  {"dataBits", 8},
                                  {"parity", "None"},        {"stopBits", "One"}, {"flowControl", "None"},
                                  {"timeout", 100},          {"writeData", ""},   {"readVariable", "serial_data"},
                                  {"operation", "WriteRead"}};
    m_params = m_defaultParams;
}

SerialPortPlugin::~SerialPortPlugin() = default;

bool SerialPortPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "SerialPortPlugin initialized";
    return true;
}

bool SerialPortPlugin::openSerialPort(QSerialPort& serial, const QString& portName, int baudRate,
                                      QSerialPort::DataBits dataBits, QSerialPort::Parity parity,
                                      QSerialPort::StopBits stopBits, QSerialPort::FlowControl flowControl) {
    serial.setPortName(portName);
    serial.setBaudRate(baudRate);
    serial.setDataBits(dataBits);
    serial.setParity(parity);
    serial.setStopBits(stopBits);
    serial.setFlowControl(flowControl);

    if (!serial.open(QIODevice::ReadWrite)) {
        emit errorOccurred(tr("无法打开串口 %1: %2").arg(portName, serial.errorString()));
        return false;
    }

    Logger::instance().info(QString("SerialPort: Opened %1 at %2 baud").arg(portName).arg(baudRate), "Communication");
    return true;
}

QString SerialPortPlugin::readFromPort(QSerialPort& serial, int timeoutMs) {
    if (!serial.waitForReadyRead(timeoutMs)) {
        return QString();
    }

    QByteArray data = serial.readAll();
    while (serial.waitForReadyRead(10)) {
        data += serial.readAll();
    }

    return QString::fromUtf8(data);
}

bool SerialPortPlugin::writeToPort(QSerialPort& serial, const QString& data) {
    if (!serial.isOpen()) {
        emit errorOccurred(tr("串口未打开"));
        return false;
    }

    QByteArray bytes = data.toUtf8();
    qint64 written = serial.write(bytes);
    m_writeMayHaveOccurred = written > 0;
    if (written != bytes.size()) {
        emit errorOccurred(tr("写入串口失败"));
        return false;
    }

    if (!serial.flush()) {
        emit errorOccurred(tr("刷新串口写入失败"));
        return false;
    }
    return true;
}

ExecutionResult SerialPortPlugin::execute(const PortValueMap& inputs, PortValueMap& outputs,
                                          ExecutionContext& context) {
    m_writeMayHaveOccurred = false;
    const ExecutionResult result = ModuleBase::execute(inputs, outputs, context);
    if (!result.success && m_writeMayHaveOccurred) {
        return ExecutionResult::fail(ExecError::SideEffectUncertain,
                                     tr("串口写入结果无法确认：写入已提交但未收到有效响应"), result.diagnostics);
    }
    return result;
}

bool SerialPortPlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    const QJsonObject params = currentParams();
    const QString portName = params["portName"].toString();
    const int baudRate = params["baudRate"].toInt(9600);
    const int timeout = params["timeout"].toInt(100);
    const QString writeData = params["writeData"].toString();
    const QString readVariable = params["readVariable"].toString("serial_data");
    const QString operation = params["operation"].toString("WriteRead");

    QSerialPort serial;
    Q_ASSERT_X(serial.thread() == QThread::currentThread(), "SerialPortPlugin::process",
               "SerialPort socket must be created in its execution thread");

    // Data bits
    QSerialPort::DataBits dataBits = QSerialPort::Data8;
    switch (params["dataBits"].toInt(8)) {
    case 5:
        dataBits = QSerialPort::Data5;
        break;
    case 6:
        dataBits = QSerialPort::Data6;
        break;
    case 7:
        dataBits = QSerialPort::Data7;
        break;
    default:
        break;
    }

    // Parity
    const QString parityValue = params["parity"].toString("None");
    QSerialPort::Parity parity = QSerialPort::NoParity;
    if (parityValue == "Even")
        parity = QSerialPort::EvenParity;
    else if (parityValue == "Odd")
        parity = QSerialPort::OddParity;
    else if (parityValue == "Space")
        parity = QSerialPort::SpaceParity;
    else if (parityValue == "Mark")
        parity = QSerialPort::MarkParity;

    // Stop bits
    const QString stopBitsValue = params["stopBits"].toString("One");
    QSerialPort::StopBits stopBits = QSerialPort::OneStop;
    if (stopBitsValue == "OneAndHalf")
        stopBits = QSerialPort::OneAndHalfStop;
    else if (stopBitsValue == "Two")
        stopBits = QSerialPort::TwoStop;

    // Flow control
    const QString flowControlValue = params["flowControl"].toString("None");
    QSerialPort::FlowControl flowControl = QSerialPort::NoFlowControl;
    if (flowControlValue == "Hardware")
        flowControl = QSerialPort::HardwareControl;
    else if (flowControlValue == "Software")
        flowControl = QSerialPort::SoftwareControl;

    if (portName.isEmpty()) {
        emit errorOccurred(tr("串口名称不能为空"));
        return false;
    }

    if (!openSerialPort(serial, portName, baudRate, dataBits, parity, stopBits, flowControl)) {
        return false;
    }

    bool success = true;

    if (operation == "Write" || operation == "WriteRead") {
        QString dataToWrite = writeData;
        if (dataToWrite.isEmpty()) {
            dataToWrite = input.data("send_data").toString();
        }
        if (!dataToWrite.isEmpty()) {
            success = writeToPort(serial, dataToWrite);
            if (success) {
                Logger::instance().debug(QString("SerialPort: Wrote '%1'").arg(dataToWrite), "Communication");
            }
        }
    }

    if (success && (operation == "Read" || operation == "WriteRead")) {
        QString readData = readFromPort(serial, timeout);
        if (!readData.isEmpty()) {
            output.setData(readVariable, readData);
            output.setData(readVariable + "_bytes", readData.toUtf8().size());
            Logger::instance().debug(QString("SerialPort: Read '%1'").arg(readData), "Communication");
        } else {
            emit errorOccurred(tr("串口读取超时，未收到响应"));
            success = false;
        }
    }

    serial.close();
    return success;
}

bool SerialPortPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    // 阶段 2: 串口名称等外部资源允许暂时为空，由 process() 在运行时报告"未配置"
    Q_UNUSED(params)
    error.clear();
    return true;
}

QWidget* SerialPortPlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    // Port name combo
    QComboBox* portCombo = new QComboBox();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& portInfo : ports) {
        portCombo->addItem(portInfo.portName(), portInfo.portName());
    }
    QString currentPort = m_params["portName"].toString();
    int idx = portCombo->findData(currentPort);
    if (idx >= 0)
        portCombo->setCurrentIndex(idx);

    // Baud rate
    QSpinBox* baudSpin = new QSpinBox();
    baudSpin->setRange(110, 4000000);
    baudSpin->setValue(m_params["baudRate"].toInt(9600));
    baudSpin->setPrefix(tr("波特率: "));

    // Data bits
    QComboBox* dataBitsCombo = new QComboBox();
    dataBitsCombo->addItem("5", 5);
    dataBitsCombo->addItem("6", 6);
    dataBitsCombo->addItem("7", 7);
    dataBitsCombo->addItem("8", 8);
    idx = dataBitsCombo->findData(m_params["dataBits"].toInt(8));
    if (idx >= 0)
        dataBitsCombo->setCurrentIndex(idx);

    // Parity
    QComboBox* parityCombo = new QComboBox();
    parityCombo->addItem(tr("无"), "None");
    parityCombo->addItem(tr("偶校验"), "Even");
    parityCombo->addItem(tr("奇校验"), "Odd");
    parityCombo->addItem(tr("Space"), "Space");
    parityCombo->addItem(tr("Mark"), "Mark");
    idx = parityCombo->findData(m_params["parity"].toString("None"));
    if (idx >= 0)
        parityCombo->setCurrentIndex(idx);

    // Stop bits
    QComboBox* stopBitsCombo = new QComboBox();
    stopBitsCombo->addItem("1", "One");
    stopBitsCombo->addItem("1.5", "OneAndHalf");
    stopBitsCombo->addItem("2", "Two");
    idx = stopBitsCombo->findData(m_params["stopBits"].toString("One"));
    if (idx >= 0)
        stopBitsCombo->setCurrentIndex(idx);

    // Flow control
    QComboBox* flowCombo = new QComboBox();
    flowCombo->addItem(tr("无"), "None");
    flowCombo->addItem(tr("硬件"), "Hardware");
    flowCombo->addItem(tr("软件"), "Software");
    idx = flowCombo->findData(m_params["flowControl"].toString("None"));
    if (idx >= 0)
        flowCombo->setCurrentIndex(idx);

    // Operation mode
    QComboBox* operationCombo = new QComboBox();
    operationCombo->addItem(tr("写入"), "Write");
    operationCombo->addItem(tr("读取"), "Read");
    operationCombo->addItem(tr("写入并读取"), "WriteRead");
    idx = operationCombo->findData(m_params["operation"].toString("WriteRead"));
    if (idx >= 0)
        operationCombo->setCurrentIndex(idx);

    // Write data
    QLineEdit* writeEdit = new QLineEdit(m_params["writeData"].toString());

    // Read variable
    QLineEdit* readVarEdit = new QLineEdit(m_params["readVariable"].toString("serial_data"));

    // Timeout
    QSpinBox* timeoutSpin = new QSpinBox();
    timeoutSpin->setRange(10, 30000);
    timeoutSpin->setValue(m_params["timeout"].toInt(100));
    timeoutSpin->setPrefix(tr("超时(ms): "));

    formLayout->addRow(tr("串口:"), portCombo);
    formLayout->addRow(tr("波特率:"), baudSpin);
    formLayout->addRow(tr("数据位:"), dataBitsCombo);
    formLayout->addRow(tr("校验位:"), parityCombo);
    formLayout->addRow(tr("停止位:"), stopBitsCombo);
    formLayout->addRow(tr("流控制:"), flowCombo);
    formLayout->addRow(tr("操作模式:"), operationCombo);
    formLayout->addRow(tr("发送数据:"), writeEdit);
    formLayout->addRow(tr("读取变量:"), readVarEdit);
    formLayout->addRow(tr("超时:"), timeoutSpin);

    layout->addLayout(formLayout);
    layout->addStretch();

    // Connections
    connect(portCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { setParam("portName", portCombo->currentData().toString()); });

    connect(baudSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { setParam("baudRate", value); });

    connect(dataBitsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { setParam("dataBits", dataBitsCombo->currentData().toInt()); });

    connect(parityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { setParam("parity", parityCombo->currentData().toString()); });

    connect(stopBitsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { setParam("stopBits", stopBitsCombo->currentData().toString()); });

    connect(flowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { setParam("flowControl", flowCombo->currentData().toString()); });

    connect(operationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int) { setParam("operation", operationCombo->currentData().toString()); });

    connect(writeEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("writeData", text); });

    connect(readVarEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("readVariable", text); });

    connect(timeoutSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { setParam("timeout", value); });

    return widget;
}

IModule* SerialPortPlugin::cloneImpl() const {
    return new SerialPortPlugin();
}

} // namespace DeepLux
