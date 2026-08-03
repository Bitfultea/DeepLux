#include "ConditionPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>

namespace DeepLux {

ConditionPlugin::ConditionPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"variableName", ""},
        {"operator", "NotEmpty"},
        {"compareValue", ""}
    };
    m_params = m_defaultParams;
}

bool ConditionPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "ConditionPlugin initialized";
    return true;
}

bool ConditionPlugin::evaluateCondition(const QString& value)
{
    QJsonObject params = currentParams();
    QString op = params["operator"].toString("NotEmpty");
    QString compareValue = params["compareValue"].toString();

    if (op == "NotEmpty") {
        return !value.isEmpty();
    } else if (op == "Empty") {
        return value.isEmpty();
    } else if (op == "Equal") {
        return value == compareValue;
    } else if (op == "NotEqual") {
        return value != compareValue;
    } else if (op == "Contains") {
        return value.contains(compareValue);
    } else if (op == "GreaterThan") {
        return value.toDouble() > compareValue.toDouble();
    } else if (op == "LessThan") {
        return value.toDouble() < compareValue.toDouble();
    } else if (op == "StartsWith") {
        return value.startsWith(compareValue);
    } else if (op == "EndsWith") {
        return value.endsWith(compareValue);
    }

    return false;
}

bool ConditionPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QJsonObject params = currentParams();
    m_variableName = params["variableName"].toString();
    if (m_variableName.isEmpty()) {
        emit errorOccurred(tr("变量名不能为空"));
        return false;
    }

    QString value = input.data(m_variableName).toString();
    bool result = evaluateCondition(value);

    output.setData("condition_result", result);
    output.setData("condition_passed", result);

    Logger::instance().info(QString("Condition: '%1' %2 = %3")
        .arg(m_variableName)
        .arg(params["operator"].toString())
        .arg(result ? "true" : "false"), "Logic");

    return true;
}

bool ConditionPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    // 阶段 2: variableName 等外部资源允许暂时为空，由 process() 在运行时报告"未配置"
    Q_UNUSED(params)
    error.clear();
    return true;
}

QWidget* ConditionPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* varEdit = new QLineEdit(m_params["variableName"].toString());

    QComboBox* opCombo = new QComboBox();
    opCombo->addItem(tr("不为空"), "NotEmpty");
    opCombo->addItem(tr("为空"), "Empty");
    opCombo->addItem(tr("等于"), "Equal");
    opCombo->addItem(tr("不等于"), "NotEqual");
    opCombo->addItem(tr("包含"), "Contains");
    opCombo->addItem(tr("大于"), "GreaterThan");
    opCombo->addItem(tr("小于"), "LessThan");
    opCombo->addItem(tr("开头是"), "StartsWith");
    opCombo->addItem(tr("结尾是"), "EndsWith");
    int idx = opCombo->findData(m_params["operator"].toString("NotEmpty"));
    if (idx >= 0) opCombo->setCurrentIndex(idx);

    QLineEdit* compareEdit = new QLineEdit(m_params["compareValue"].toString());

    formLayout->addRow(tr("变量名:"), varEdit);
    formLayout->addRow(tr("条件:"), opCombo);
    formLayout->addRow(tr("比较值:"), compareEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(varEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        setParam("variableName", text);
    });

    connect(opCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int) {
        setParam("operator", opCombo->currentData().toString());
    });

    connect(compareEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        setParam("compareValue", text);
    });

    return widget;
}

IModule* ConditionPlugin::cloneImpl() const
{
    return new ConditionPlugin();
}

} // namespace DeepLux
