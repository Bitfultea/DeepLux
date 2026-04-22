#include "SerialPortPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QSerialPortInfo>

namespace DeepLux {

SerialPortPlugin::SerialPortPlugin(QObject* parent)
    : ModuleBase(parent)
    , m_serialPort(new QSerialPort(this))
{
    m_defaultParams = QJsonObject{
        {"portName", ""},
        {"baudRate", 9600},
        {"dataBits", 8},
        {"parity", "None"},
        {"stopBits", "One"},
        {"flowControl", "None"},
        {"timeout", 100},
        {"writeData", ""},
        {"readVariable", "serial_data"},
        {"operation", "WriteRead"}
    };
    m_params = m_defaultParams;
}

SerialPortPlugin::~SerialPortPlugin()
{
    closeSerialPort();
}

bool SerialPortPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "SerialPortPlugin initialized";
    return true;
}

bool SerialPortPlugin::openSerialPort()
{
    if (m_serialPort->isOpen()) {
        m_serialPort->close();
    }

    m_serialPort->setPortName(m_portName);
    m_serialPort->setBaudRate(m_baudRate);
    m_serialPort->setDataBits(m_dataBits);
    m_serialPort->setParity(m_parity);
    m_serialPort->setStopBits(m_stopBits);
    m_serialPort->setFlowControl(m_flowControl);

    if (!m_serialPort->open(QIODevice::ReadWrite)) {
        emit errorOccurred(tr("无法打开串口 %1: %2").arg(m_portName).arg(m_serialPort->errorString()));
        return false;
    }

    Logger::instance().info(QString("SerialPort: Opened %1 at %2 baud").arg(m_portName).arg(m_baudRate), "Communication");
    return true;
}

void SerialPortPlugin::closeSerialPort()
{
    if (m_serialPort->isOpen()) {
        m_serialPort->close();
        Logger::instance().info(QString("SerialPort: Closed %1").arg(m_portName), "Communication");
    }
}

QString SerialPortPlugin::readFromPort()
{
    if (!m_serialPort->waitForReadyRead(m_timeout)) {
        return QString();
    }

    QByteArray data = m_serialPort->readAll();
    while (m_serialPort->waitForReadyRead(10)) {
        data += m_serialPort->readAll();
    }

    return QString::fromUtf8(data);
}

bool SerialPortPlugin::writeToPort(const QString& data)
{
    if (!m_serialPort->isOpen()) {
        emit errorOccurred(tr("串口未打开"));
        return false;
    }

    QByteArray bytes = data.toUtf8();
    qint64 written = m_serialPort->write(bytes);
    if (written != bytes.size()) {
        emit errorOccurred(tr("写入串口失败"));
        return false;
    }

    m_serialPort->flush();
    return true;
}

bool SerialPortPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_portName = m_params["portName"].toString();
    m_baudRate = m_params["baudRate"].toInt(9600);
    m_timeout = m_params["timeout"].toInt(100);
    m_writeData = m_params["writeData"].toString();
    m_readVariable = m_params["readVariable"].toString("serial_data");

    // Data bits
    int dataBits = m_params["dataBits"].toInt(8);
    switch (dataBits) {
        case 5: m_dataBits = QSerialPort::Data5; break;
        case 6: m_dataBits = QSerialPort::Data6; break;
        case 7: m_dataBits = QSerialPort::Data7; break;
        default: m_dataBits = QSerialPort::Data8; break;
    }

    // Parity
    QString parity = m_params["parity"].toString("None");
    if (parity == "Even") m_parity = QSerialPort::EvenParity;
    else if (parity == "Odd") m_parity = QSerialPort::OddParity;
    else if (parity == "Space") m_parity = QSerialPort::SpaceParity;
    else if (parity == "Mark") m_parity = QSerialPort::MarkParity;
    else m_parity = QSerialPort::NoParity;

    // Stop bits
    QString stopBits = m_params["stopBits"].toString("One");
    if (stopBits == "OneAndHalf") m_stopBits = QSerialPort::OneAndHalfStop;
    else if (stopBits == "Two") m_stopBits = QSerialPort::TwoStop;
    else m_stopBits = QSerialPort::OneStop;

    // Flow control
    QString flowControl = m_params["flowControl"].toString("None");
    if (flowControl == "Hardware") m_flowControl = QSerialPort::HardwareControl;
    else if (flowControl == "Software") m_flowControl = QSerialPort::SoftwareControl;
    else m_flowControl = QSerialPort::NoFlowControl;

    QString operation = m_params["operation"].toString("WriteRead");

    if (m_portName.isEmpty()) {
        emit errorOccurred(tr("串口名称不能为空"));
        return false;
    }

    if (!openSerialPort()) {
        return false;
    }

    bool success = true;

    if (operation == "Write" || operation == "WriteRead") {
        QString dataToWrite = m_writeData;
        if (dataToWrite.isEmpty()) {
            dataToWrite = input.data("send_data").toString();
        }
        if (!dataToWrite.isEmpty()) {
            success = writeToPort(dataToWrite);
            if (success) {
                Logger::instance().debug(QString("SerialPort: Wrote '%1'").arg(dataToWrite), "Communication");
            }
        }
    }

    if (success && (operation == "Read" || operation == "WriteRead")) {
        QString readData = readFromPort();
        if (!readData.isEmpty()) {
            output.setData(m_readVariable, readData);
            output.setData(m_readVariable + "_bytes", readData.toUtf8().size());
            Logger::instance().debug(QString("SerialPort: Read '%1'").arg(readData), "Communication");
        }
    }

    closeSerialPort();
    return success;
}

bool SerialPortPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["portName"].toString().isEmpty()) {
        error = QString("Port name cannot be empty");
        return false;
    }
    return true;
}

QWidget* SerialPortPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    // Port name combo
    QComboBox* portCombo = new QComboBox();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &portInfo : ports) {
        portCombo->addItem(portInfo.portName(), portInfo.portName());
    }
    QString currentPort = m_params["portName"].toString();
    int idx = portCombo->findData(currentPort);
    if (idx >= 0) portCombo->setCurrentIndex(idx);

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
    if (idx >= 0) dataBitsCombo->setCurrentIndex(idx);

    // Parity
    QComboBox* parityCombo = new QComboBox();
    parityCombo->addItem(tr("无"), "None");
    parityCombo->addItem(tr("偶校验"), "Even");
    parityCombo->addItem(tr("奇校验"), "Odd");
    parityCombo->addItem(tr("Space"), "Space");
    parityCombo->addItem(tr("Mark"), "Mark");
    idx = parityCombo->findData(m_params["parity"].toString("None"));
    if (idx >= 0) parityCombo->setCurrentIndex(idx);

    // Stop bits
    QComboBox* stopBitsCombo = new QComboBox();
    stopBitsCombo->addItem("1", "One");
    stopBitsCombo->addItem("1.5", "OneAndHalf");
    stopBitsCombo->addItem("2", "Two");
    idx = stopBitsCombo->findData(m_params["stopBits"].toString("One"));
    if (idx >= 0) stopBitsCombo->setCurrentIndex(idx);

    // Flow control
    QComboBox* flowCombo = new QComboBox();
    flowCombo->addItem(tr("无"), "None");
    flowCombo->addItem(tr("硬件"), "Hardware");
    flowCombo->addItem(tr("软件"), "Software");
    idx = flowCombo->findData(m_params["flowControl"].toString("None"));
    if (idx >= 0) flowCombo->setCurrentIndex(idx);

    // Operation mode
    QComboBox* operationCombo = new QComboBox();
    operationCombo->addItem(tr("写入"), "Write");
    operationCombo->addItem(tr("读取"), "Read");
    operationCombo->addItem(tr("写入并读取"), "WriteRead");
    idx = operationCombo->findData(m_params["operation"].toString("WriteRead"));
    if (idx >= 0) operationCombo->setCurrentIndex(idx);

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
    connect(portCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["portName"] = portCombo->currentData().toString();
    });

    connect(baudSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["baudRate"] = value;
    });

    connect(dataBitsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["dataBits"] = dataBitsCombo->currentData().toInt();
    });

    connect(parityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["parity"] = parityCombo->currentData().toString();
    });

    connect(stopBitsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["stopBits"] = stopBitsCombo->currentData().toString();
    });

    connect(flowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["flowControl"] = flowCombo->currentData().toString();
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

    connect(timeoutSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["timeout"] = value;
    });

    return widget;
}

} // namespace DeepLux
