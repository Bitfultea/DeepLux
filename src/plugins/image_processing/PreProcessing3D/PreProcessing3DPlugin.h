#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批3：3D 预处理（0143D 重建，metadata 名沿用旧版 3DPreProcessing）。
// 输入数值深度/高度图：单通道 8/16/32/64 位；2 通道按旧版 Decompose2 语义
// 提取第 2 通道（深度）；3/4 通道明确失败。先消费上游无效值契约
// （height_invalid_value，按源精度量化；键存在但类型错误或非有限均失败
// 关闭），旧无效像素统一转为本次 fillValue；自动 NoData 检测复用 TiffLoader
// 重复极值判据（原始像素单位启发式，单通道 32/64 位浮点，autoNoData 可关，
// 携带契约时让位，双侧极值同满足判据的歧义图失败关闭不按分布猜测）。ROI 坐标按输入像素网格解释，
// 任一 ROI 维度为 0 表示该轴全幅，支持参数逐项编辑；高度筛选按输入单位计算。
// 下采样按步长保留样本，scaleZ 缩放输出高度/统计，scaleX/scaleY 缩放输出点云坐标。
// 非有限值、契约值、NoData 哨兵、区间外与 ROI 外像素统一填充 fillValue；填充值与存活合法高度碰撞时失败关闭；
// 输出 CV_32F 高度图、筛选统计，并写出供下游使用的 height_invalid_value 契约键。
class DEEPLUX_PLUGIN_API PreProcessing3DPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DEEPLUX_MODULE_INTERFACE_IID FILE "metadata.json")
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
