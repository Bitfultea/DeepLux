#pragma once

#include "core/base/ModuleBase.h"
#include <QSerialPort>
#include <QSerialPortInfo>

namespace DeepLux {

class SerialPortPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit SerialPortPlugin(QObject* parent = nullptr);
    ~SerialPortPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.serialport"; }
    QString name() const override { return tr("串口通信"); }
    QString category() const override { return "communication"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("串口数据读写"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    bool openSerialPort();
    void closeSerialPort();
    QString readFromPort();
    bool writeToPort(const QString& data);

    QSerialPort* m_serialPort;
    QString m_portName;
    int m_baudRate;
    QSerialPort::DataBits m_dataBits;
    QSerialPort::Parity m_parity;
    QSerialPort::StopBits m_stopBits;
    QSerialPort::FlowControl m_flowControl;
    int m_timeout;
    QString m_writeData;
    QString m_readVariable;
};

} // namespace DeepLux
