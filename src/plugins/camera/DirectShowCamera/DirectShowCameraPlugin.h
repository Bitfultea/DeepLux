#pragma once

#include "core/interface/ICameraPlugin.h"

namespace DeepLux {

/**
 * @brief DirectShow 相机插件 (Windows)
 */
class DirectShowCameraPlugin : public ICameraPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.ICameraPlugin" FILE "metadata.json")
    Q_INTERFACES(DeepLux::ICameraPlugin)

public:
    DirectShowCameraPlugin(QObject* parent = nullptr);
    ~DirectShowCameraPlugin() override;

    // ICameraPlugin interface
    QString pluginId() const override { return "directshow"; }
    QString pluginName() const override { return "DirectShow Camera"; }
    QString manufacturer() const override { return "Microsoft"; }

    bool isAvailable() const override;
    QString availabilityMessage() const override;

    QList<CameraInfo> discoverCameras() override;
    QObject* createCamera(const CameraInfo& info) override;

private:
    QString getDeviceName(int index) const;
    bool isDirectShowDevice(int index) const;
};

} // namespace DeepLux
