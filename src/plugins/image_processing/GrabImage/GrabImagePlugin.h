#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

#include <QTimer>

namespace DeepLux {

class GrabImagePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit GrabImagePlugin(QObject* parent = nullptr);
    ~GrabImagePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.grabimage"; }
    QString name() const override { return tr("图像采集"); }
    QString category() const override { return "image_processing"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("从相机或文件采集图像"); }

    bool initialize() override;
    void shutdown() override;

    void setParam(const QString& key, const QVariant& value) override;
    QWidget* createConfigWidget() override;

    enum class GrabSource {
        Camera,
        File,
        Demo
    };

    QString cameraId() const { return m_cameraId; }
    void setCameraId(const QString& id) { m_cameraId = id; }

    QString filePath() const { return m_filePath; }
    void setFilePath(const QString& path) { m_filePath = path; }

signals:
    void cameraIdChanged();
    void filePathChanged();

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    bool grabFromCamera(ImageData& output);
    bool grabFromFile(ImageData& output);
    bool grabDemo(ImageData& output);
    void onCameraGrabTimeout();

    QString m_cameraId;
    QString m_filePath;

    // Camera grab timeout support
    QTimer* m_grabTimer;
    bool m_grabTimedOut;
    bool m_grabSuccess;

#ifdef DEEPLUX_HAS_OPENCV
    cv::VideoCapture m_capture;
    cv::Mat m_frame;
#endif

protected:
    IModule* cloneImpl() const override;
};

} // namespace DeepLux
