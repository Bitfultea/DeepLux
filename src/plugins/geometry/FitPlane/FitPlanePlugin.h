#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批3：平面拟合（0143D 重建）。输入单通道高度图与可选旋转矩形 ROI
// （中心/长边/短边/角度，长边或短边 <=0 为全图），对有效像素（有限且
// != invalidValue）做最小二乘平面拟合 z = aX+bY+c（X/Y 为 pixelSize 缩放后的
// 物理坐标，z 乘 zScale）。设计矩阵去质心 + RMS 归一化后做奇异值门禁
// （共线/秩亏/严重病态失败关闭，且对平移/尺度不变，沿用批2五轮结论）。
// 输出法向量/平面距离/平面度（最大-最小偏差）/偏差极值/RMS/有效点数，
// 以及 Plane3D 契约（9 数值 = 拟合平面上 3 个非共线点）供下游组合。
class FitPlanePlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit FitPlanePlugin(QObject* parent = nullptr);
    ~FitPlanePlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.fitplane";
    }
    QString name() const override {
        return tr("平面拟合");
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
        return tr("高度图 ROI 内最小二乘拟合平面并输出平面度");
    }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override {
        return nullptr; // 统一走 metadata/PropertyPanel
    }

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;
};

} // namespace DeepLux
