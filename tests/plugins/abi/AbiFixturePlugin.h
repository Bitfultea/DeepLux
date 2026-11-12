#pragma once

#include "core/base/ModuleBase.h"

#include <QByteArray>
#include <QFile>

namespace DeepLuxTest {

class AbiFixturePlugin : public DeepLux::ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DEEPLUX_MODULE_INTERFACE_IID FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    AbiFixturePlugin() {
        const QByteArray marker = qgetenv("DEEPLUX_ABI_FIXTURE_INSTANCE_MARKER");
        if (!marker.isEmpty()) {
            QFile file(QString::fromLocal8Bit(marker));
            if (file.open(QIODevice::WriteOnly)) {
                file.write("instantiated");
            }
        }
    }

    QString moduleId() const override {
        return QStringLiteral("com.deeplux.test.abi_fixture");
    }
    QString name() const override {
        return QStringLiteral("ABI fixture");
    }
    QString category() const override {
        return QStringLiteral("test");
    }
    QString version() const override {
        return QStringLiteral("1");
    }
    QString author() const override {
        return QStringLiteral("tests");
    }
    QString description() const override {
        return QStringLiteral("Plugin ABI test fixture");
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }

protected:
    bool process(const DeepLux::ImageData&, DeepLux::ImageData&) override {
        return true;
    }
};

} // namespace DeepLuxTest
