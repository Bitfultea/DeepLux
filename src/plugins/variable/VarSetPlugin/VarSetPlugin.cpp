#include "VarSetPlugin.h"
#include "core/manager/GlobalVarManager.h"
#include "core/common/VarModel.h"
#include "core/common/Logger.h"
#include "core/engine/RunEngine.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QFormLayout>

namespace DeepLux {

VarSetPlugin::VarSetPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_moduleId = "com.deeplux.plugin.varset";
    m_name = "变量赋值";
    m_category = "variable";
    m_description = "设置已存在变量的值，支持表达式或引用其他变量";

    m_defaultParams = QJsonObject{
        {"variableName", ""},
        {"value", ""}
    };
    m_params = m_defaultParams;
}

bool VarSetPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "VarSetPlugin initialized";
    return true;
}

bool VarSetPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QString varName = m_params["variableName"].toString();
    QString valueStr = m_params["value"].toString();

    if (varName.isEmpty()) {
        Logger::instance().warning("VarSetPlugin: variableName is empty", "Variable");
        return false;
    }

    if (!GlobalVarManager::instance().hasVariable(varName)) {
        Logger::instance().warning(QString("VarSetPlugin: variable '%1' does not exist").arg(varName), "Variable");
        return false;
    }

    // Get target type from existing variable
    VarModel* existingVar = GlobalVarManager::instance().getVariable(varName);
    VarDataType targetType = existingVar ? existingVar->dataType() : VarDataType::String;

    // Evaluate the value expression
    QVariant resultValue = evaluateExpression(valueStr, targetType);

    // Set value in GlobalVarManager
    GlobalVarManager::instance().setVariableValue(varName, resultValue);

    Logger::instance().info(QString("VarSetPlugin: %1 = %2").arg(varName).arg(resultValue.toString()), "Variable");

    return true;
}

QVariant VarSetPlugin::evaluateExpression(const QString& expr, VarDataType targetType) const
{
    if (expr.isEmpty()) {
        return QVariant();
    }

    QString trimmed = expr.trimmed();

    // Check if it's a simple variable reference: ${varName}
    if (trimmed.startsWith("${") && trimmed.endsWith("}")) {
        QString refName = trimmed.mid(2, trimmed.length() - 3);
        if (GlobalVarManager::instance().hasVariable(refName)) {
            QVariant refValue = GlobalVarManager::instance().getVariableValue(refName);
            return convertValue(refValue, targetType);
        }
        return QVariant();
    }

    // Check if it's a binary expression: a + b, a - b, a * b, a / b
    static const QRegularExpression binOpRe(R"(^(-?\d+(?:\.\d+)?)\s*([+\-*/])\s*(-?\d+(?:\.\d+)?)$)");
    QRegularExpressionMatch match = binOpRe.match(trimmed);
    if (match.hasMatch()) {
        double left = match.captured(1).toDouble();
        QString op = match.captured(2);
        double right = match.captured(3).toDouble();

        double result = 0.0;
        if (op == "+") result = left + right;
        else if (op == "-") result = left - right;
        else if (op == "*") result = left * right;
        else if (op == "/") result = right != 0.0 ? left / right : 0.0;

        if (targetType == VarDataType::Int) {
            return QVariant((int)result);
        }
        return QVariant(result);
    }

    // Fall back to direct value resolution
    return resolveValue(trimmed, targetType);
}

QVariant VarSetPlugin::resolveValue(const QString& val, VarDataType targetType) const
{
    switch (targetType) {
        case VarDataType::Int: {
            bool ok = false;
            int intVal = val.toInt(&ok);
            return ok ? QVariant(intVal) : QVariant(0);
        }
        case VarDataType::Double: {
            bool ok = false;
            double dblVal = val.toDouble(&ok);
            return ok ? QVariant(dblVal) : QVariant(0.0);
        }
        case VarDataType::Bool: {
            QString lower = val.toLower();
            if (lower == "true" || lower == "1" || lower == "yes") return QVariant(true);
            if (lower == "false" || lower == "0" || lower == "no") return QVariant(false);
            return QVariant(val.toInt() != 0);
        }
        case VarDataType::String:
        default:
            return QVariant(val);
    }
}

QVariant VarSetPlugin::convertValue(const QVariant& val, VarDataType targetType) const
{
    switch (targetType) {
        case VarDataType::Int:
            return QVariant(val.toInt());
        case VarDataType::Double:
            return QVariant(val.toDouble());
        case VarDataType::Bool:
            return QVariant(val.toBool());
        case VarDataType::String:
            return QVariant(val.toString());
        default:
            return val;
    }
}

bool VarSetPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["variableName"].toString().isEmpty()) {
        error = QString("Variable name cannot be empty");
        return false;
    }
    return true;
}

QWidget* VarSetPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* nameEdit = new QLineEdit(m_params["variableName"].toString());
    QLineEdit* valueEdit = new QLineEdit(m_params["value"].toString());

    formLayout->addRow(tr("变量名:"), nameEdit);
    formLayout->addRow(tr("值 (表达式或${变量}:)"), valueEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(nameEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["variableName"] = text;
    });

    connect(valueEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["value"] = text;
    });

    return widget;
}

} // namespace DeepLux
