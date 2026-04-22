#include "StrFormatPlugin.h"
#include "core/manager/GlobalVarManager.h"
#include "core/common/Logger.h"
#include "core/engine/RunEngine.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QFormLayout>
#include <QComboBox>
#include <QJsonArray>

namespace DeepLux {

StrFormatPlugin::StrFormatPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_moduleId = "com.deeplux.plugin.strformat";
    m_name = "字符串格式化";
    m_category = "variable";
    m_description = "格式化字符串并输出到变量";

    m_defaultParams = QJsonObject{
        {"format", "%s_%d"},
        {"inputVariables", QJsonArray()},
        {"outputVariable", "formattedString"}
    };
    m_params = m_defaultParams;
}

bool StrFormatPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "StrFormatPlugin initialized";
    return true;
}

bool StrFormatPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QString format = m_params["format"].toString("%s");
    QJsonArray inputArr = m_params["inputVariables"].toArray();
    QString outputVar = m_params["outputVariable"].toString("formattedString");

    if (outputVar.isEmpty()) {
        Logger::instance().warning("StrFormatPlugin: outputVariable is empty", "Variable");
        return false;
    }

    // Resolve each input variable value
    QStringList resolvedValues;
    for (const QJsonValue& v : inputArr) {
        QString varExpr = v.toString();
        resolvedValues.append(resolveVarValue(varExpr));
    }

    // Perform the formatting
    QString result = performFormat(format, resolvedValues);

    // Store in GlobalVarManager
    GlobalVarManager::instance().setVariableValue(outputVar, result);

    // Set to RunEngine output
    RunEngine::instance().setOutput(m_name, outputVar, result);

    Logger::instance().info(QString("StrFormatPlugin: formatted '%1' -> '%2'")
        .arg(format).arg(result), "Variable");

    return true;
}

QString StrFormatPlugin::resolveVarValue(const QString& varExpr) const
{
    if (varExpr.isEmpty()) {
        return QString();
    }

    // Check for variable reference: ${varName}
    if (varExpr.startsWith("${") && varExpr.endsWith("}")) {
        QString varName = varExpr.mid(2, varExpr.length() - 3);
        if (GlobalVarManager::instance().hasVariable(varName)) {
            return GlobalVarManager::instance().getVariableValue(varName).toString();
        }
        return varExpr; // Return as-is if not found
    }

    // Plain value - return as-is
    return varExpr;
}

QString StrFormatPlugin::performFormat(const QString& format, const QStringList& values) const
{
    QString result = format;
    int valueIndex = 0;

    // Replace %s, %d, %f placeholders in order
    for (int i = 0; i < result.length(); ++i) {
        if (result[i] == '%' && i + 1 < result.length()) {
            QChar spec = result[i + 1];
            if (spec == 's' || spec == 'd' || spec == 'f') {
                if (valueIndex < values.size()) {
                    result.replace(i, 2, values[valueIndex]);
                    valueIndex++;
                    // After replace, adjust index since string length changed
                    i += values[valueIndex - 1].length() - 1;
                } else {
                    // No more values, replace with empty
                    result.replace(i, 2, QString());
                }
            }
        }
    }

    return result;
}

bool StrFormatPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["outputVariable"].toString().isEmpty()) {
        error = QString("Output variable name cannot be empty");
        return false;
    }
    return true;
}

QWidget* StrFormatPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* formatEdit = new QLineEdit(m_params["format"].toString());
    QLineEdit* outputEdit = new QLineEdit(m_params["outputVariable"].toString("formattedString"));

    formLayout->addRow(tr("Format (e.g. %s_%d):"), formatEdit);
    formLayout->addRow(tr("Output Variable:"), outputEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(formatEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["format"] = text;
    });

    connect(outputEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["outputVariable"] = text;
    });

    return widget;
}

} // namespace DeepLux
