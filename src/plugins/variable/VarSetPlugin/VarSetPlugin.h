#pragma once

#include "core/base/ModuleBase.h"
#include "core/common/VarModel.h"

namespace DeepLux {

class VarSetPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit VarSetPlugin(QObject* parent = nullptr);
    ~VarSetPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.varset"; }
    QString name() const override { return tr("变量赋值"); }
    QString category() const override { return "variable"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("设置已存在变量的值，支持表达式或引用其他变量"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    QVariant evaluateExpression(const QString& expr, VarDataType targetType) const;
    QVariant resolveValue(const QString& val, VarDataType targetType) const;
    QVariant convertValue(const QVariant& val, VarDataType targetType) const;

    QString m_variableName;
    QString m_value;
};

} // namespace DeepLux
