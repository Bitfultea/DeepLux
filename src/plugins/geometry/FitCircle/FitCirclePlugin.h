#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class FitCirclePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit FitCirclePlugin(QObject* parent = nullptr);
    ~FitCirclePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.fitcircle"; }
    QString name() const override { return tr("圆拟合"); }
    QString category() const override { return "geometry"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("对输入点集进行圆拟合"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool fitCircleRANSAC(const QVector<QPointF>& points,
                         double& centerX, double& centerY, double& radius);

    double m_threshold = 2.0;
    double m_iterations = 100;
    double m_minRadius = 1.0;
    double m_maxRadius = 1000.0;

    // 输出参数
    double m_resultCenterX = 0.0;
    double m_resultCenterY = 0.0;
    double m_resultRadius = 0.0;
    double m_resultError = 0.0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_pointsMat;
#endif
};

} // namespace DeepLux