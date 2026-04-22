#pragma once

#include "core/interface/ICamera.h"
#include <QString>
#include <QRect>
#include <QImage>
#include <QVariantMap>
#include <QMutex>
#include <QSemaphore>
#include <QThread>

#ifdef __linux__
#include "MvCameraControl.h"
#include "ObsoleteCamParams.h"
#endif

namespace DeepLux {

/**
 * @brief Hikvision GigE/USB3 相机
 */
class HikvisionCamera : public ICamera
{
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICamera)

public:
    explicit HikvisionCamera(void* handle, const QString& deviceId, QObject* parent = nullptr);
    ~HikvisionCamera() override;

    // ICamera interface
    QString deviceId() const override { return m_deviceId; }
    QString name() const override { return m_name; }
    QString serialNumber() const override { return m_serialNumber; }
    QString manufacturer() const override { return "Hikvision"; }
    CameraCapabilities capabilities() const override { return m_capabilities; }

    bool isPlatformSupported() const override;
    QString platformNotSupportedMessage() const override;

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

    QImage lastImage() const override { QMutexLocker locker(&m_frameMutex); return m_lastImage; }
    QVariantMap lastImageMetadata() const override { return m_metadata; }
    int imageWidth() const override { return m_lastImage.width(); }
    int imageHeight() const override { return m_lastImage.height(); }

    QWidget* createConfigWidget() override;
    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;

    // Internal for plugin
    void* cameraHandle() const { return m_hCamera; }
    void setDeviceInfo(const QString& name, const QString& serial);

private:
    bool grabFrame();
public:
    bool processFrame(unsigned char* pData, MV_FRAME_OUT_INFO* pFrameInfo);

    void* m_hCamera;  // 海康相机句柄
    QString m_deviceId;
    QString m_name;
    QString m_serialNumber;
    bool m_connected = false;
    bool m_acquiring = false;
    TriggerMode m_triggerMode = TriggerMode::Continuous;

    double m_exposureTime = 10000.0;
    double m_gain = 1.0;
    double m_frameRate = 30.0;
    QRect m_roi;

    QImage m_lastImage;
    QVariantMap m_metadata;
    CameraCapabilities m_capabilities;

    mutable QMutex m_frameMutex;
    QByteArray m_frameBuffer;
};

} // namespace DeepLux
