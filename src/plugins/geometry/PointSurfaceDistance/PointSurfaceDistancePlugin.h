#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class PointSurfaceDistancePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit PointSurfaceDistancePlugin(QObject* parent = nullptr);
    ~PointSurfaceDistancePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.pointsurfacedistance"; }
    QString name() const override { return tr("点面距离"); }
    QString category() const override { return "geometry"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("计算点到平面的距离"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    double calculatePointToPlaneDistance(double pointX, double pointY, double pointZ,
                                         double planeX1, double planeY1, double planeZ1,
                                         double planeX2, double planeY2, double planeZ2,
                                         double planeX3, double planeY3, double planeZ3);

    // 输出参数
    double m_resultDistance = 0.0;
    double m_resultFootX = 0.0;
    double m_resultFootY = 0.0;
    double m_resultFootZ = 0.0;
};

} // namespace DeepLux