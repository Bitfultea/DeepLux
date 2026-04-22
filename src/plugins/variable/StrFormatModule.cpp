#include "StrFormatModule.h"
#include "core/engine/RunEngine.h"
#include "core/common/Logger.h"

namespace DeepLux {

StrFormatModule::StrFormatModule(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "字符串格式化";
    m_moduleId = "StrFormatModule";
    m_description = "字符串格式化模块";
}

QJsonObject StrFormatModule::defaultParams() const
{
    QJsonObject params;
    params["inputString"] = "";
    params["formatPattern"] = "%s";
    return params;
}

void StrFormatModule::setParams(const QJsonObject& params)
{
    m_inputString = params.value("inputString").toString();
    m_formatPattern = params.value("formatPattern").toString("%s");
}

bool StrFormatModule::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)
    Q_UNUSED(output)

    QString result = formatString(m_formatPattern, m_inputString);

    // Set output
    RunEngine::instance().setOutput(m_name, "result", result);

    Logger::instance().debug(QString("StrFormat: formatted to '%1'").arg(result), "Var");

    return true;
}

QString StrFormatModule::formatString(const QString& pattern, const QString& input)
{
    // Simple string formatting - replace %s with input
    QString result = pattern;
    result.replace("%s", input);
    return result;
}

} // namespace DeepLux