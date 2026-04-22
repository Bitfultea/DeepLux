#pragma once

#include "core/interface/ICamera.h"
#include <QString>
#include <QRect>
#include <QImage>
#include <QVariantMap>

#ifdef _WIN32
#include <dshow.h>
#include <windows.h>
#endif

namespace DeepLux {

/**
 * @brief DirectShow 相机设备 (Windows)
 */
class DirectShowCamera : public ICamera
{
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICamera)

public:
    explicit DirectShowCamera(const QString& devicePath, QObject* parent = nullptr);
    ~DirectShowCamera() override;

    // ICamera interface
    QString deviceId() const override { return m_devicePath; }
    QString name() const override { return m_name; }
    QString serialNumber() const override { return m_serialNumber; }
    QString manufacturer() const override { return "Windows"; }
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

    QString m_devicePath;
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

#ifdef _WIN32
    IGraphBuilder* m_pGraph = nullptr;
    ICaptureGraphBuilder2* m_pCapture = nullptr;
    IMediaControl* m_pControl = nullptr;
    IBaseFilter* m_pDeviceFilter = nullptr;
#endif
};

} // namespace DeepLux
