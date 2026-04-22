#include "CreateStringPlugin.h"
#include "core/common/Logger.h"
#include "core/common/VarModel.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QFormLayout>

namespace DeepLux {

CreateStringPlugin::CreateStringPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"stringSource", "Fixed"},
        {"fixedString", ""},
        {"outputVarName", "newString"}
    };
    m_params = m_defaultParams;
}

bool CreateStringPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "CreateStringPlugin initialized";
    return true;
}

bool CreateStringPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QString stringSource = m_params["stringSource"].toString("Fixed");
    QString resultString;

    if (stringSource == "Input") {
        resultString = input.data("input_string").toString();
        if (resultString.isEmpty()) {
            resultString = input.data("barcode").toString();
        }
        if (resultString.isEmpty()) {
            resultString = m_params["fixedString"].toString();
        }
    } else {
        resultString = m_params["fixedString"].toString();
    }

    QString outputVarName = m_params["outputVarName"].toString("newString");
    output.setData(outputVarName, resultString);
    output.setData("string_length", resultString.length());

    Logger::instance().info(QString("CreateString: Created string '%1' with length %2")
        .arg(resultString.left(20)).arg(resultString.length()), "Variable");

    return true;
}

bool CreateStringPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["outputVarName"].toString().isEmpty()) {
        error = QString("Output variable name cannot be empty");
        return false;
    }
    return true;
}

QWidget* CreateStringPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QComboBox* sourceCombo = new QComboBox();
    sourceCombo->addItem(tr("固定字符串"), "Fixed");
    sourceCombo->addItem(tr("从输入获取"), "Input");
    QString currentSource = m_params["stringSource"].toString("Fixed");
    int index = sourceCombo->findData(currentSource);
    if (index >= 0) sourceCombo->setCurrentIndex(index);

    QLineEdit* fixedEdit = new QLineEdit(m_params["fixedString"].toString());
    QLineEdit* outputEdit = new QLineEdit(m_params["outputVarName"].toString("newString"));

    formLayout->addRow(tr("字符串来源:"), sourceCombo);
    formLayout->addRow(tr("固定字符串:"), fixedEdit);
    formLayout->addRow(tr("输出变量名:"), outputEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int idx) {
        m_params["stringSource"] = sourceCombo->itemData(idx).toString();
    });

    connect(fixedEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["fixedString"] = text;
    });

    connect(outputEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["outputVarName"] = text;
    });

    return widget;
}

} // namespace DeepLux
