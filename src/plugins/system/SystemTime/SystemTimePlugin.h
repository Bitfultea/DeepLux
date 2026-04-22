#pragma once

#include "core/base/ModuleBase.h"
#include <QDateTime>

namespace DeepLux {

class SystemTimePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit SystemTimePlugin(QObject* parent = nullptr);
    ~SystemTimePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.systemtime"; }
    QString name() const override { return tr("系统时间"); }
    QString category() const override { return "system"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("获取系统当前时间"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    QString m_timeFormat = "yyyy-MM-dd HH:mm:ss";
};

} // namespace DeepLux