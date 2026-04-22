#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class IfModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit IfModule(QObject* parent = nullptr);
    ~IfModule() override = default;

    QString category() const override { return "逻辑工具"; }
    QString description() const override { return "条件分支模块"; }

    QJsonObject defaultParams() const override;
    void setParams(const QJsonObject& params) override;

    bool evaluateCondition();

    enum class ConditionType {
        BoolLink,
        Expression
    };
    ConditionType conditionType() const { return m_conditionType; }
    void setConditionType(ConditionType type) { m_conditionType = type; }

    QString boolLinkText() const { return m_boolLinkText; }
    void setBoolLinkText(const QString& text) { m_boolLinkText = text; }

    QString expressionString() const { return m_expressionString; }
    void setExpressionString(const QString& expr) { m_expressionString = expr; }

    bool boolInversion() const { return m_boolInversion; }
    void setBoolInversion(bool inv) { m_boolInversion = inv; }

signals:
    void conditionTypeChanged();
    void boolLinkTextChanged();
    void expressionStringChanged();
    void boolInversionChanged();

protected:
    bool process(const ImageData& input, ImageData& output) override;

private:
    ConditionType m_conditionType = ConditionType::BoolLink;
    QString m_boolLinkText;
    QString m_expressionString;
    bool m_boolInversion = false;
    bool m_compileSuccess = false;
};

} // namespace DeepLux