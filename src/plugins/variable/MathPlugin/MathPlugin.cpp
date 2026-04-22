#include "MathPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>

namespace DeepLux {

MathPlugin::MathPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"operation", "Add"},
        {"operandA", ""},
        {"operandB", ""},
        {"outputVar", "math_result"}
    };
    m_params = m_defaultParams;
}

bool MathPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "MathPlugin initialized";
    return true;
}

double MathPlugin::calculate(const QString& op, double a, double b)
{
    if (op == "Add") return a + b;
    if (op == "Subtract") return a - b;
    if (op == "Multiply") return a * b;
    if (op == "Divide") return (b != 0) ? (a / b) : 0;
    if (op == "Modulo") return fmod(a, b);
    if (op == "Power") return pow(a, b);
    if (op == "Min") return qMin(a, b);
    if (op == "Max") return qMax(a, b);
    return 0;
}

bool MathPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_operation = m_params["operation"].toString("Add");
    m_operandA = m_params["operandA"].toString();
    m_operandB = m_params["operandB"].toString();
    m_outputVar = m_params["outputVar"].toString("math_result");

    double a = 0, b = 0;

    if (!m_operandA.isEmpty()) {
        bool ok;
        a = input.data(m_operandA).toDouble(&ok);
        if (!ok) a = m_operandA.toDouble(&ok);
    }

    if (!m_operandB.isEmpty()) {
        bool ok;
        b = input.data(m_operandB).toDouble(&ok);
        if (!ok) b = m_operandB.toDouble(&ok);
    }

    double result = calculate(m_operation, a, b);
    output.setData(m_outputVar, result);
    output.setData(m_outputVar + "_string", QString::number(result));

    Logger::instance().info(QString("Math: %1 %2 %3 = %4")
        .arg(a).arg(m_operation).arg(b).arg(result), "Variable");

    return true;
}

bool MathPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["outputVar"].toString().isEmpty()) {
        error = QString("Output variable name cannot be empty");
        return false;
    }
    return true;
}

QWidget* MathPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QComboBox* opCombo = new QComboBox();
    opCombo->addItem(tr("加法 (+)"), "Add");
    opCombo->addItem(tr("减法 (-)"), "Subtract");
    opCombo->addItem(tr("乘法 (*)"), "Multiply");
    opCombo->addItem(tr("除法 (/)"), "Divide");
    opCombo->addItem(tr("取模 (%)"), "Modulo");
    opCombo->addItem(tr("幂运算"), "Power");
    opCombo->addItem(tr("最小值"), "Min");
    opCombo->addItem(tr("最大值"), "Max");
    int idx = opCombo->findData(m_params["operation"].toString("Add"));
    if (idx >= 0) opCombo->setCurrentIndex(idx);

    QLineEdit* operandAEdit = new QLineEdit(m_params["operandA"].toString());
    QLineEdit* operandBEdit = new QLineEdit(m_params["operandB"].toString());
    QLineEdit* outputEdit = new QLineEdit(m_params["outputVar"].toString("math_result"));

    formLayout->addRow(tr("运算:"), opCombo);
    formLayout->addRow(tr("操作数A:"), operandAEdit);
    formLayout->addRow(tr("操作数B:"), operandBEdit);
    formLayout->addRow(tr("输出变量:"), outputEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(opCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["operation"] = opCombo->currentData().toString();
    });

    connect(operandAEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["operandA"] = text;
    });

    connect(operandBEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["operandB"] = text;
    });

    connect(outputEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["outputVar"] = text;
    });

    return widget;
}

} // namespace DeepLux
