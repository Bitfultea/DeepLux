#ifndef DEEPLUX_AGENT_CONNECTION_H
#define DEEPLUX_AGENT_CONNECTION_H

#include <QObject>
#include <QLocalSocket>
#include <QJsonObject>

namespace DeepLux {

/**
 * @brief Agent 连接类 - 封装与单个 Agent 的通信
 */
class AgentConnection : public QObject
{
    Q_OBJECT

public:
    explicit AgentConnection(QLocalSocket* socket, const QString& clientId, QObject* parent = nullptr);
    ~AgentConnection() override;

    void send(const QJsonObject& msg);
    QString clientId() const { return m_clientId; }
    bool isConnected() const { return m_socket && m_socket->state() == QLocalSocket::ConnectedState; }

signals:
    void messageReceived(const QString& clientId, const QJsonObject& msg);
    void disconnected(const QString& clientId);
    void error(const QString& error);

private slots:
    void onReadyRead();
    void onDisconnected();
    void onError(QLocalSocket::LocalSocketError error);

private:
    void parseBuffer();

    QLocalSocket* m_socket = nullptr;
    QString m_clientId;
    QByteArray m_buffer;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CONNECTION_H
