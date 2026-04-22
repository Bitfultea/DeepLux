#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class DistancePPPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit DistancePPPlugin(QObject* parent = nullptr);
    ~DistancePPPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.distancepp"; }
    QString name() const override { return tr("点点距离"); }
    QString category() const override { return "geometry"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("计算两点之间的距离"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    double calculateDistance(double x1, double y1, double x2, double y2);

    // 输出参数
    double m_resultDistance = 0.0;
    double m_resultDeltaX = 0.0;
    double m_resultDeltaY = 0.0;
};

} // namespace DeepLux