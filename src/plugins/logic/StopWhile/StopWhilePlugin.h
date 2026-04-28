#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class StopWhilePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit StopWhilePlugin(QObject* parent = nullptr);
    ~StopWhilePlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.stopwhile"; }
    QString name() const override { return tr("停止循环"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("放在循环内用于提前退出"); }
    ControlFlowType flowControlType() const override { return ControlFlowType::StopLoop; }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    bool m_stopCondition = true;
};

} // namespace DeepLux
