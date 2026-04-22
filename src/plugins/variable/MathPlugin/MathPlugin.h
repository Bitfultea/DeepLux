#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class MathPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit MathPlugin(QObject* parent = nullptr);
    ~MathPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.math"; }
    QString name() const override { return tr("数学运算"); }
    QString category() const override { return "variable"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("基本数学运算"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    double calculate(const QString& op, double a, double b);

    QString m_operation;
    QString m_operandA;
    QString m_operandB;
    QString m_outputVar;
};

} // namespace DeepLux
