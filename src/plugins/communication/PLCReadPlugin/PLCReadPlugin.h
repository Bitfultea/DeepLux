#pragma once

#include "core/base/ModuleBase.h"

#include <QVector>

namespace DeepLux {

class DEEPLUX_PLUGIN_API PLCReadPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit PLCReadPlugin(QObject* parent = nullptr);
    ~PLCReadPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.plcread";
    }
    QString name() const override {
        return tr("PLC读取");
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
        return tr("通过Modbus TCP读取PLC保持寄存器");
    }

    bool initialize() override;
    QWidget* createConfigWidget() override;

    int lastWaitMs() const {
        return m_lastWaitMs;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    QByteArray buildReadRequest(quint16 transactionId, quint8 unitId, quint16 startAddress, quint16 readCount);
    QVector<quint16> parseReadResponse(const QByteArray& data);

    quint8 m_slaveId;
    quint16 m_readCount;
    int m_lastWaitMs = 0;
    quint16 m_transactionId = 0;
};

} // namespace DeepLux
