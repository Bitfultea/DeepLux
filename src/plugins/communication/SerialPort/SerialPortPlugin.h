#pragma once

#include "core/base/ModuleBase.h"

#include <QSerialPort>
#include <QSerialPortInfo>

namespace DeepLux {

class DEEPLUX_PLUGIN_API SerialPortPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DEEPLUX_MODULE_INTERFACE_IID FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit SerialPortPlugin(QObject* parent = nullptr);
    ~SerialPortPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.serialport";
    }
    QString name() const override {
        return tr("串口通信");
    }
    QString category() const override {
        return "communication";
    }
    QString version() const override {
        return "1.0.0";
    }
    QString author() const override {
        return "DeepLux Team";
    }
    QString description() const override {
        return tr("串口数据读写");
    }

    bool initialize() override;
    QWidget* createConfigWidget() override;
    using ModuleBase::execute;
    ExecutionResult execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool openSerialPort(QSerialPort& serial, const QString& portName, int baudRate, QSerialPort::DataBits dataBits,
                        QSerialPort::Parity parity, QSerialPort::StopBits stopBits,
                        QSerialPort::FlowControl flowControl);
    QString readFromPort(QSerialPort& serial, int timeoutMs);
    bool writeToPort(QSerialPort& serial, const QString& data);

    bool m_writeMayHaveOccurred = false;
};

} // namespace DeepLux
