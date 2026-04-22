#include "CommunicationManager.h"
#include "common/Logger.h"
#include <QTimer>

namespace DeepLux {

CommunicationManager& CommunicationManager::instance()
{
    static CommunicationManager instance;
    return instance;
}

CommunicationManager::CommunicationManager()
{
    Logger::instance().info(tr("Communication manager initialized"), "Comm");
}

CommunicationManager::~CommunicationManager()
{
    disconnectAll();
}

CommunicationConfig* CommunicationManager::findConfig(const QString& configId)
{
    for (auto& config : m_configs) {
        if (config.id == configId) {
            return &config;
        }
    }
    return nullptr;
}

bool CommunicationManager::connect(const QString& configId)
{
    CommunicationConfig* config = findConfig(configId);
    if (!config) {
        Logger::instance().error(QString("Communication config not found: %1").arg(configId), "Comm");
        return false;
    }

    if (isConnected(configId)) {
        return true;
    }

    emit connectionStateChanged(configId, CommunicationState::Connecting);
    Logger::instance().info(QString("Connecting: %1 (%2)").arg(config->name).arg(configId), "Comm");

    switch (config->type) {
        case CommunicationType::TCP_Client: {
            QTcpSocket* socket = new QTcpSocket(this);
            m_tcpSockets[configId] = socket;

            QObject::connect(socket, &QTcpSocket::connected, this, [this, configId, config]() {
                m_states[configId] = CommunicationState::Connected;
                emit connectionStateChanged(configId, CommunicationState::Connected);
                Logger::instance().success(QString("Connected to TCP server: %1:%2").arg(config->ipAddress).arg(config->port), "Comm");
            });

            QObject::connect(socket, &QTcpSocket::readyRead, this, &CommunicationManager::onReadyRead);
            QObject::connect(socket, &QTcpSocket::disconnected, this, &CommunicationManager::onDisconnected);

            socket->connectToHost(config->ipAddress, config->port);
            break;
        }

        case CommunicationType::TCP_Server: {
            QTcpServer* server = new QTcpServer(this);
            m_tcpServers[configId] = server;

            QObject::connect(server, &QTcpServer::newConnection, this, &CommunicationManager::onNewConnection);

            if (server->listen(QHostAddress::Any, config->port)) {
                m_states[configId] = CommunicationState::Listening;
                emit connectionStateChanged(configId, CommunicationState::Listening);
                Logger::instance().success(QString("TCP server listening on port: %1").arg(config->port), "Comm");
            } else {
                emit errorOccurred(configId, server->errorString());
                Logger::instance().error(QString("Failed to start TCP server: %1").arg(server->errorString()), "Comm");
            }
            break;
        }

        case CommunicationType::SerialPort: {
            QSerialPort* serial = new QSerialPort(this);
            m_serialPorts[configId] = serial;

            serial->setPortName(config->portName);
            serial->setBaudRate(config->baudRate);
            serial->setDataBits(static_cast<QSerialPort::DataBits>(config->dataBits));
            serial->setStopBits(static_cast<QSerialPort::StopBits>(config->stopBits));

            if (config->parity == "Even") {
                serial->setParity(QSerialPort::EvenParity);
            } else if (config->parity == "Odd") {
                serial->setParity(QSerialPort::OddParity);
            } else {
                serial->setParity(QSerialPort::NoParity);
            }

            QObject::connect(serial, &QSerialPort::readyRead, this, &CommunicationManager::onReadyRead);

            if (serial->open(QIODevice::ReadWrite)) {
                m_states[configId] = CommunicationState::Connected;
                emit connectionStateChanged(configId, CommunicationState::Connected);
                Logger::instance().success(QString("Serial port opened: %1").arg(config->portName), "Comm");
            } else {
                emit errorOccurred(configId, serial->errorString());
                Logger::instance().error(QString("Failed to open serial port: %1").arg(serial->errorString()), "Comm");
            }
            break;
        }

        case CommunicationType::PLC:
            // PLC通讯需要专门的实现
            Logger::instance().warning("PLC communication not implemented", "Comm");
            break;
    }

    return true;
}

void CommunicationManager::disconnect(const QString& configId)
{
    CommunicationConfig* config = findConfig(configId);
    if (!config) {
        return;
    }

    if (m_tcpSockets.contains(configId)) {
        m_tcpSockets[configId]->disconnectFromHost();
        delete m_tcpSockets[configId];
        m_tcpSockets.remove(configId);
    }

    if (m_tcpServers.contains(configId)) {
        m_tcpServers[configId]->close();
        delete m_tcpServers[configId];
        m_tcpServers.remove(configId);
    }

    if (m_serialPorts.contains(configId)) {
        m_serialPorts[configId]->close();
        delete m_serialPorts[configId];
        m_serialPorts.remove(configId);
    }

    m_states[configId] = CommunicationState::Disconnected;
    emit connectionStateChanged(configId, CommunicationState::Disconnected);
    Logger::instance().info(QString("Disconnected: %1").arg(config->name), "Comm");
}

void CommunicationManager::disconnectAll()
{
    for (const auto& config : m_configs) {
        disconnect(config.id);
    }
}

CommunicationState CommunicationManager::state(const QString& configId) const
{
    return m_states.value(configId, CommunicationState::Disconnected);
}

bool CommunicationManager::isConnected(const QString& configId) const
{
    CommunicationState s = state(configId);
    return s == CommunicationState::Connected || s == CommunicationState::Listening;
}

bool CommunicationManager::sendData(const QString& configId, const QByteArray& data)
{
    if (m_tcpSockets.contains(configId)) {
        qint64 written = m_tcpSockets[configId]->write(data);
        return written == data.size();
    }

    if (m_serialPorts.contains(configId)) {
        qint64 written = m_serialPorts[configId]->write(data);
        return written == data.size();
    }

    return false;
}

bool CommunicationManager::sendString(const QString& configId, const QString& data)
{
    return sendData(configId, data.toUtf8());
}

void CommunicationManager::onNewConnection()
{
    QTcpServer* server = qobject_cast<QTcpServer*>(sender());
    if (!server) return;

    while (server->hasPendingConnections()) {
        QTcpSocket* socket = server->nextPendingConnection();
        QString tempId = QString("tcp_%1").arg(socket->socketDescriptor());

        QObject::connect(socket, &QTcpSocket::readyRead, this, &CommunicationManager::onReadyRead);
        QObject::connect(socket, &QTcpSocket::disconnected, this, &CommunicationManager::onDisconnected);

        Logger::instance().info(QString("New TCP connection: %1").arg(tempId), "Comm");
    }
}

void CommunicationManager::onReadyRead()
{
    QIODevice* device = qobject_cast<QIODevice*>(sender());
    if (!device) return;

    QByteArray data = device->readAll();
    QString configId;

    // 查找对应的configId
    for (auto it = m_tcpSockets.begin(); it != m_tcpSockets.end(); ++it) {
        if (it.value() == device) {
            configId = it.key();
            break;
        }
    }

    for (auto it = m_serialPorts.begin(); it != m_serialPorts.end(); ++it) {
        if (it.value() == device) {
            configId = it.key();
            break;
        }
    }

    if (!configId.isEmpty()) {
        emit dataReceived(configId, data);
    }
}

void CommunicationManager::onDisconnected()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    QString configId;
    for (auto it = m_tcpSockets.begin(); it != m_tcpSockets.end(); ++it) {
        if (it.value() == socket) {
            configId = it.key();
            break;
        }
    }

    if (!configId.isEmpty()) {
        m_states[configId] = CommunicationState::Disconnected;
        emit connectionStateChanged(configId, CommunicationState::Disconnected);
    }

    socket->deleteLater();
}

} // namespace DeepLux