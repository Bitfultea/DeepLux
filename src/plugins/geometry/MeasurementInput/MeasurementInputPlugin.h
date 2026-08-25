#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class MeasurementInputPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit MeasurementInputPlugin(QObject* parent = nullptr);
    ~MeasurementInputPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.measurementinput";
    }
    QString name() const override {
        return tr("测量输入");
    }
    QString category() const override {
        return "geometry";
    }
    QString version() const override {
        return "1.0.0";
    }
    QString author() const override {
        return "DeepLux";
    }
    QString description() const override {
        return tr("测量输入适配器");
    }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    static QVariantList jsonArrayToVariantList(const QJsonArray& arr);
};

} // namespace DeepLux
