#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QVariantMap>

namespace DeepLux {

/**
 * @brief 相机信息 - 用于相机发现和连接
 */
struct CameraInfo {
    QString pluginId;           // 来自哪个插件
    QString deviceId;           // 厂商定义的设备ID (V4L2下是 /dev/videoX)
    QString name;              // 显示名称
    QString serialNumber;       // 序列号 (如果可用)
    QString ipAddress;         // GigE相机IP (如果适用)
    int width = 0;             // 默认宽度
    int height = 0;            // 默认高度
    QString pixelFormat;       // 像素格式 "Mono8", "RGB8" 等
    QVariantMap rawInfo;        // 厂商原始信息
    bool isConnected = false;   // 是否已连接
};

/**
 * @brief 相机插件接口 - 所有相机驱动必须实现
 *
 * 用于运行时发现和创建相机实例
 */
class ICameraPlugin : public QObject
{
    Q_OBJECT
public:
    ICameraPlugin(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~ICameraPlugin() = default;

    // ========== 插件信息 ==========

    virtual QString pluginId() const = 0;      // "v4l2", "basler", "hikvision"
    virtual QString pluginName() const = 0;    // "Video4Linux2", "Basler Pylon"
    virtual QString manufacturer() const = 0;  // "Linux", "Basler AG"

    // ========== 平台支持 ==========

    virtual bool isAvailable() const = 0;     // SDK是否安装/驱动是否可用
    virtual QString availabilityMessage() const = 0;  // 不可用原因

    // ========== 相机发现 ==========

    virtual QList<CameraInfo> discoverCameras() = 0;

    // ========== 相机创建 ==========

    virtual QObject* createCamera(const CameraInfo& info) = 0;

signals:
    void cameraDiscovered(const CameraInfo& camera);
    void availabilityChanged(bool available);
};

} // namespace DeepLux

Q_DECLARE_INTERFACE(DeepLux::ICameraPlugin, "com.deeplux.ICameraPlugin/1.0")
