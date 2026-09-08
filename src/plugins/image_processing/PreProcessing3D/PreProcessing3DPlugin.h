#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批3：3D 预处理（0143D 重建，metadata 名沿用旧版 3DPreProcessing）。
// 输入数值深度/高度图：单通道 8/16/32/64 位；2 通道按旧版 Decompose2 语义
// 提取第 2 通道（深度）；3/4 通道明确失败。执行可选 ROI 提取与高度筛选：
// 非有限值、[heightFilterMin,heightFilterMax] 区间外与 ROI 外像素统一填充
// fillValue；输出 CV_32F 高度图与筛选统计（valid/filtered 像素数、存活值域）。
class PreProcessing3DPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit PreProcessing3DPlugin(QObject* parent = nullptr);
    ~PreProcessing3DPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.3dpreprocessing";
    }
    QString name() const override {
        return tr("3D预处理");
    }
    QString category() const override {
        return "image_processing";
    }
    QString version() const override {
        return "1.0.0";
    }
    QString author() const override {
        return "DeepLux Team";
    }
    QString description() const override {
        return tr("深度图通道提取/ROI/高度筛选预处理");
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
