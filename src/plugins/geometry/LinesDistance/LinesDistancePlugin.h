#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class LinesDistancePlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit LinesDistancePlugin(QObject* parent = nullptr);
    ~LinesDistancePlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.linesdistance";
    }
    QString name() const override {
        return tr("线线距离");
    }
    QString category() const override {
        return "geometry";
    }
    QString version() const override {
        return "1.0.0";
    }
    QString author() const override {
        return "DeepLux Team";
    }
    QString description() const override {
        return tr("计算两条线段之间的最短距离");
    }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    // P0-1: 有限线段最短距离（非无限直线）。输出最近点对。
    double calculateSegmentDistance(double ax1, double ay1, double ax2, double ay2, double bx1, double by1, double bx2,
                                    double by2, double& nearAx, double& nearAy, double& nearBx, double& nearBy);
    // 点到线段最短距离，返回最近点
    static double pointSegmentDistance(double px, double py, double ax, double ay, double bx, double by,
                                       double& nearestX, double& nearestY);
    // 两线段是否相交
    static bool segmentsIntersect(double ax1, double ay1, double ax2, double ay2, double bx1, double by1, double bx2,
                                  double by2);

    // 输出参数
    double m_resultDistance = 0.0;
    double m_nearAx = 0.0;
    double m_nearAy = 0.0;
    double m_nearBx = 0.0;
    double m_nearBy = 0.0;
};

} // namespace DeepLux