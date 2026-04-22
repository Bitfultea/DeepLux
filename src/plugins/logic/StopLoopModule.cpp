#include "StopLoopModule.h"
#include "core/common/Logger.h"

namespace DeepLux {

StopLoopModule::StopLoopModule(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "停止循环";
    m_moduleId = "StopLoopModule";
    m_description = "停止循环模块";
}

QJsonObject StopLoopModule::defaultParams() const
{
    QJsonObject params;
    params["stopCondition"] = true;
    return params;
}

void StopLoopModule::setParams(const QJsonObject& params)
{
    m_stopCondition = params.value("stopCondition").toBool(true);
}

bool StopLoopModule::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)
    Q_UNUSED(output)

    Logger::instance().debug("StopLoop: stopping current loop", "Logic");

    if (m_stopCondition) {
        return false;
    }
    return true;
}

} // namespace DeepLux