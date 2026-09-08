#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批3：3D 间隙测量（0143D 重建）。高度图轴对齐矩形 ROI 内按行均值提取
// 单截面轮廓（跳过无效值），中值+高斯平滑后中心差分求斜率：最陡下降沿为
// 起点拐角、其右侧满足最小峰距的最陡上升沿为终点拐角（阈值门限+抛物线
// 亚像素细化），拐角间距 × pixelSizeX 为间隙宽度。未找到合格拐角时按契约
// 输出 measureFailValue 且 gap_found=false（真实反映"未检出"，非伪成功）；
// is_pass = 检出且宽度 <= specUpperLimit。
class GapMeasure3DPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit GapMeasure3DPlugin(QObject* parent = nullptr);
    ~GapMeasure3DPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.gapmeasure3d";
    }
    QString name() const override {
        return tr("3D间隙测量");
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
        return tr("高度图截面导数寻峰测量间隙宽度");
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
