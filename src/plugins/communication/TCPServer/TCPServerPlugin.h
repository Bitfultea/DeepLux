#pragma once

#include "core/base/ModuleBase.h"

#include <functional>

class QTcpSocket;

namespace DeepLux {

class DEEPLUX_PLUGIN_API TCPServerPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    struct ResponseWriteOutcome {
        qint64 acceptedBytes = 0;
        bool completed = false;
        QString failureStage;
        QString errorMessage;
    };
#ifdef DEEPLUX_ENABLE_TEST_HOOKS
    using ResponseWriterTestHook = std::function<ResponseWriteOutcome(QTcpSocket&, const QByteArray&, int timeoutMs)>;
#endif

    explicit TCPServerPlugin(QObject* parent = nullptr);
    ~TCPServerPlugin() override;

#ifdef DEEPLUX_ENABLE_TEST_HOOKS
    static void setResponseWriterTestHook(ResponseWriterTestHook hook);
#endif

    QString moduleId() const override {
        return "com.deeplux.plugin.tcpserver";
    }
    QString name() const override {
        return tr("TCP服务器");
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
        return tr("TCP服务器数据收发");
    }

    bool initialize() override;
    QWidget* createConfigWidget() override;
    ExecutionResult execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    ResponseWriteOutcome writeResponse(QTcpSocket& socket, const QByteArray& bytes, int timeoutMs) const;

    qint64 m_responseAcceptedBytes = 0;
    qint64 m_responseExpectedBytes = 0;
    QString m_responseFailureStage;
};

} // namespace DeepLux
