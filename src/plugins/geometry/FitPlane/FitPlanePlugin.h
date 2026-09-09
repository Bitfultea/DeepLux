#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批3：平面拟合（0143D 重建）。输入单通道高度图与可选旋转矩形 ROI
// （中心/长边/短边/角度，两边必须同为 0=全图或同 >0），对有效像素做最小二乘
// 平面拟合 z = aX+bY+c（X/Y 为 pixelSize 缩放物理坐标，z 乘 zScale）。
// 无效值契约：输入携带的 height_invalid_value（3DPreProcessing 写出）优先于
// 自身 invalidValue 参数，哨兵按源图存储精度量化（CV_32F 非整数哨兵可精确
// 匹配）；TiffLoader 重复极值 NoData 自动检测可经 autoNoData 关闭，且输入
// 已携带契约时让位（不重复判定、不误删合法平台、省去整幅扫描）。
// 拟合为多遍扫描累计正规方程（O(1) 内存），去质心+RMS 归一化的特征值门禁
// 拒绝共线/秩亏/严重病态（对平移/尺度不变，与批2五轮 SVD 门禁同一判据），
// 每行响应取消令牌。输出法向量/平面距离/平面度（最大-最小偏差）/偏差极值/
// RMS/有效点数，以及 Plane3D 契约（9 数值 = 平面上 3 个非共线点）供下游组合。
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
