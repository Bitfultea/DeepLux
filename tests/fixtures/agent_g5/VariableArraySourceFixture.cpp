#include "VariableArraySourceFixture.h"

#include <QJsonArray>

VariableArraySourceFixture::VariableArraySourceFixture(QObject* parent) : DeepLux::ModuleBase(parent) {
    m_moduleId = QStringLiteral("com.deeplux.test.agent_variable_array_source");
    m_name = QStringLiteral("VariableArraySourceFixture");
    m_category = QStringLiteral("test");
    m_version = QStringLiteral("1.0.0");
    m_author = QStringLiteral("DeepLux Test");
    m_description = QStringLiteral("Deterministic ImageData array source for Agent step tests");
    m_defaultParams = {{QStringLiteral("outputVariable"), QStringLiteral("agent_registers")},
                       {QStringLiteral("values"), QJsonArray{0x1234}}};
}

bool VariableArraySourceFixture::process(const DeepLux::ImageData& input, DeepLux::ImageData& output) {
    output = input;
    const QString outputVariable = m_params.value(QStringLiteral("outputVariable")).toString().trimmed();
    const QJsonArray values = m_params.value(QStringLiteral("values")).toArray();
    if (outputVariable.isEmpty() || values.isEmpty()) {
        emit errorOccurred(tr("Fixture output variable and values must be configured"));
        return false;
    }
    output.setData(outputVariable, values);
    return true;
}

QWidget* VariableArraySourceFixture::createConfigWidget() {
    return nullptr;
}

DeepLux::IModule* VariableArraySourceFixture::cloneImpl() const {
    return new VariableArraySourceFixture;
}
