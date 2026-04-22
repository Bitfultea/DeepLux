#pragma once

#include "core/base/ModuleBase.h"
#include <QElapsedTimer>

namespace DeepLux {

class TimeSlicePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit TimeSlicePlugin(QObject* parent = nullptr);
    ~TimeSlicePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.timeslice"; }
    QString name() const override { return tr("时间片"); }
    QString category() const override { return "system"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("测量代码执行时间"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    qint64 m_startTime = 0;
    qint64 m_elapsedMs = 0;
    QString m_sliceName;
};

} // namespace DeepLux