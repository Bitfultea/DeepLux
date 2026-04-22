#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class DistancePLPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit DistancePLPlugin(QObject* parent = nullptr);
    ~DistancePLPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.distancepl"; }
    QString name() const override { return tr("点线距离"); }
    QString category() const override { return "geometry"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("计算点到直线的距离"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    double calculateDistancePointToLine(double pointX, double pointY,
                                        double lineX1, double lineY1,
                                        double lineX2, double lineY2);

    // 输出参数
    double m_resultDistance = 0.0;
    double m_resultFootX = 0.0;
    double m_resultFootY = 0.0;
};

} // namespace DeepLux