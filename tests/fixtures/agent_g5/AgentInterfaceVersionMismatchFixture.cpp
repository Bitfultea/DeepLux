#include "AgentInterfaceVersionMismatchFixture.h"

AgentInterfaceVersionMismatchFixture::AgentInterfaceVersionMismatchFixture(QObject* parent)
    : DeepLux::ModuleBase(parent) {
    m_moduleId = QStringLiteral("com.deeplux.test.agent_interface_version_mismatch");
    m_name = QStringLiteral("AgentInterfaceVersionMismatchFixture");
    m_category = QStringLiteral("test");
    m_version = QStringLiteral("1.0.0");
    m_author = QStringLiteral("DeepLux Test");
    m_description = QStringLiteral("IModule castable fixture reporting an older interface version");
}

bool AgentInterfaceVersionMismatchFixture::process(const DeepLux::ImageData& input, DeepLux::ImageData& output) {
    output = input;
    return true;
}

QWidget* AgentInterfaceVersionMismatchFixture::createConfigWidget() {
    return nullptr;
}
