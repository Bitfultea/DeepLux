#pragma once

#include "core/base/ModuleBase.h"
#include <QTcpSocket>
#include <QTimer>
#include <QEventLoop>

namespace DeepLux {

class TCPClientPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit TCPClientPlugin(QObject* parent = nullptr);
    ~TCPClientPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.tcpclient"; }
    QString name() const override { return tr("TCP客户端"); }
    QString category() const override { return "communication"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("TCP客户端数据读写"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

    /// Number of milliseconds waited during last connect/read
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
    bool connectToServerAsync();
    void disconnectFromServer();
    QString readFromServerAsync();
    bool writeToServer(const QString& data);

    QTcpSocket* m_socket;
    QTimer* m_timeoutTimer;
    QEventLoop* m_connectLoop;
    QEventLoop* m_readLoop;
    QString m_host;
    quint16 m_port;
    int m_timeout;
    QString m_writeData;
    QString m_readVariable;
    int m_lastWaitMs = 0;

    // Async read state
    QByteArray m_readBuffer;
    bool m_readTimedOut = false;
};

} // namespace DeepLux
