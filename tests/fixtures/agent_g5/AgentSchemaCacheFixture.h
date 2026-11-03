#pragma once

#include "core/base/ModuleBase.h"

class AgentSchemaCacheFixture final : public DeepLux::ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "schema_metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit AgentSchemaCacheFixture(QObject* parent = nullptr);

protected:
    bool process(const DeepLux::ImageData& input, DeepLux::ImageData& output) override;
    QWidget* createConfigWidget() override;
    DeepLux::IModule* cloneImpl() const override;
};
