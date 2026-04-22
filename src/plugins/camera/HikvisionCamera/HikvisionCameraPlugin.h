#pragma once

#include "core/interface/ICameraPlugin.h"

namespace DeepLux {

class HikvisionCamera;

/**
 * @brief Hikvision 相机插件
 */
class HikvisionCameraPlugin : public ICameraPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.ICameraPlugin" FILE "metadata.json")
    Q_INTERFACES(DeepLux::ICameraPlugin)

public:
    HikvisionCameraPlugin(QObject* parent = nullptr);
    ~HikvisionCameraPlugin() override;

    // ICameraPlugin interface
    QString pluginId() const override { return "hikvision"; }
    QString pluginName() const override { return "Hikvision Camera"; }
    QString manufacturer() const override { return "Hikvision"; }

    bool isAvailable() const override;
    QString availabilityMessage() const override;

    QList<CameraInfo> discoverCameras() override;
    QObject* createCamera(const CameraInfo& info) override;

private:
    bool initSDK();
    void cleanupSDK();

    bool m_sdkInitialized;
};

} // namespace DeepLux
