#include "WhilePlugin.h"
#include "core/engine/RunEngine.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>

namespace DeepLux {

WhilePlugin::WhilePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"conditionVariable", ""},
        {"comparison", "NotEmpty"},
        {"compareValue", ""},
        {"maxIterations", 1000}
    };
    m_params = m_defaultParams;
    m_name = "循环";
    m_moduleId = "WhilePlugin";
    m_category = "logic";
    m_description = "当条件满足时循环执行";
}

bool WhilePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "WhilePlugin initialized";
    return true;
}

bool WhilePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["conditionVariable"].toString().isEmpty()) {
        error = QString("Condition variable cannot be empty");
        return false;
    }
    if (params["maxIterations"].toInt() <= 0) {
        error = QString("Max iterations must be greater than 0");
        return false;
    }
    return true;
}

bool WhilePlugin::evaluateCondition(const QString& value)
{
    Q_UNUSED(value);
    QString op = m_params["comparison"].toString("NotEmpty");
    QString compareValue = m_params["compareValue"].toString();

    if (op == "NotEmpty") {
        return !value.isEmpty();
    } else if (op == "Empty") {
        return value.isEmpty();
    } else if (op == "Equal") {
        return value == compareValue;
    } else if (op == "NotEqual") {
        return value != compareValue;
    } else if (op == "GreaterThan") {
        return value.toDouble() > compareValue.toDouble();
    } else if (op == "LessThan") {
        return value.toDouble() < compareValue.toDouble();
    }

    return false;
}

bool WhilePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_conditionVariable = m_params["conditionVariable"].toString();
    QString value = input.data(m_conditionVariable).toString();

    bool result = evaluateCondition(value);
    output.setData("while_result", result);

    Logger::instance().info(QString("While: condition '%1' %2 = %3")
        .arg(m_conditionVariable)
        .arg(m_params["comparison"].toString())
        .arg(result ? "true" : "false"), "Logic");

    return true;
}

QWidget* WhilePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* varEdit = new QLineEdit(m_params["conditionVariable"].toString());
    QComboBox* opCombo = new QComboBox();
    opCombo->addItem(tr("不为空"), "NotEmpty");
    opCombo->addItem(tr("为空"), "Empty");
    opCombo->addItem(tr("等于"), "Equal");
    opCombo->addItem(tr("不等于"), "NotEqual");
    opCombo->addItem(tr("大于"), "GreaterThan");
    opCombo->addItem(tr("小于"), "LessThan");
    int idx = opCombo->findData(m_params["comparison"].toString("NotEmpty"));
    if (idx >= 0) opCombo->setCurrentIndex(idx);

    QLineEdit* compareEdit = new QLineEdit(m_params["compareValue"].toString());
    QSpinBox* maxIterSpin = new QSpinBox();
    maxIterSpin->setRange(1, 100000);
    maxIterSpin->setValue(m_params["maxIterations"].toInt(1000));
    maxIterSpin->setPrefix(tr("最大迭代: "));

    formLayout->addRow(tr("条件变量:"), varEdit);
    formLayout->addRow(tr("比较操作:"), opCombo);
    formLayout->addRow(tr("比较值:"), compareEdit);
    formLayout->addRow(tr("最大迭代:"), maxIterSpin);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(varEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["conditionVariable"] = text;
    });

    connect(opCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        m_params["comparison"] = opCombo->currentData().toString();
    });

    connect(compareEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["compareValue"] = text;
    });

    connect(maxIterSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["maxIterations"] = value;
    });

    return widget;
}

QWidget* WhileEndPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("(循环结束，无需配置)")));
    return widget;
}

} // namespace DeepLux
