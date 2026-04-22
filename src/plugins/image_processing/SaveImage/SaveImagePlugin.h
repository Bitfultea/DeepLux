#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class SaveImagePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit SaveImagePlugin(QObject* parent = nullptr);
    ~SaveImagePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.saveimage"; }
    QString name() const override { return tr("保存图像"); }
    QString category() const override { return "image_processing"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("保存图像到文件"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool saveImage(const QString& filePath, const QString& format, int quality);

    QString m_filePath;
    QString m_format;
    int m_quality;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_mat;
#endif
};

} // namespace DeepLux
