#pragma once

#include "core/interface/ICameraPlugin.h"
#include <QList>
#include <QString>

namespace DeepLux {

/**
 * @brief V4L2 相机插件 - 使用 Video4Linux2 枚举和访问相机
 */
class V4L2CameraPlugin : public ICameraPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.ICameraPlugin" FILE "metadata.json")
    Q_INTERFACES(DeepLux::ICameraPlugin)

public:
    explicit V4L2CameraPlugin(QObject* parent = nullptr);
    ~V4L2CameraPlugin() override;

    // ICameraPlugin interface
    QString pluginId() const override { return "v4l2"; }
    QString pluginName() const override { return "Video4Linux2"; }
    QString manufacturer() const override { return "Linux"; }

    bool isAvailable() const override;
    QString availabilityMessage() const override;

    QList<CameraInfo> discoverCameras() override;
    QObject* createCamera(const CameraInfo& info) override;

private:
    QString getDeviceName(const QString& devicePath) const;
    bool isV4L2Device(const QString& devicePath) const;
};

} // namespace DeepLux
