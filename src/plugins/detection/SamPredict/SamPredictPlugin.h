#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

/**
 * @brief SAM 分割生产者插件（SAM 批1）
 *
 * 输入 Image2D 与 prompt 参数（正点/负点/框），调用 SamBackendClient 完成
 * set_image + predict，输出强类型 Region2D（polygon/bbox/maskRle/score/label）。
 * 客户端按调用线程栈上创建并以本地事件循环同步收口（NAM 线程亲和），超时
 * 或后端错误失败关闭；mask 像素不入载荷（仅 RLE）。
 */
class SamPredictPlugin : public ModuleBase {
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
