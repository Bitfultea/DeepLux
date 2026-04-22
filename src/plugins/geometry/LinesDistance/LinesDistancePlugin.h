#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class LinesDistancePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit LinesDistancePlugin(QObject* parent = nullptr);
    ~LinesDistancePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.linesdistance"; }
    QString name() const override { return tr("线线距离"); }
    QString category() const override { return "geometry"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("计算两条直线之间的距离"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    double calculateLinesDistance(double line1X1, double line1Y1, double line1X2, double line1Y2,
                                 double line2X1, double line2Y1, double line2X2, double line2Y2);

    // 输出参数
    double m_resultDistance = 0.0;
};

} // namespace DeepLux