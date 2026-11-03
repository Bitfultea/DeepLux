#pragma once

#include "core/base/ModuleBase.h"

#include <QTcpSocket>

namespace DeepLux {

class PLCCommunicatePlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit PLCCommunicatePlugin(QObject* parent = nullptr);
    ~PLCCommunicatePlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.plccommunicate";
    }
    QString name() const override {
        return tr("PLC通信测试");
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
        return tr("测试PLC Modbus TCP连接是否正常");
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
    bool connectToHost(QTcpSocket& socket, const QString& host, quint16 port, int timeoutMs);
    void disconnectFromHost(QTcpSocket& socket);
    QByteArray buildReadRequest(quint16 transactionId, quint8 unitId, quint16 startAddress, quint16 readCount);
    bool parseReadResponse(const QByteArray& data);

    QString m_host;
    quint16 m_port;
    int m_timeout;
    int m_lastWaitMs = 0;
    quint16 m_transactionId = 0;
};

} // namespace DeepLux
