#pragma once

#include "core/base/ModuleBase.h"

class AgentInterfaceVersionMismatchFixture final : public DeepLux::ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "version_mismatch_metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit AgentInterfaceVersionMismatchFixture(QObject* parent = nullptr);

    int interfaceVersion() const override {
        return DeepLux::DEEPLUX_MODULE_INTERFACE_VERSION - 1;
    }

protected:
    bool process(const DeepLux::ImageData& input, DeepLux::ImageData& output) override;
    QWidget* createConfigWidget() override;
};
