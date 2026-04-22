#pragma once

#include <QObject>
#include <QImage>
#include <QVariantMap>
#include <QRect>
#include <QWidget>

namespace DeepLux {

/**
 * @brief 触发模式
 */
enum class TriggerMode {
    Software,       // 软触发
    Hardware,       // 硬触发
    Continuous      // 连续采集
};

/**
 * @brief 相机能力
 */
struct CameraCapabilities {
    bool softwareTrigger = true;
    bool hardwareTrigger = true;
    bool continuousMode = true;
    bool exposureControl = true;
    bool gainControl = true;
    bool frameRateControl = true;
    bool roiControl = true;
    bool pixelFormatControl = true;
    bool whiteBalance = false;
    bool depthData = false;
    
    bool supportedOnWindows = true;
    bool supportedOnLinux = true;
};

/**
 * @brief 相机接口 - 所有相机驱动必须实现
 */
class ICamera : public QObject
{
    Q_OBJECT

public:
    ICamera(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~ICamera() = default;

    // ========== 设备信息 ==========
    
    virtual QString deviceId() const = 0;
    virtual QString name() const = 0;
    virtual QString serialNumber() const = 0;
    virtual QString manufacturer() const = 0;
    virtual CameraCapabilities capabilities() const = 0;

    // ========== 平台支持 ==========
    
    virtual bool isPlatformSupported() const = 0;
    virtual QString platformNotSupportedMessage() const { return QString(); }

    // ========== 连接管理 ==========
    
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // ========== 采集控制 ==========
    
    virtual bool startAcquisition() = 0;
    virtual void stopAcquisition() = 0;
    virtual bool isAcquiring() const = 0;
    virtual bool triggerSoftware() = 0;
    virtual void setTriggerMode(TriggerMode mode) = 0;
    virtual TriggerMode triggerMode() const = 0;

    // ========== 参数设置 ==========
    
    virtual void setExposureTime(double microseconds) = 0;
    virtual double exposureTime() const = 0;
    virtual void setGain(double gain) = 0;
    virtual double gain() const = 0;
    virtual void setFrameRate(double fps) = 0;
    virtual double frameRate() const = 0;
    virtual void setRoi(int x, int y, int width, int height) = 0;
    virtual QRect roi() const = 0;

    // ========== 图像获取 ==========
    
    virtual QImage lastImage() const = 0;
    virtual QVariantMap lastImageMetadata() const = 0;
    virtual int imageWidth() const = 0;
    virtual int imageHeight() const = 0;

    // ========== 配置 ==========
    
    virtual QWidget* createConfigWidget() = 0;
    virtual QJsonObject toJson() const = 0;
    virtual bool fromJson(const QJsonObject& json) = 0;

signals:
    void imageReceived(const QImage& image, const QVariantMap& metadata);
    void connectedChanged(bool connected);
    void acquiringChanged(bool acquiring);
    void errorOccurred(const QString& error);
};

} // namespace DeepLux

Q_DECLARE_INTERFACE(DeepLux::ICamera, "com.deeplux.ICamera/2.0")
