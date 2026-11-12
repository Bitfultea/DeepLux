#pragma once

#include <QObject>

class AgentAbiMismatchFixture final : public QObject {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "abi_metadata.json")
};
