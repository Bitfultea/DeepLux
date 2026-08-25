#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class LoadPointCloudPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit LoadPointCloudPlugin(QObject* parent = nullptr);
    ~LoadPointCloudPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.loadpointcloud";
    }
    QString name() const override {
        return tr("加载点云");
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
        return tr("加载 PLY 或 TIFF 点云文件");
    }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;
};

} // namespace DeepLux
