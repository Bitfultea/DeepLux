#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class FreeformSurfacePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit FreeformSurfacePlugin(QObject* parent = nullptr);
    ~FreeformSurfacePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.freeformsurface"; }
    QString name() const override { return tr("自由曲面"); }
    QString category() const override { return "geometry"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("处理和分析自由曲面点云数据"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool fitPlaneToPoints(const std::vector<cv::Point3f>& points, cv::Vec4f& planeCoeffs);

    double m_samplingInterval = 1.0;
    int m_pointCount = 0;
    double m_surfaceArea = 0.0;
    double m_surfaceRoughness = 0.0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_pointCloud;
#endif
};

} // namespace DeepLux