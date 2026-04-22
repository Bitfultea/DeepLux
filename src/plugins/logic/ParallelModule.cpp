#include "ParallelModule.h"
#include "core/common/Logger.h"

namespace DeepLux {

ParallelModule::ParallelModule(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "并行开始";
    m_moduleId = "ParallelModule";
    m_description = "并行执行模块";
}

QJsonObject ParallelModule::defaultParams() const
{
    QJsonObject params;
    params["parallelCount"] = 2;
    return params;
}

void ParallelModule::setParams(const QJsonObject& params)
{
    m_parallelCount = params.value("parallelCount").toInt(2);
}

bool ParallelModule::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)
    Q_UNUSED(output)

    Logger::instance().debug(QString("Parallel: starting %1 branches").arg(m_parallelCount), "Logic");
    return true;
}

ParallelEndModule::ParallelEndModule(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "并行结束";
    m_moduleId = "ParallelEndModule";
    m_description = "并行结束模块";
}

bool ParallelEndModule::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)
    Q_UNUSED(output)

    Logger::instance().debug("Parallel: all branches completed", "Logic");
    return true;
}

} // namespace DeepLux