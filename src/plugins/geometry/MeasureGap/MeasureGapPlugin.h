#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class MeasureGapPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit MeasureGapPlugin(QObject* parent = nullptr);
    ~MeasureGapPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.measuregap"; }
    QString name() const override { return tr("间隙测量"); }
    QString category() const override { return "geometry"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("测量两点之间的间隙距离"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    double calculateGapDistance(double x1, double y1, double z1,
                                double x2, double y2, double z2);

    // 输出参数
    double m_resultGap = 0.0;
    double m_resultDeltaX = 0.0;
    double m_resultDeltaY = 0.0;
    double m_resultDeltaZ = 0.0;
};

} // namespace DeepLux