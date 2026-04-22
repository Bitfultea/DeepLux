#pragma once

#include "core/interface/ICamera.h"
#include <QString>
#include <QRect>
#include <QImage>
#include <QVariantMap>

#ifdef __linux__
#include <linux/videodev2.h>
#endif

namespace DeepLux {

/**
 * @brief V4L2 相机设备
 */
class V4L2Camera : public ICamera
{
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICamera)

public:
    explicit V4L2Camera(const QString& devicePath, QObject* parent = nullptr);
    ~V4L2Camera() override;

    // ICamera interface
    QString deviceId() const override { return m_devicePath; }
    QString name() const override { return m_name; }
    QString serialNumber() const override { return m_serialNumber; }
    QString manufacturer() const override { return "Linux"; }
    CameraCapabilities capabilities() const override { return m_capabilities; }

    bool isPlatformSupported() const override { return true; }
    QString platformNotSupportedMessage() const override { return QString(); }

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override { return m_connected; }

    bool startAcquisition() override;
    void stopAcquisition() override;
    bool isAcquiring() const override { return m_acquiring; }
    bool triggerSoftware() override;
    void setTriggerMode(TriggerMode mode) override;
    TriggerMode triggerMode() const override { return m_triggerMode; }

    void setExposureTime(double microseconds) override;
    double exposureTime() const override { return m_exposureTime; }
    void setGain(double gain) override;
    double gain() const override { return m_gain; }
    void setFrameRate(double fps) override;
    double frameRate() const override { return m_frameRate; }
    void setRoi(int x, int y, int width, int height) override;
    QRect roi() const override { return m_roi; }

    QImage lastImage() const override { return m_lastImage; }
    QVariantMap lastImageMetadata() const override { return m_metadata; }
    int imageWidth() const override { return m_lastImage.width(); }
    int imageHeight() const override { return m_lastImage.height(); }

    QWidget* createConfigWidget() override;
    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;

private:
    bool openDevice();
    void closeDevice();
    bool grabFrame();
    bool configureFormat(int width, int height);

#ifdef __linux__
    bool initBuffers();
    void uninitBuffers();
    QImage convertV4L2Frame(const void* data, size_t bytesused, int width, int height, uint32_t pixelformat);
    static QImage yuyvToRgb(const uchar* yuyv, int width, int height);
#endif

    QString m_devicePath;
    QString m_name;
    QString m_serialNumber;
    bool m_connected = false;
    bool m_acquiring = false;
    TriggerMode m_triggerMode = TriggerMode::Continuous;

    double m_exposureTime = 10000.0;  // microseconds
    double m_gain = 1.0;
    double m_frameRate = 30.0;
    QRect m_roi;

    QImage m_lastImage;
    QVariantMap m_metadata;
    CameraCapabilities m_capabilities;

#ifdef __linux__
    int m_fd = -1;
    uint32_t m_pixelFormat = 0;

    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
    };
    Buffer m_buffers[2];
    int m_bufferCount = 0;
#endif
};

} // namespace DeepLux
