#pragma once

#include <QObject>
#include <QWidget>

namespace DeepLux {

/**
 * @brief 运动控制能力
 */
struct MotionCapabilities {
    bool supportedOnWindows = true;
    bool supportedOnLinux = false;
    int maxAxes = 4;
    bool interpolation = true;
    bool gCode = false;
};

/**
 * @brief 运动控制接口
 */
class IMotion : public QObject
{
    Q_OBJECT

public:
    virtual ~IMotion() = default;

    // ========== 设备信息 ==========
    virtual QString name() const = 0;
    virtual QString manufacturer() const = 0;
    virtual MotionCapabilities capabilities() const = 0;

    // ========== 平台支持 ==========
    virtual bool isPlatformSupported() const = 0;

    // ========== 初始化 ==========
    virtual bool initialize(int cardIndex = 0) = 0;
    virtual void shutdown() = 0;
    virtual bool isInitialized() const = 0;

    // ========== 轴控制 ==========
    virtual int axisCount() const = 0;
    virtual bool enableAxis(int axis, bool enable) = 0;
    virtual bool isAxisEnabled(int axis) const = 0;

    // ========== 回零 ==========
    virtual bool home(int axis) = 0;
    virtual bool isHoming(int axis) const = 0;
    virtual bool isHomed(int axis) const = 0;

    // ========== 运动 ==========
    virtual bool moveAbsolute(int axis, double position, double velocity) = 0;
    virtual bool moveRelative(int axis, double distance, double velocity) = 0;
    virtual bool moveStop(int axis) = 0;
    virtual bool emergencyStop() = 0;

    // ========== 状态 ==========
    virtual double position(int axis) const = 0;
    virtual double velocity(int axis) const = 0;
    virtual bool isMoving(int axis) const = 0;
    virtual bool isLimitPositive(int axis) const = 0;
    virtual bool isLimitNegative(int axis) const = 0;

    // ========== 配置 ==========
    virtual QWidget* createConfigWidget() = 0;
    virtual QJsonObject toJson() const = 0;
    virtual bool fromJson(const QJsonObject& json) = 0;

signals:
    void motionComplete(int axis);
    void homeComplete(int axis);
    void limitHit(int axis, bool positive);
    void errorOccurred(const QString& error);
};

} // namespace DeepLux

Q_DECLARE_INTERFACE(DeepLux::IMotion, "com.deeplux.IMotion/2.0")
