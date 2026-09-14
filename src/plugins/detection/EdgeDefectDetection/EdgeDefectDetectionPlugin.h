#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批2：边缘缺陷检测。对参考边缘拟合基准圆（退化点集——重复/共线/严重病态
// ——失败关闭），参考点按 atan2 角序排序并按角度去重后逐射线卡钳实测边缘，
// 偏差 = 实测半径 - 基准圆半径；最低成功数 + 角度覆盖率门禁（大角空洞不计入
// 覆盖），失败射线与明显角度空洞均为区域分隔，|偏差|>阈值 的同类连续射线环形
// 合并为缺陷区域（极性翻转必切分）。输出选定极性区域数（defect_count/has_defect
// 恒一致）、凸/凹区域数、结构化区域表（defect_regions,Table：极性/原始索引/
// 角度/wraps_zero/射线数/偏差）与逐采样偏差统计。
class EdgeDefectDetectionPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit EdgeDefectDetectionPlugin(QObject* parent = nullptr);
    ~EdgeDefectDetectionPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.edgedefectdetection";
    }
    QString name() const override {
        return tr("边缘缺陷检测");
    }
    QString category() const override {
        return "detection";
    }
    QString version() const override {
        return "1.0.0";
    }
    QString author() const override {
        return "DeepLux Team";
    }
    QString description() const override {
        return tr("相对参考边缘检测凸出/凹陷缺陷");
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
