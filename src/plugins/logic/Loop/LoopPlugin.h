#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class LoopPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit LoopPlugin(QObject* parent = nullptr);
    ~LoopPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.loop"; }
    QString name() const override { return tr("循环"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("循环执行指定次数"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    int m_loopCount;
    int m_currentIteration;
};

} // namespace DeepLux
