#pragma once

#include "core/base/ModuleBase.h"
#include <QQueue>

namespace DeepLux {

class QueueInPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit QueueInPlugin(QObject* parent = nullptr);
    ~QueueInPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.queuein"; }
    QString name() const override { return tr("队列输入"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("将数据加入队列"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    QString m_queueName;
    QString m_dataVariable;
};

} // namespace DeepLux
