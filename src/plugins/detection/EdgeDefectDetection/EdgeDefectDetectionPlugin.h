#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批2：边缘缺陷检测。对参考边缘拟合基准圆，沿参考点方向卡钳实测边缘，
// 计算径向偏差（实测半径-参考半径），按阈值统计凸出/凹陷缺陷与偏差统计。
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
