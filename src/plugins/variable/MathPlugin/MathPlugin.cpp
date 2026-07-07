#include "MathPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QStringList>
#include <cmath>

namespace DeepLux {

namespace {

bool isSupportedOperation(const QString& operation)
{
    static const QStringList supported = {
        "Add", "Subtract", "Multiply", "Divide", "Modulo", "Power", "Min", "Max"
    };
    return supported.contains(operation);
}

bool toFiniteDouble(const QVariant& value, double& number)
{
    bool ok = false;
    number = value.toDouble(&ok);
    return ok && std::isfinite(number);
}

} // namespace

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
    if (op == "Divide") return a / b;
    if (op == "Modulo") return std::fmod(a, b);
    if (op == "Power") return std::pow(a, b);
    if (op == "Min") return qMin(a, b);
    if (op == "Max") return qMax(a, b);
    return 0;
}

bool MathPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QJsonObject params = currentParams();
    m_operation = params["operation"].toString("Add");
    m_operandA = params["operandA"].toString();
    m_operandB = params["operandB"].toString();
    m_outputVar = params["outputVar"].toString("math_result");

    double a = 0, b = 0;

    if (!m_operandA.isEmpty()) {
        if (input.hasData(m_operandA)) {
            if (!toFiniteDouble(input.data(m_operandA), a)) {
                emit errorOccurred(tr("操作数A不是有效数字"));
                return false;
            }
        } else {
            if (!toFiniteDouble(m_operandA, a)) {
                emit errorOccurred(tr("操作数A不是有效数字"));
                return false;
            }
        }
    }

    if (!m_operandB.isEmpty()) {
        if (input.hasData(m_operandB)) {
            if (!toFiniteDouble(input.data(m_operandB), b)) {
                emit errorOccurred(tr("操作数B不是有效数字"));
                return false;
            }
        } else {
            if (!toFiniteDouble(m_operandB, b)) {
                emit errorOccurred(tr("操作数B不是有效数字"));
                return false;
            }
        }
    }

    if ((m_operation == "Divide" || m_operation == "Modulo") && b == 0.0) {
        emit errorOccurred(tr("除数不能为0"));
        return false;
    }

    double result = calculate(m_operation, a, b);
    if (!std::isfinite(result)) {
        emit errorOccurred(tr("计算结果不是有效数字"));
        return false;
    }

    output.setData(m_outputVar, result);
    output.setData(m_outputVar + "_string", QString::number(result));

    Logger::instance().info(QString("Math: %1 %2 %3 = %4")
        .arg(a).arg(m_operation).arg(b).arg(result), "Variable");

    return true;
}

bool MathPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    const QString operation = params["operation"].toString("Add");
    if (!isSupportedOperation(operation)) {
        error = QString("Math operation is unsupported");
        return false;
    }

    if (params["outputVar"].toString().trimmed().isEmpty()) {
        error = QString("Output variable name cannot be empty");
        return false;
    }

    error.clear();
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
        setParam("operation", opCombo->currentData().toString());
    });

    connect(operandAEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        setParam("operandA", text);
    });

    connect(operandBEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        setParam("operandB", text);
    });

    connect(outputEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        setParam("outputVar", text);
    });

    return widget;
}

IModule* MathPlugin::cloneImpl() const
{
    MathPlugin* clone = new MathPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
