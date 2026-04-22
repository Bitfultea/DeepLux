#include "VarSetModule.h"
#include "core/engine/RunEngine.h"
#include "core/manager/GlobalVarManager.h"
#include "core/common/Logger.h"

namespace DeepLux {

VarSetModule::VarSetModule(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "变量赋值";
    m_moduleId = "VarSetModule";
    m_description = "变量赋值模块";
}

QJsonObject VarSetModule::defaultParams() const
{
    QJsonObject params;
    params["varName"] = "";
    params["varType"] = "double";
    params["expression"] = "";
    params["varValue"] = 0;
    return params;
}

void VarSetModule::setParams(const QJsonObject& params)
{
    m_varName = params.value("varName").toString();
    QString typeStr = params.value("varType").toString("double");
    m_varType = VarModel::stringToDataType(typeStr);
    m_expression = params.value("expression").toString();
    m_varValue = params.value("varValue").toVariant();
}

bool VarSetModule::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)
    Q_UNUSED(output)

    if (m_varName.isEmpty()) {
        Logger::instance().warning("VarSetModule: varName is empty", "Var");
        return false;
    }

    // 计算表达式的值
    QVariant value = m_varValue;
    if (!m_expression.isEmpty()) {
        // 简化表达式求值
        value = evaluateSimpleExpression(m_expression);
    }

    // 设置到全局变量管理器
    GlobalVarManager::instance().setVariableValue(m_varName, value);

    // 设置到运行引擎输出
    RunEngine::instance().setOutput(m_name, m_varName, value);

    Logger::instance().debug(QString("VarSet: %1 = %2").arg(m_varName).arg(value.toString()), "Var");
    return true;
}

QVariant VarSetModule::evaluateSimpleExpression(const QString& expr)
{
    // 简化表达式处理
    if (expr.isEmpty()) {
        return m_varValue;
    }

    // 检查是否是数字
    bool ok;
    double num = expr.toDouble(&ok);
    if (ok) {
        if (m_varType == VarDataType::Int) {
            return (int)num;
        }
        return num;
    }

    // 检查是否是布尔值
    if (expr == "true" || expr == "True" || expr == "TRUE") {
        return true;
    }
    if (expr == "false" || expr == "False" || expr == "FALSE") {
        return false;
    }

    // 字符串原样返回
    if (m_varType == VarDataType::String) {
        return expr;
    }

    return expr;
}

} // namespace DeepLux