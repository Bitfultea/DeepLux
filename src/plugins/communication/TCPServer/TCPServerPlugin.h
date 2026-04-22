#pragma once

#include "core/base/ModuleBase.h"
#include <QTcpServer>
#include <QTcpSocket>

namespace DeepLux {

class TCPServerPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit TCPServerPlugin(QObject* parent = nullptr);
    ~TCPServerPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.tcpserver"; }
    QString name() const override { return tr("TCP服务器"); }
    QString category() const override { return "communication"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("TCP服务器数据收发"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private slots:
    void onNewConnection();
    void onDisconnected();
    void onReadyRead();

private:
    bool startServer();
    void stopServer();
    bool writeToClient(const QString& data);
    QString readFromClient();

    QTcpServer* m_server;
    QTcpSocket* m_clientSocket;
    quint16 m_port;
    int m_timeout;
    QString m_writeData;
    QString m_readVariable;
};

} // namespace DeepLux
