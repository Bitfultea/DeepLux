#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

// 阶7 批1：椭圆拟合。对输入点集做直接最小二乘圆锥拟合（Fitzgibbon），
// 按离群阈值迭代剔除后重拟合，输出中心/角度/长短半轴/椭圆度/误差。
class FitEllipsePlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    struct EllipseResult {
        double centerX = 0.0;
        double centerY = 0.0;
        double phi = 0.0;    // 度
        double majorR = 0.0; // 长半轴
        double minorR = 0.0; // 短半轴
        double error = 0.0;
    };

    explicit FitEllipsePlugin(QObject* parent = nullptr);
    ~FitEllipsePlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.fitellipse";
    }
    QString name() const override {
        return tr("椭圆拟合");
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
        return tr("对输入边缘点集进行椭圆拟合");
    }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool fitEllipseLeastSquares(const QVector<QPointF>& points, EllipseResult& result) const;

    EllipseResult m_result;
};

} // namespace DeepLux
