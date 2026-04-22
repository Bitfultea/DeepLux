#pragma once

#include "core/base/ModuleBase.h"
#include <QTableWidget>

namespace DeepLux {

class TableOutPutPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit TableOutPutPlugin(QObject* parent = nullptr);
    ~TableOutPutPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.tableoutput"; }
    QString name() const override { return tr("表格输出"); }
    QString category() const override { return "system"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("以表格形式输出数据"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    int m_rowCount = 0;
    int m_colCount = 0;
};

} // namespace DeepLux