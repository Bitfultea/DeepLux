#include "StrFormatPlugin.h"

#include "core/common/Logger.h"
#include "core/common/VarModel.h"
#include "core/engine/RunEngine.h"
#include "core/manager/GlobalVarManager.h"

#include <QComboBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace DeepLux {

StrFormatPlugin::StrFormatPlugin(QObject* parent) : ModuleBase(parent) {
    m_moduleId = "com.deeplux.plugin.strformat";
    m_name = "字符串格式化";
    m_category = "variable";
    m_description = "格式化字符串并输出到变量";

    m_defaultParams =
        QJsonObject{{"format", "%s_%d"}, {"inputVariables", QJsonArray()}, {"outputVariable", "formattedString"}};
    m_params = m_defaultParams;
}

bool StrFormatPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "StrFormatPlugin initialized";
    return true;
}

bool StrFormatPlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    QJsonObject params = currentParams();
    QString format = params["format"].toString("%s");
    QJsonArray inputArr = params["inputVariables"].toArray();
    QString outputVar = params["outputVariable"].toString("formattedString");

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

    GlobalVarManager& variables = GlobalVarManager::instance();
    if (variables.hasVariable(outputVar)) {
        variables.setVariableValue(outputVar, result);
    } else {
        variables.addVariable(new VarModel(outputVar, VarDataType::String, result));
    }

    output.setData(outputVar, result);
    RunEngine::instance().setOutput(m_name, outputVar, result);

    Logger::instance().info(QString("StrFormatPlugin: formatted '%1' -> '%2'").arg(format).arg(result), "Variable");

    return true;
}

QString StrFormatPlugin::resolveVarValue(const QString& varExpr) const {
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

QString StrFormatPlugin::performFormat(const QString& format, const QStringList& values) const {
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

bool StrFormatPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    if (params["outputVariable"].toString().isEmpty()) {
        error = QString("Output variable name cannot be empty");
        return false;
    }
    if (!params["inputVariables"].isArray()) {
        error = QString("Input variables must be an array");
        return false;
    }
    return true;
}

QWidget* StrFormatPlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* formatEdit = new QLineEdit(m_params["format"].toString());
    QLineEdit* inputsEdit = new QLineEdit();
    QLineEdit* outputEdit = new QLineEdit(m_params["outputVariable"].toString("formattedString"));

    QStringList inputVariables;
    for (const QJsonValue& value : m_params["inputVariables"].toArray()) {
        inputVariables.append(value.toString());
    }
    inputsEdit->setText(inputVariables.join(", "));

    formLayout->addRow(tr("Format (e.g. %s_%d):"), formatEdit);
    formLayout->addRow(tr("Input Variables (comma separated):"), inputsEdit);
    formLayout->addRow(tr("Output Variable:"), outputEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(formatEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("format", text); });

    connect(inputsEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        QJsonArray inputs;
        for (const QString& value : text.split(',', Qt::SkipEmptyParts)) {
            const QString trimmed = value.trimmed();
            if (!trimmed.isEmpty()) {
                inputs.append(trimmed);
            }
        }
        setParam("inputVariables", inputs);
    });

    connect(outputEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("outputVariable", text); });

    return widget;
}

IModule* StrFormatPlugin::cloneImpl() const {
    auto* clone = new StrFormatPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
