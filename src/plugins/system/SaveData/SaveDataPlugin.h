#pragma once

#include "core/base/ModuleBase.h"

#include <QFile>
#include <QTextStream>

namespace DeepLux {

class SaveDataPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit SaveDataPlugin(QObject* parent = nullptr);
    ~SaveDataPlugin() override;
    using ModuleBase::execute;

    QString moduleId() const override {
        return "com.deeplux.plugin.savedata";
    }
    QString name() const override {
        return tr("保存数据");
    }
    QString category() const override {
        return "system";
    }
    QString version() const override {
        return "1.0.0";
    }
    QString author() const override {
        return "DeepLux Team";
    }
    QString description() const override {
        return tr("将数据保存到文件");
    }

    bool initialize() override;
    void shutdown() override;
    ExecutionResult execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool saveToJson(const QString& filePath, const QVariantMap& data);
    bool saveToCsv(const QString& filePath, const QVariantMap& data);
    bool finishTextWrite(QFile& file, QTextStream& stream, const QString& format, const QVariantMap& data);

    bool m_saveResult = false;
    bool m_writeUnconfirmed = false;
    QString m_writeDiagnostics;
};

} // namespace DeepLux
