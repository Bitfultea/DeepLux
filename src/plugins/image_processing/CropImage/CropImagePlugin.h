#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批4：图像裁剪（001图像处理 重建）。rectangles 数组参数驱动 1..64 个
// 旋转矩形（[cx,cy,l1,l2,deg]，l1/l2 为半长——旧版 HALCON GenRectangle2
// Length1/Length2 口径），逐矩形取旋转角的包含式轴对齐包围盒（下界 floor/
// 上界 ceil，分数边界不向内丢像素、小尺寸矩形不产生零宽）与图像求交裁剪；
// 无交集矩形记 valid=false 行（不静默丢弃），全部无交集失败关闭。输出裁剪图组 crop_images（Table：矩形回显/裁剪框/
// ImageData 子图）与 crop_count；outputFirstAsImage（旧版 IsOutputCropImage
// 对应物）为真时 image 输出端口替换为首个有效裁剪图供下游直接消费，
// 否则原样透传。
class CropImagePlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit CropImagePlugin(QObject* parent = nullptr);
    ~CropImagePlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.cropimage";
    }
    QString name() const override {
        return tr("图像裁剪");
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
        return tr("旋转矩形数组批量裁剪，输出裁剪图组");
    }

    bool initialize() override;
    void shutdown() override;
    // 阶7 批4复核（P1-2）：rectangles 数组参数 PropertyPanel 无法编辑，
    // 提供独立配置页（JSON 数组编辑，validateParams+setParams 写入）
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;
};

} // namespace DeepLux
