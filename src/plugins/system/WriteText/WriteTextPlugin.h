#pragma once

#include "core/base/ModuleBase.h"
#include <QFile>

namespace DeepLux {

class WriteTextPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit WriteTextPlugin(QObject* parent = nullptr);
    ~WriteTextPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.writetext"; }
    QString name() const override { return tr("写入文本"); }
    QString category() const override { return "system"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("将文本写入文件"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool m_writeResult = false;
};

} // namespace DeepLux