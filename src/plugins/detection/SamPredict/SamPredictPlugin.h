#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

/**
 * @brief SAM 分割生产者插件（SAM 批1）
 *
 * 输入 Image2D 与 prompt 参数（正点/负点/框），输出强类型 Region2D
 * （polygon/bbox/maskRle/score/label）。同步预测核心（栈上客户端 + 本地
 * 事件循环 + 取消轮询 + Region2D 契约门禁）自 SAM 批3 起收口到共享的
 * SamPredictor（与 Agent sam_predict/sam_annotate 工具同一实现）；超时、
 * 后端错误或引擎取消（stop）失败关闭；mask 像素不入载荷（仅 RLE）。
 */
class DEEPLUX_PLUGIN_API SamPredictPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit SamPredictPlugin(QObject* parent = nullptr);
    ~SamPredictPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.sampredict";
    }
    QString name() const override {
        return tr("SAM 分割");
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
        return tr("SAM 点/框 prompt 分割，输出 Region2D 区域");
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
