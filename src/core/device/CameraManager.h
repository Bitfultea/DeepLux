#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QMap>
#include <QJsonObject>
#include <QDateTime>
#include <QMutex>

namespace DeepLux {

class ICamera;
class ICameraPlugin;

/**
 * @brief 相机状态
 */
enum class CameraState {
    Disconnected,
    Connecting,
    Connected,
    Grabbing,
    Error
};

/**
 * @brief 相机状态信息
 */
struct CameraStatus {
    QString deviceId;
    QString name;
    CameraState state = CameraState::Disconnected;
    QDateTime lastGrabTime;
    QString lastError;
};

/**
 * @brief 相机管理器 - 单例模式
 *
 * 管理所有相机插件和相机实例
 */
class CameraManager : public QObject
{
    Q_OBJECT

public:
    static CameraManager& instance();

    // ========== 插件管理 ==========

    void registerCameraPlugin(ICameraPlugin* plugin);
    void unregisterCameraPlugin(const QString& pluginId);
    QList<ICameraPlugin*> plugins() const;
    ICameraPlugin* findPlugin(const QString& pluginId) const;

    // ========== 相机发现 ==========

    QList<CameraStatus> discoverCameras();
    void refreshCameras();

    // ========== 连接管理 ==========

    bool connectCamera(const QString& deviceId);
    void disconnectCamera(const QString& deviceId);
    void disconnectAll();

    // ========== 相机访问 ==========

    ICamera* getCamera(const QString& deviceId) const;
    QList<ICamera*> allCameras() const;

    // ========== 状态查询 ==========

    bool isConnected(const QString& deviceId) const;
    CameraState cameraState(const QString& deviceId) const;
    QString lastError(const QString& deviceId) const;

signals:
    void pluginRegistered(const QString& pluginId);
    void pluginUnregistered(const QString& pluginId);
    void cameraDiscovered(const QString& deviceId, const QString& name);
    void cameraConnected(const QString& deviceId);
    void cameraDisconnected(const QString& deviceId);
    void cameraStateChanged(const QString& deviceId, CameraState state);
    void errorOccurred(const QString& deviceId, const QString& error);

private:
    CameraManager();
    ~CameraManager();

    Q_DISABLE_COPY(CameraManager)

    QMap<QString, ICameraPlugin*> m_plugins;       // pluginId -> plugin
    QMap<QString, ICamera*> m_cameras;             // deviceId -> camera instance
    QMap<QString, CameraStatus> m_cameraStatuses;  // deviceId -> status
    QMutex m_mutex;
};

} // namespace DeepLux
