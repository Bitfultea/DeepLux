#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class FitLinePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit FitLinePlugin(QObject* parent = nullptr);
    ~FitLinePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.fitline"; }
    QString name() const override { return tr("直线拟合"); }
    QString category() const override { return "geometry"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("对输入点集进行直线拟合"); }

    enum class FitMethod {
        RANSAC,
        LSD,
        FTH
    };

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool fitLineRANSAC(const QVector<QPointF>& points,
                       double& rx, double& ry,
                       double& px, double& py);

    FitMethod m_fitMethod = FitMethod::RANSAC;
    double m_threshold = 3.0;
    double m_iterations = 100;

    // 输出参数
    double m_resultRow1 = 0.0;
    double m_resultCol1 = 0.0;
    double m_resultRow2 = 0.0;
    double m_resultCol2 = 0.0;
    double m_resultPhi = 0.0;
    double m_resultRho = 0.0;
    double m_resultError = 0.0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_contour;
#endif
};

} // namespace DeepLux
