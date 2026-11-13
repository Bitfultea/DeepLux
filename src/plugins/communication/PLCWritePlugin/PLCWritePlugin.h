#pragma once

#include "core/base/ModuleBase.h"

#include <QTcpSocket>

namespace DeepLux {

class DEEPLUX_PLUGIN_API PLCWritePlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DEEPLUX_MODULE_INTERFACE_IID FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit PLCWritePlugin(QObject* parent = nullptr);
    ~PLCWritePlugin() override;
    using ModuleBase::execute;

    QString moduleId() const override {
        return "com.deeplux.plugin.plcwrite";
    }
    QString name() const override {
        return tr("PLC写入");
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
        return tr("通过Modbus TCP写入PLC保持寄存器");
    }

    bool initialize() override;
    QWidget* createConfigWidget() override;
    ExecutionResult execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) override;

    int lastWaitMs() const {
        return m_lastWaitMs;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool connectToHost(QTcpSocket& socket);
    void disconnectFromHost(QTcpSocket& socket);
    QByteArray buildWriteRequest(quint16 transactionId, quint8 unitId, quint16 startAddress,
                                 const QVector<quint16>& values);
    bool parseWriteResponse(const QByteArray& data);

    QByteArray m_readBuffer;
    QString m_host;
    quint16 m_port;
    quint8 m_slaveId;
    quint16 m_startAddress;
    QVector<quint16> m_values;
    QString m_valueSource;
    int m_timeout;
    int m_lastWaitMs = 0;
    bool m_writeRequestAccepted = false;
    quint16 m_transactionId = 0;
};

} // namespace DeepLux
