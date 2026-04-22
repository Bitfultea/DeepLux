#pragma once

#include "core/base/ModuleBase.h"
#include "core/common/VarModel.h"

namespace DeepLux {

class VarSetModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit VarSetModule(QObject* parent = nullptr);
    ~VarSetModule() override = default;

    QString category() const override { return "变量工具"; }
    QString description() const override { return "变量赋值模块"; }

    QJsonObject defaultParams() const override;
    void setParams(const QJsonObject& params) override;

    QString varName() const { return m_varName; }
    void setVarName(const QString& name) { m_varName = name; }

    VarDataType varType() const { return m_varType; }
    void setVarType(VarDataType type) { m_varType = type; }

    QString expression() const { return m_expression; }
    void setExpression(const QString& expr) { m_expression = expr; }

    QVariant varValue() const { return m_varValue; }
    void setVarValue(const QVariant& value) { m_varValue = value; }

signals:
    void varNameChanged();
    void varTypeChanged();
    void expressionChanged();
    void varValueChanged();

protected:
    bool process(const ImageData& input, ImageData& output) override;

private:
    QString m_varName;
    VarDataType m_varType = VarDataType::Double;
    QString m_expression;
    QVariant m_varValue;
};

} // namespace DeepLux