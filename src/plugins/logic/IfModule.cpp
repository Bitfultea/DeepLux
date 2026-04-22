#include "IfModule.h"
#include "core/engine/RunEngine.h"
#include "core/common/Logger.h"

namespace DeepLux {

IfModule::IfModule(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "如果";
    m_moduleId = "IfModule";
    m_description = "条件分支模块";
}

QJsonObject IfModule::defaultParams() const
{
    QJsonObject params;
    params["conditionType"] = "BoolLink";
    params["boolLinkText"] = "";
    params["expressionString"] = "true";
    params["boolInversion"] = false;
    return params;
}

void IfModule::setParams(const QJsonObject& params)
{
    QString condType = params.value("conditionType").toString("BoolLink");
    if (condType == "Expression") {
        m_conditionType = ConditionType::Expression;
    } else {
        m_conditionType = ConditionType::BoolLink;
    }

    m_boolLinkText = params.value("boolLinkText").toString();
    m_expressionString = params.value("expressionString").toString("true");
    m_boolInversion = params.value("boolInversion").toBool(false);
}

bool IfModule::evaluateCondition()
{
    if (m_conditionType == ConditionType::BoolLink) {
        if (m_boolLinkText.isEmpty()) {
            Logger::instance().warning("BoolLink is empty in IfModule", "Logic");
            return false;
        }

        // 从RunEngine获取变量值
        QString moduleName, varName;
        if (m_boolLinkText.startsWith("$")) {
            QString link = m_boolLinkText.mid(1);
            int dotIndex = link.indexOf('.');
            if (dotIndex > 0) {
                moduleName = link.left(dotIndex);
                varName = link.mid(dotIndex + 1);
            }
        }

        QVariant value = RunEngine::instance().getOutput(moduleName, varName);
        bool result = value.toBool();

        if (m_boolInversion) {
            result = !result;
        }

        Logger::instance().debug(QString("IfModule condition: %1 = %2").arg(m_boolLinkText).arg(result), "Logic");
        return result;
    }
    else {
        // 表达式求值 - 简化版本，实际需要表达式引擎
        QString expr = m_expressionString;
        if (expr.isEmpty()) {
            return false;
        }

        // 简单的表达式处理
        if (expr == "true" || expr == "True" || expr == "TRUE") {
            return !m_boolInversion;
        }
        if (expr == "false" || expr == "False" || expr == "FALSE") {
            return m_boolInversion;
        }

        Logger::instance().warning(QString("IfModule: Unknown expression: %1").arg(expr), "Logic");
        return false;
    }
}

bool IfModule::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)
    Q_UNUSED(output)

    return evaluateCondition();
}

} // namespace DeepLux