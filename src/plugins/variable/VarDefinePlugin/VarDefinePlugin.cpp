#include "VarDefinePlugin.h"
#include "core/manager/GlobalVarManager.h"
#include "core/common/VarModel.h"
#include "core/common/Logger.h"
#include "core/engine/RunEngine.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QFormLayout>
#include <QMessageBox>

namespace DeepLux {

VarDefinePlugin::VarDefinePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_moduleId = "com.deeplux.plugin.vardefine";
    m_name = "变量定义";
    m_category = "variable";
    m_description = "在GlobalVarManager中创建新变量";

    m_defaultParams = QJsonObject{
        {"variableName", ""},
        {"variableType", "double"},
        {"initialValue", 0}
    };
    m_params = m_defaultParams;
}

bool VarDefinePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "VarDefinePlugin initialized";
    return true;
}

bool VarDefinePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QString varName = m_params["variableName"].toString();
    QString varType = m_params["variableType"].toString("double");
    QVariant initValue = m_params["initialValue"].toVariant();

    if (varName.isEmpty()) {
        Logger::instance().warning("VarDefinePlugin: variableName is empty", "Variable");
        return false;
    }

    // Convert string type to VarDataType
    VarDataType dataType = VarModel::stringToDataType(varType);

    // Coerce initial value to the correct type
    QVariant coercedValue = initValue;
    switch (dataType) {
        case VarDataType::Int:
        case VarDataType::Double: {
            bool ok = false;
            double num = initValue.toDouble(&ok);
            if (dataType == VarDataType::Int) {
                coercedValue = initValue.toInt(&ok);
            } else {
                coercedValue = ok ? num : 0.0;
            }
            break;
        }
        case VarDataType::Bool:
            coercedValue = initValue.toBool();
            break;
        case VarDataType::String:
            coercedValue = initValue.toString();
            break;
        default:
            break;
    }

    // Create or update variable in GlobalVarManager
    if (GlobalVarManager::instance().hasVariable(varName)) {
        GlobalVarManager::instance().setVariableValue(varName, coercedValue);
    } else {
        VarModel* newVar = new VarModel(varName, dataType, coercedValue);
        GlobalVarManager::instance().addVariable(newVar);
    }

    // Set to RunEngine output
    RunEngine::instance().setOutput(m_name, varName, coercedValue);

    Logger::instance().info(QString("VarDefinePlugin: %1 (%2) = %3")
        .arg(varName).arg(varType).arg(coercedValue.toString()), "Variable");

    return true;
}

bool VarDefinePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["variableName"].toString().isEmpty()) {
        error = QString("Variable name cannot be empty");
        return false;
    }

    QString varType = params["variableType"].toString("double");
    if (!QStringList{"int", "double", "string", "bool"}.contains(varType)) {
        error = QString("Unsupported variable type: %1").arg(varType);
        return false;
    }

    return true;
}

QWidget* VarDefinePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* nameEdit = new QLineEdit(m_params["variableName"].toString());
    QComboBox* typeCombo = new QComboBox();
    typeCombo->addItem("int", "int");
    typeCombo->addItem("double", "double");
    typeCombo->addItem("string", "string");
    typeCombo->addItem("bool", "bool");

    QString currentType = m_params["variableType"].toString("double");
    int typeIdx = typeCombo->findData(currentType);
    if (typeIdx >= 0) typeCombo->setCurrentIndex(typeIdx);

    QLineEdit* initEdit = new QLineEdit(m_params["initialValue"].toString());

    formLayout->addRow(tr("Variable Name:"), nameEdit);
    formLayout->addRow(tr("Variable Type:"), typeCombo);
    formLayout->addRow(tr("Initial Value:"), initEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(nameEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["variableName"] = text;
    });

    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int idx) {
        m_params["variableType"] = typeCombo->itemData(idx).toString();
    });

    connect(initEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        QString varType = m_params["variableType"].toString("double");
        if (varType == "int") {
            m_params["initialValue"] = text.toInt();
        } else if (varType == "double") {
            m_params["initialValue"] = text.toDouble();
        } else if (varType == "bool") {
            m_params["initialValue"] = (text == "true" || text == "1");
        } else {
            m_params["initialValue"] = text;
        }
    });

    return widget;
}

} // namespace DeepLux
