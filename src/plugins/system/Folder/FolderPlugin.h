#pragma once

#include "core/base/ModuleBase.h"
#include <QDir>

namespace DeepLux {

class FolderPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit FolderPlugin(QObject* parent = nullptr);
    ~FolderPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.folder"; }
    QString name() const override { return tr("文件夹操作"); }
    QString category() const override { return "system"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("创建、删除、遍历文件夹"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool m_operationResult = false;
};

} // namespace DeepLux