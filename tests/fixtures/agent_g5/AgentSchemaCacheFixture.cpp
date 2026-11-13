#include "AgentSchemaCacheFixture.h"

#include <QByteArray>

AgentSchemaCacheFixture::AgentSchemaCacheFixture(QObject* parent) : DeepLux::ModuleBase(parent) {
    m_moduleId = QStringLiteral("com.deeplux.test.agent_schema_cache");
    m_name = QStringLiteral("AgentSchemaCacheFixture");
    m_category = QStringLiteral("test");
    m_version = QStringLiteral("1.0.0");
    m_author = QStringLiteral("DeepLux Test");
    m_description = QStringLiteral("Schema cache invalidation fixture");
    const QByteArray version = qgetenv("DEEPLUX_TEST_SCHEMA_VERSION");
    if (version == "v2") {
        m_defaultParams = {{QStringLiteral("newField"), QStringLiteral("schema-v2")},
                           {QStringLiteral("sharedField"), 2}};
    } else {
        m_defaultParams = {{QStringLiteral("oldField"), QStringLiteral("schema-v1")},
                           {QStringLiteral("sharedField"), 1}};
    }
}

bool AgentSchemaCacheFixture::process(const DeepLux::ImageData& input, DeepLux::ImageData& output) {
    output = input;
    return true;
}

QWidget* AgentSchemaCacheFixture::createConfigWidget() {
    return nullptr;
}

DeepLux::IModule* AgentSchemaCacheFixture::cloneImpl() const {
    return new AgentSchemaCacheFixture;
}
