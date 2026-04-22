#include "LoopModule.h"
#include "core/engine/RunEngine.h"
#include "core/common/Logger.h"

namespace DeepLux {

LoopModule::LoopModule(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "循环开始";
    m_moduleId = "LoopStartModule";
    m_description = "循环开始模块";
}

QJsonObject LoopModule::defaultParams() const
{
    QJsonObject params;
    params["cycleCount"] = 10;
    params["currentIndex"] = -1;
    return params;
}

void LoopModule::setParams(const QJsonObject& params)
{
    m_cycleCount = params.value("cycleCount").toInt(10);
    m_currentIndex = params.value("currentIndex").toInt(-1);
}

bool LoopModule::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)
    Q_UNUSED(output)

    m_currentIndex++;

    if (m_currentIndex < m_cycleCount) {
        Logger::instance().debug(QString("Loop: iteration %1 of %2")
            .arg(m_currentIndex + 1).arg(m_cycleCount), "Logic");
        return true; // 继续循环
    }

    Logger::instance().debug("Loop: completed", "Logic");
    return false; // 循环结束
}

LoopEndModule::LoopEndModule(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "循环结束";
    m_moduleId = "LoopEndModule";
    m_description = "循环结束模块";
}

bool LoopEndModule::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)
    Q_UNUSED(output)
    return false;
}

} // namespace DeepLux