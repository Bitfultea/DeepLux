#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class DEEPLUX_PLUGIN_API TCPClientPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit TCPClientPlugin(QObject* parent = nullptr);

    QString moduleId() const override {
        return "com.deeplux.plugin.tcpclient";
    }
    QString name() const override {
        return tr("TCP客户端");
    }
    QString category() const override {
        return "communication";
    }
    QString version() const override {
        return "1.0.0";
    }
    QString author() const override {
        return "DeepLux Team";
    }
    QString description() const override {
        return tr("TCP客户端数据读写");
    }

    bool initialize() override;
    ExecutionResult execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) override;
    QWidget* createConfigWidget() override;

    /// Number of milliseconds waited during last connect/read
    int lastWaitMs() const {
        return m_lastWaitMs;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    int m_lastWaitMs = 0;
    bool m_writeMayHaveOccurred = false;
    QString m_lastFailureMessage;
};

} // namespace DeepLux
