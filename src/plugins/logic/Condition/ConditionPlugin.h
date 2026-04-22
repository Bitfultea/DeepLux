#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class ConditionPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit ConditionPlugin(QObject* parent = nullptr);
    ~ConditionPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.condition"; }
    QString name() const override { return tr("条件判断"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("根据条件判断执行分支"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    QString m_variableName;
    QString m_operator;
    QString m_compareValue;
    bool evaluateCondition(const QString& value);
};

} // namespace DeepLux
