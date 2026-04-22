#include "VarDefineModule.h"
#include "core/engine/RunEngine.h"
#include "core/manager/GlobalVarManager.h"
#include "core/common/Logger.h"

namespace DeepLux {

VarDefineModule::VarDefineModule(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "变量定义";
    m_moduleId = "VarDefineModule";
    m_description = "变量定义模块";
}

QJsonObject VarDefineModule::defaultParams() const
{
    QJsonObject params;
    params["isAlwaysExe"] = true;
    return params;
}

void VarDefineModule::setParams(const QJsonObject& params)
{
    m_isAlwaysExe = params.value("isAlwaysExe").toBool(true);
}

bool VarDefineModule::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)
    Q_UNUSED(output)

    bool allSuccess = true;

    for (VarModel* var : m_localVars) {
        if (var->expression().isEmpty() || var->expression() == "NULL") {
            continue;
        }

        // Evaluate expression and set value
        QVariant result = var->evaluateExpression();
        var->setValue(result);

        // Add to global variable manager
        if (!GlobalVarManager::instance().hasVariable(var->name())) {
            GlobalVarManager::instance().addVariable(var);
        } else {
            GlobalVarManager::instance().setVariableValue(var->name(), result);
        }

        // Set to run engine output
        RunEngine::instance().setOutput(m_name, var->name(), result);

        Logger::instance().debug(QString("VarDefine: %1 = %2").arg(var->name()).arg(result.toString()), "Var");
    }

    return allSuccess;
}

} // namespace DeepLux