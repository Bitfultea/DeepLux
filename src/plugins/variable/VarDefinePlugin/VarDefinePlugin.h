#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class VarDefinePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit VarDefinePlugin(QObject* parent = nullptr);
    ~VarDefinePlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.vardefine"; }
    QString name() const override { return tr("变量定义"); }
    QString category() const override { return "variable"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("在GlobalVarManager中创建新变量"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    QString m_variableName;
    QString m_variableType;
    QVariant m_initialValue;
};

} // namespace DeepLux
