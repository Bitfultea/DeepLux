#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class ParallelPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit ParallelPlugin(QObject* parent = nullptr);
    ~ParallelPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.parallel"; }
    QString name() const override { return tr("并行开始"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("并行执行多个分支"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    int m_parallelCount = 2;
    QStringList m_branchNames;
};

class ParallelEndPlugin : public ModuleBase
{
    Q_OBJECT

public:
    explicit ParallelEndPlugin(QObject* parent = nullptr);
    ~ParallelEndPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.parallel.end"; }
    QString name() const override { return tr("并行结束"); }
    QString category() const override { return "logic"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("并行结束，汇总结果"); }

protected:
    bool process(const ImageData& input, ImageData& output) override;
    QWidget* createConfigWidget() override;
};

} // namespace DeepLux
