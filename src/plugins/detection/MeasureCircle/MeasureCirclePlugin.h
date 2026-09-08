#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

// 阶7 批2：圆测量。在初始圆邻域内以径向卡钳提取边缘点（梯度峰值>=阈值），
// 代数最小二乘拟合圆，按剔除半径剔除离群边缘点后重拟合，输出圆心/半径/直径/圆度。
class MeasureCirclePlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit MeasureCirclePlugin(QObject* parent = nullptr);
    ~MeasureCirclePlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.measurecircle";
    }
    QString name() const override {
        return tr("圆测量");
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
        return tr("径向卡钳提取边缘点并拟合圆");
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
