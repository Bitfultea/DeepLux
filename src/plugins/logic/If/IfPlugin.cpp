#include "IfPlugin.h"
#include "core/engine/RunEngine.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>

namespace DeepLux {

IfPlugin::IfPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"conditionType", "BoolLink"},
        {"boolLinkText", ""},
        {"expressionString", "true"},
        {"boolInversion", false}
    };
    m_params = m_defaultParams;
    m_name = "条件分支";
    m_moduleId = "IfPlugin";
    m_category = "logic";
    m_description = "根据条件决定是否执行后续模块";
}

bool IfPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "IfPlugin initialized";
    return true;
}

bool IfPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    QString condType = params["conditionType"].toString("BoolLink");
    if (condType == "BoolLink") {
        if (params["boolLinkText"].toString().isEmpty()) {
            error = QString("Bool link cannot be empty");
            return false;
        }
    }
    return true;
}

bool IfPlugin::evaluateCondition()
{
    if (m_conditionType == ConditionType::BoolLink) {
        if (m_boolLinkText.isEmpty()) {
            Logger::instance().warning("BoolLink is empty in IfPlugin", "Logic");
            return false;
        }

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

        Logger::instance().debug(QString("IfPlugin condition: %1 = %2").arg(m_boolLinkText).arg(result), "Logic");
        return result;
    } else {
        QString expr = m_expressionString;
        if (expr.isEmpty()) {
            return false;
        }

        if (expr == "true" || expr == "True" || expr == "TRUE") {
            return !m_boolInversion;
        }
        if (expr == "false" || expr == "False" || expr == "FALSE") {
            return m_boolInversion;
        }

        Logger::instance().warning(QString("IfPlugin: Unknown expression: %1").arg(expr), "Logic");
        return false;
    }
}

bool IfPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QString condType = m_params["conditionType"].toString("BoolLink");
    if (condType == "Expression") {
        m_conditionType = ConditionType::Expression;
    } else {
        m_conditionType = ConditionType::BoolLink;
    }
    m_boolLinkText = m_params["boolLinkText"].toString();
    m_expressionString = m_params["expressionString"].toString("true");
    m_boolInversion = m_params["boolInversion"].toBool(false);

    bool result = evaluateCondition();
    output.setData("if_result", result);
    output.setData("if_passed", result);

    Logger::instance().info(QString("IfPlugin: condition evaluated to %1").arg(result ? "true" : "false"), "Logic");

    return result;
}

QWidget* IfPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QComboBox* typeCombo = new QComboBox();
    typeCombo->addItem(tr("布尔链接"), "BoolLink");
    typeCombo->addItem(tr("表达式"), "Expression");
    int idx = typeCombo->findData(m_params["conditionType"].toString("BoolLink"));
    if (idx >= 0) typeCombo->setCurrentIndex(idx);

    QLineEdit* boolLinkEdit = new QLineEdit(m_params["boolLinkText"].toString());
    QLineEdit* exprEdit = new QLineEdit(m_params["expressionString"].toString("true"));
    QCheckBox* invertCheck = new QCheckBox(tr("反转条件"));
    invertCheck->setChecked(m_params["boolInversion"].toBool(false));

    formLayout->addRow(tr("条件类型:"), typeCombo);
    formLayout->addRow(tr("布尔链接:"), boolLinkEdit);
    formLayout->addRow(tr("表达式:"), exprEdit);
    formLayout->addRow(tr(""), invertCheck);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["conditionType"] = typeCombo->currentData().toString();
        boolLinkEdit->setEnabled(typeCombo->currentData().toString() == "BoolLink");
        exprEdit->setEnabled(typeCombo->currentData().toString() == "Expression");
    });

    connect(boolLinkEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["boolLinkText"] = text;
    });

    connect(exprEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["expressionString"] = text;
    });

    connect(invertCheck, &QCheckBox::toggled, this, [=](bool checked) {
        m_params["boolInversion"] = checked;
    });

    boolLinkEdit->setEnabled(m_params["conditionType"].toString("BoolLink") == "BoolLink");
    exprEdit->setEnabled(m_params["conditionType"].toString("BoolLink") == "Expression");

    return widget;
}

// IfEndPlugin

IfEndPlugin::IfEndPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_name = "条件结束";
    m_moduleId = "IfEndPlugin";
    m_category = "logic";
    m_description = "条件分支结束";
}

bool IfEndPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;
    Logger::instance().debug("IfEndPlugin: condition branch ended", "Logic");
    return true;
}

QWidget* IfEndPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    QLabel* label = new QLabel(tr("(条件结束，无需配置)"));
    layout->addWidget(label);
    return widget;
}

} // namespace DeepLux
