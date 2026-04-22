#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class FindCirclePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit FindCirclePlugin(QObject* parent = nullptr);
    ~FindCirclePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.findcircle"; }
    QString name() const override { return tr("圆检测"); }
    QString category() const override { return "detection"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("检测图像中的圆并返回圆心坐标和半径"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool findCircleHough(const cv::Mat& gray, double& centerX, double& centerY, double& radius);

    double m_minRadius = 10.0;
    double m_maxRadius = 500.0;
    double m_param1 = 50.0;  // Canny edge detection high threshold
    double m_param2 = 30.0;   // Accumulator threshold for circle centers

    // 输出参数
    double m_resultCenterX = 0.0;
    double m_resultCenterY = 0.0;
    double m_resultRadius = 0.0;
    double m_resultScore = 0.0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_gray;
#endif
};

} // namespace DeepLux
