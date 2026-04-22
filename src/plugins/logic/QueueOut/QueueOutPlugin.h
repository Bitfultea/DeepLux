#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class QueueOutPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit QueueOutPlugin(QObject* parent = nullptr);
    ~QueueOutPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.queueout"; }
    QString name() const override { return tr("队列输出"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("从队列取出数据"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    QString m_queueName;
    QString m_outputVariable;
    bool m_peekOnly;
};

} // namespace DeepLux
