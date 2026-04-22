#pragma once

#include "core/base/ModuleBase.h"
#include <QString>
#include <QWidget>

namespace DeepLux {

class WhilePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit WhilePlugin(QObject* parent = nullptr);
    ~WhilePlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.while"; }
    QString name() const override { return tr("条件循环"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("当条件满足时循环执行"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    bool evaluateCondition(const QString& value);

    enum class ConditionType {
        BoolLink,
        Expression
    };

    ConditionType m_conditionType = ConditionType::BoolLink;
    QString m_boolLinkText;
    QString m_expressionString;
    QString m_conditionVariable;
    int m_maxIterations = 100;
    int m_currentIteration = 0;
};

class WhileEndPlugin : public ModuleBase
{
    Q_OBJECT

public:
    explicit WhileEndPlugin(QObject* parent = nullptr);
    ~WhileEndPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.while.end"; }
    QString name() const override { return tr("条件循环结束"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("条件循环结束"); }

protected:
    bool process(const ImageData& input, ImageData& output) override;
    QWidget* createConfigWidget() override;
};

} // namespace DeepLux
