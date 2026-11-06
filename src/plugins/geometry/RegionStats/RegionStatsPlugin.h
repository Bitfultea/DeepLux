#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

/**
 * @brief Region2D 消费者插件（SAM 批1）
 *
 * 消费 Region2D 载荷（强类型或 polygon+bbox map 形态），输出面积（多边形
 * shoelace，polygon 不足 3 点时回退 bbox 面积）、外接矩形、质心、点数、
 * 置信度与标签，验证 Region2D 契约在主流程中可被下游消费。
 */
class DEEPLUX_PLUGIN_API RegionStatsPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit RegionStatsPlugin(QObject* parent = nullptr);
    ~RegionStatsPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.regionstats";
    }
    QString name() const override {
        return tr("区域统计");
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
        return tr("Region2D 区域面积/外接矩形/质心统计");
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
