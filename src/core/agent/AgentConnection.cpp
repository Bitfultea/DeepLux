#include "AgentConnection.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QDebug>

namespace DeepLux {

AgentConnection::AgentConnection(QLocalSocket* socket, const QString& clientId, QObject* parent)
    : QObject(parent)
    , m_socket(socket)
    , m_clientId(clientId)
{
    connect(m_socket, &QLocalSocket::readyRead, this, &AgentConnection::onReadyRead);
    connect(m_socket, &QLocalSocket::disconnected, this, &AgentConnection::onDisconnected);
    connect(m_socket, &QLocalSocket::errorOccurred,
            this, &AgentConnection::onError);
}

AgentConnection::~AgentConnection()
{
    if (m_socket) {
        m_socket->disconnect();
        m_socket->deleteLater();
    }
}

void AgentConnection::send(const QJsonObject& msg)
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) {
        return;
    }

    QJsonDocument doc(msg);
    QByteArray data = doc.toJson(QJsonDocument::Compact) + "\n";
    m_socket->write(data);
    m_socket->flush();
}

void AgentConnection::onReadyRead()
{
    if (!m_socket) return;

    m_buffer.append(m_socket->readAll());

    // 解析 JSON 行（每行一个 JSON 对象）
    parseBuffer();
}

void AgentConnection::onDisconnected()
{
    qDebug() << "Agent connection disconnected:" << m_clientId;
    emit disconnected(m_clientId);
}

void AgentConnection::onError(QLocalSocket::LocalSocketError socketError)
{
    Q_UNUSED(socketError);
    qWarning() << "Agent connection error:" << m_socket->errorString();
    emit error(m_socket->errorString());
}

void AgentConnection::parseBuffer()
{
    // 按行分割，解析 JSON
    while (m_buffer.contains('\n')) {
        int newlineIndex = m_buffer.indexOf('\n');
        QByteArray line = m_buffer.left(newlineIndex);
        m_buffer = m_buffer.mid(newlineIndex + 1);

        if (line.isEmpty()) continue;

        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);

        if (parseError.error == QJsonParseError::NoError) {
            emit messageReceived(m_clientId, doc.object());
        } else {
            qWarning() << "Failed to parse JSON from agent:" << parseError.errorString();
        }
    }
}

} // namespace DeepLux
