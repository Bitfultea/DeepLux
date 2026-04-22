#include "CameraManager.h"
#include "core/interface/ICameraPlugin.h"
#include "core/interface/ICamera.h"
#include "core/common/Logger.h"

#include <QCoreApplication>

namespace DeepLux {

CameraManager& CameraManager::instance()
{
    static CameraManager instance;
    return instance;
}

CameraManager::CameraManager()
{
    Logger::instance().info("Camera manager initialized", "Camera");
}

CameraManager::~CameraManager()
{
    disconnectAll();
}

void CameraManager::registerCameraPlugin(ICameraPlugin* plugin)
{
    if (!plugin) return;

    QMutexLocker locker(&m_mutex);

    QString pluginId = plugin->pluginId();
    if (m_plugins.contains(pluginId)) {
        Logger::instance().warning(QString("Camera plugin already registered: %1").arg(pluginId), "Camera");
        return;
    }

    m_plugins.insert(pluginId, plugin);
    Logger::instance().info(QString("Camera plugin registered: %1 (%2)")
        .arg(plugin->pluginName())
        .arg(plugin->manufacturer()), "Camera");

    // 如果插件可用，立即发现相机
    if (plugin->isAvailable()) {
        QList<CameraInfo> cameras = plugin->discoverCameras();
        for (const CameraInfo& info : cameras) {
            CameraStatus status;
            status.deviceId = info.deviceId;
            status.name = info.name;
            status.state = CameraState::Disconnected;
            m_cameraStatuses.insert(info.deviceId, status);

            emit cameraDiscovered(info.deviceId, info.name);
            Logger::instance().info(
                QString("Camera discovered: %1 (%2) - %3")
                    .arg(info.name).arg(info.deviceId).arg(plugin->pluginName()),
                "Camera");
        }
    }

    emit pluginRegistered(pluginId);
}

void CameraManager::unregisterCameraPlugin(const QString& pluginId)
{
    QMutexLocker locker(&m_mutex);

    if (!m_plugins.contains(pluginId)) {
        return;
    }

    // 断开该插件的所有相机
    for (auto it = m_cameras.begin(); it != m_cameras.end(); ) {
        ICamera* camera = it.value();
        if (camera) {
            camera->disconnect();
            delete camera;
        }
        it = m_cameras.erase(it);
    }

    m_plugins.remove(pluginId);
    Logger::instance().info(QString("Camera plugin unregistered: %1").arg(pluginId), "Camera");
    emit pluginUnregistered(pluginId);
}

QList<ICameraPlugin*> CameraManager::plugins() const
{
    return m_plugins.values();
}

ICameraPlugin* CameraManager::findPlugin(const QString& pluginId) const
{
    return m_plugins.value(pluginId);
}

QList<CameraStatus> CameraManager::discoverCameras()
{
    QMutexLocker locker(&m_mutex);

    QList<CameraStatus> discovered;

    for (ICameraPlugin* plugin : m_plugins) {
        if (!plugin->isAvailable()) {
            Logger::instance().debug(
                QString("Plugin not available: %1 - %2")
                    .arg(plugin->pluginName())
                    .arg(plugin->availabilityMessage()),
                "Camera");
            continue;
        }

        QList<CameraInfo> cameras = plugin->discoverCameras();
        for (const CameraInfo& info : cameras) {
            // 更新状态
            if (m_cameraStatuses.contains(info.deviceId)) {
                CameraStatus& status = m_cameraStatuses[info.deviceId];
                status.name = info.name;
            } else {
                CameraStatus status;
                status.deviceId = info.deviceId;
                status.name = info.name;
                status.state = CameraState::Disconnected;
                m_cameraStatuses.insert(info.deviceId, status);
            }

            discovered.append(m_cameraStatuses.value(info.deviceId));
        }
    }

    Logger::instance().info(QString("Discovered %1 camera(s)").arg(discovered.size()), "Camera");
    return discovered;
}

void CameraManager::refreshCameras()
{
    // 清除旧的未连接相机状态
    QMutexLocker locker(&m_mutex);

    for (auto it = m_cameraStatuses.begin(); it != m_cameraStatuses.end(); ) {
        if (it.value().state == CameraState::Disconnected) {
            it = m_cameraStatuses.erase(it);
        } else {
            ++it;
        }
    }

    // 重新发现
    discoverCameras();
}

bool CameraManager::connectCamera(const QString& deviceId)
{
    QMutexLocker locker(&m_mutex);

    // 如果已连接，直接返回
    if (m_cameras.contains(deviceId)) {
        ICamera* camera = m_cameras.value(deviceId);
        if (camera && camera->isConnected()) {
            return true;
        }
    }

    // 查找相机所属的插件
    ICameraPlugin* targetPlugin = nullptr;
    CameraInfo targetInfo;

    Logger::instance().debug(QString("Connecting camera: %1, registered plugins: %2").arg(deviceId).arg(m_plugins.size()), "Camera");

    for (ICameraPlugin* plugin : m_plugins) {
        QString pluginId = plugin->pluginId();
        bool available = plugin->isAvailable();
        QString availMsg = plugin->availabilityMessage();
        Logger::instance().debug(QString("Checking plugin: %1, available=%2, msg=%3").arg(pluginId).arg(available).arg(availMsg), "Camera");

        if (!available) {
            Logger::instance().warning(QString("Plugin %1 not available: %2").arg(pluginId).arg(availMsg), "Camera");
            continue;
        }

        QList<CameraInfo> cameras = plugin->discoverCameras();
        Logger::instance().debug(QString("Plugin %1 discovered %2 camera(s)").arg(pluginId).arg(cameras.size()), "Camera");

        for (const CameraInfo& info : cameras) {
            Logger::instance().debug(QString("  - %1 (%2)").arg(info.name).arg(info.deviceId), "Camera");
            if (info.deviceId == deviceId) {
                targetPlugin = plugin;
                targetInfo = info;
                break;
            }
        }
        if (targetPlugin) break;
    }

    if (!targetPlugin) {
        QString error = QString("Camera not found: %1. Ensure the camera plugin is loaded and the device is powered/on the network.").arg(deviceId);
        Logger::instance().error(error, "Camera");
        if (m_cameraStatuses.contains(deviceId)) {
            m_cameraStatuses[deviceId].lastError = error;
        } else {
            CameraStatus st;
            st.deviceId = deviceId;
            st.lastError = error;
            m_cameraStatuses.insert(deviceId, st);
        }
        emit errorOccurred(deviceId, error);
        return false;
    }

    // 创建相机实例
    QObject* obj = targetPlugin->createCamera(targetInfo);
    ICamera* camera = qobject_cast<ICamera*>(obj);

    if (!camera) {
        QString error = QString("Failed to create camera instance: %1").arg(deviceId);
        Logger::instance().error(error, "Camera");
        if (m_cameraStatuses.contains(deviceId)) {
            m_cameraStatuses[deviceId].lastError = error;
        }
        emit errorOccurred(deviceId, error);
        return false;
    }

    // 连接相机
    bool connected = camera->connect();

    if (!connected) {
        QString error = QString("Failed to open/connect camera: %1").arg(deviceId);
        Logger::instance().error(error, "Camera");
        if (m_cameraStatuses.contains(deviceId)) {
            m_cameraStatuses[deviceId].lastError = error;
        }
        emit errorOccurred(deviceId, error);
        delete camera;
        return false;
    }

    m_cameras.insert(deviceId, camera);

    // 更新状态
    if (m_cameraStatuses.contains(deviceId)) {
        m_cameraStatuses[deviceId].state = CameraState::Connected;
        m_cameraStatuses[deviceId].lastError.clear();
    }

    Logger::instance().success(QString("Camera connected: %1 (%2)").arg(targetInfo.name).arg(deviceId), "Camera");
    emit cameraConnected(deviceId);
    emit cameraStateChanged(deviceId, CameraState::Connected);

    return true;
}

void CameraManager::disconnectCamera(const QString& deviceId)
{
    QMutexLocker locker(&m_mutex);

    ICamera* camera = m_cameras.value(deviceId);
    if (!camera) {
        return;
    }

    camera->disconnect();
    delete camera;
    m_cameras.remove(deviceId);

    // 更新状态
    if (m_cameraStatuses.contains(deviceId)) {
        m_cameraStatuses[deviceId].state = CameraState::Disconnected;
    }

    Logger::instance().info(QString("Camera disconnected: %1").arg(deviceId), "Camera");
    emit cameraDisconnected(deviceId);
    emit cameraStateChanged(deviceId, CameraState::Disconnected);
}

void CameraManager::disconnectAll()
{
    QMutexLocker locker(&m_mutex);

    for (ICamera* camera : m_cameras.values()) {
        if (camera) {
            camera->disconnect();
            delete camera;
        }
    }
    m_cameras.clear();

    Logger::instance().info("All cameras disconnected", "Camera");
}

ICamera* CameraManager::getCamera(const QString& deviceId) const
{
    return m_cameras.value(deviceId);
}

QList<ICamera*> CameraManager::allCameras() const
{
    return m_cameras.values();
}

bool CameraManager::isConnected(const QString& deviceId) const
{
    ICamera* camera = m_cameras.value(deviceId);
    return camera && camera->isConnected();
}

CameraState CameraManager::cameraState(const QString& deviceId) const
{
    if (m_cameras.contains(deviceId)) {
        ICamera* camera = m_cameras.value(deviceId);
        if (camera->isAcquiring()) return CameraState::Grabbing;
        if (camera->isConnected()) return CameraState::Connected;
    }

    if (m_cameraStatuses.contains(deviceId)) {
        return m_cameraStatuses.value(deviceId).state;
    }

    return CameraState::Disconnected;
}

QString CameraManager::lastError(const QString& deviceId) const
{
    if (m_cameraStatuses.contains(deviceId)) {
        return m_cameraStatuses.value(deviceId).lastError;
    }
    return QString();
}

} // namespace DeepLux
