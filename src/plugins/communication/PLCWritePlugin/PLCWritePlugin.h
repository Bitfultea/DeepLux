#pragma once

#include "core/base/ModuleBase.h"
#include <QTcpSocket>
#include <QTimer>
#include <QEventLoop>

namespace DeepLux {

class PLCWritePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit PLCWritePlugin(QObject* parent = nullptr);
    ~PLCWritePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.plcwrite"; }
    QString name() const override { return tr("PLC写入"); }
    QString category() const override { return "communication"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("通过Modbus TCP写入PLC保持寄存器"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

    int lastWaitMs() const { return m_lastWaitMs; }

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private slots:
    void onConnected();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);
    void onReadyRead();
    void onConnectTimeout();
    void onReadTimeout();

private:
    bool connectToHost();
    void disconnectFromHost();
    QByteArray buildWriteRequest(quint16 transactionId, quint8 unitId, quint16 startAddress, const QVector<quint16>& values);
    bool parseWriteResponse(const QByteArray& data);

    QTcpSocket* m_socket;
    QByteArray m_readBuffer;
    QEventLoop* m_connectLoop;
    QEventLoop* m_readLoop;
    QString m_host;
    quint16 m_port;
    quint8 m_slaveId;
    quint16 m_startAddress;
    QVector<quint16> m_values;
    QString m_valueSource;
    int m_timeout;
    int m_lastWaitMs = 0;
    bool m_readTimedOut = false;
    quint16 m_transactionId = 0;
};

} // namespace DeepLux
