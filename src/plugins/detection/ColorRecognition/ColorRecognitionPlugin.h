#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class ColorRecognitionPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit ColorRecognitionPlugin(QObject* parent = nullptr);
    ~ColorRecognitionPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.colorrecognition"; }
    QString name() const override { return tr("颜色识别"); }
    QString category() const override { return "detection"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("识别图像中的指定颜色区域"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    struct ColorRange {
        QString name;
        cv::Scalar lower;
        cv::Scalar upper;
    };

    bool detectColor(const cv::Mat& hsv, const ColorRange& range, cv::Mat& mask, double& area);

    QString m_targetColor = "红色";
    int m_colorCount = 0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_resultMat;
    cv::Mat m_mask;
#endif
};

} // namespace DeepLux