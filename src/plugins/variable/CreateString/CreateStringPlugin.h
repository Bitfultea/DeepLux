#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class CreateStringPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit CreateStringPlugin(QObject* parent = nullptr);
    ~CreateStringPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.createstring"; }
    QString name() const override { return tr("创建字符串"); }
    QString category() const override { return "variable"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("创建并初始化字符串变量"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    enum class StringSource {
        Fixed,
        Input
    };

    StringSource m_stringSource = StringSource::Fixed;
    QString m_fixedString;
    QString m_outputVarName;
};

} // namespace DeepLux
