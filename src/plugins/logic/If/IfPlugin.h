#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class IfPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit IfPlugin(QObject* parent = nullptr);
    ~IfPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.if"; }
    QString name() const override { return tr("条件分支"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("根据条件决定是否执行后续模块"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    bool evaluateCondition();

    enum class ConditionType {
        BoolLink,
        Expression
    };

    ConditionType m_conditionType = ConditionType::BoolLink;
    QString m_boolLinkText;
    QString m_expressionString;
    bool m_boolInversion = false;
};

class IfEndPlugin : public ModuleBase
{
    Q_OBJECT

public:
    explicit IfEndPlugin(QObject* parent = nullptr);
    ~IfEndPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.if.end"; }
    QString name() const override { return tr("条件结束"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("条件分支结束"); }

protected:
    bool process(const ImageData& input, ImageData& output) override;
    QWidget* createConfigWidget() override;
};

} // namespace DeepLux
