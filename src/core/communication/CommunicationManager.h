#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSerialPort>

namespace DeepLux {

/**
 * @brief 通讯类型
 */
enum class CommunicationType {
    TCP_Server,
    TCP_Client,
    SerialPort,
    PLC
};

/**
 * @brief 通讯状态
 */
enum class CommunicationState {
    Disconnected,
    Connecting,
    Connected,
    Listening,
    Error
};

/**
 * @brief 通讯配置
 */
struct CommunicationConfig {
    QString id;
    QString name;
    CommunicationType type;

    // TCP配置
    QString ipAddress;
    int port = 0;

    // 串口配置
    QString portName;
    int baudRate = 115200;
    int dataBits = 8;
    int stopBits = 1;
    QString parity = "None";

    QJsonObject extraConfig;
};

/**
 * @brief 通讯管理器
 */
class CommunicationManager : public QObject
{
    Q_OBJECT

public:
    static CommunicationManager& instance();

    // 通讯配置列表
    QList<CommunicationConfig> configs() const { return m_configs; }
    CommunicationConfig* findConfig(const QString& configId);

    // 连接管理
    bool connect(const QString& configId);
    void disconnect(const QString& configId);
    void disconnectAll();

    // 状态
    CommunicationState state(const QString& configId) const;
    bool isConnected(const QString& configId) const;

    // 数据收发
    bool sendData(const QString& configId, const QByteArray& data);
    bool sendString(const QString& configId, const QString& data);

signals:
    void connectionStateChanged(const QString& configId, CommunicationState state);
    void dataReceived(const QString& configId, const QByteArray& data);
    void errorOccurred(const QString& configId, const QString& error);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    CommunicationManager();
    ~CommunicationManager();

    CommunicationConfig* findConfigBySocket(QTcpSocket* socket);

    QList<CommunicationConfig> m_configs;
    QMap<QString, CommunicationState> m_states;
    QMap<QString, QTcpSocket*> m_tcpSockets;
    QMap<QString, QTcpServer*> m_tcpServers;
    QMap<QString, QSerialPort*> m_serialPorts;
};

} // namespace DeepLux