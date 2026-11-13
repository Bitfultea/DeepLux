#pragma once

#include "V4L2DiscoverySystem.h"
#include "core/interface/ICameraDiscoveryAsync.h"
#include "core/interface/ICameraPlugin.h"

#include <QHash>
#include <QList>
#include <QString>
#include <atomic>
#include <memory>

namespace DeepLux {

/**
 * @brief V4L2 相机插件 - 使用 Video4Linux2 枚举和访问相机
 */
class DEEPLUX_PLUGIN_API V4L2CameraPlugin : public ICameraPlugin, public ICameraDiscoveryAsync {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DEEPLUX_CAMERA_PLUGIN_INTERFACE_IID FILE "metadata.json")
    Q_INTERFACES(DeepLux::ICameraPlugin DeepLux::ICameraDiscoveryAsync)

public:
    explicit V4L2CameraPlugin(QObject* parent = nullptr);
    V4L2CameraPlugin(std::shared_ptr<IV4L2DiscoverySystem> discoverySystem, QObject* parent = nullptr);
    ~V4L2CameraPlugin() override;

    // ICameraPlugin interface
    QString pluginId() const override {
        return "v4l2";
    }
    QString pluginName() const override {
        return "Video4Linux2";
    }
    QString manufacturer() const override {
        return "Linux";
    }

    bool isAvailable() const override;
    QString availabilityMessage() const override;

    QList<CameraInfo> discoverCameras() override;
    QObject* createCamera(const CameraInfo& info) override;

    CameraDiscoveryJob* startDiscovery(quint64 requestId, QString& errorCode) override;
    void requestCancel(quint64 requestId) override;

private:
    QString getDeviceName(const QString& devicePath) const;
    bool isV4L2Device(const QString& devicePath) const;
    void releaseDiscoveryRequest(quint64 requestId);

    std::shared_ptr<IV4L2DiscoverySystem> m_discoverySystem;
    QHash<quint64, std::shared_ptr<std::atomic_bool>> m_discoveryRequests;
};

} // namespace DeepLux
