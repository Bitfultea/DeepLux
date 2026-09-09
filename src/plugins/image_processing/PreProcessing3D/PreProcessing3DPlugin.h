#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批3：3D 预处理（0143D 重建，metadata 名沿用旧版 3DPreProcessing）。
// 输入数值深度/高度图：单通道 8/16/32/64 位；2 通道按旧版 Decompose2 语义
// 提取第 2 通道（深度）；3/4 通道明确失败。自动 NoData 检测复用 TiffLoader
// 重复极值判据（单通道 32/64 位浮点）。执行可选 ROI（宽高必须同为 0 或同 >0）
// 与高度筛选：非有限值、NoData 哨兵、[heightFilterMin,heightFilterMax] 区间外
// 与 ROI 外像素统一填充 fillValue；输出 CV_32F 高度图、筛选统计，且实际填充过
// 像素时写出统一无效值契约键 height_invalid_value 供下游优先采用。
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
