#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批4：对位偏移计算（005坐标标定 重建）。当前坐标（X/Y/Φ）对基准坐标
// 求平移+角度偏移：offset = target − current，角度差归一化到 (−180,180]。
// 当前坐标可经可选 Point3D 端口 current_point 覆盖（取 x,y，忽略 z，与
// MeasurementInput 点输出直接组合；端口存在时先经 portValueMatchesType
// 严格门禁再 parsePoint3D），否则用 currentX/currentY 参数。
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
