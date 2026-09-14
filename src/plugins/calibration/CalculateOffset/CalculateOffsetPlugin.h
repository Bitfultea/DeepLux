#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批4：对位偏移计算（005坐标标定 重建）。offset = 实测(current) −
// 参考(target)，与旧版 MathCoord−ModeCoord 方向一致（旧版 OffsetX =
// -(RealRef−RealFind)），角度差归一化到 (−180,180]。当前坐标可经可选
// Point3D 端口 current_point 覆盖（取 x,y，忽略 z，与 MeasurementInput 点
// 输出直接组合；先经 portValueMatchesType 严格门禁再 parsePoint3D），当前
// 角度可经 Number 端口 current_angle 逐帧覆盖（旧版 DegLink 对应物）；类型
// 错误/非有限失败关闭。旧版 Hommat2DTrans 仿射变换与 EnableRotateCenter
// 旋转中心补正未实现（台账 partial），坐标按参数单位直接作差。
class CalculateOffsetPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit CalculateOffsetPlugin(QObject* parent = nullptr);
    ~CalculateOffsetPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.calculateoffset";
    }
    QString name() const override {
        return tr("对位偏移计算");
    }
    QString category() const override {
        return "calibration";
    }
    QString version() const override {
        return "1.0.0";
    }
    QString author() const override {
        return "DeepLux Team";
    }
    QString description() const override {
        return tr("当前坐标对基准坐标的平移+角度偏移");
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
