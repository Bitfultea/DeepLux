#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class StrFormatPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit StrFormatPlugin(QObject* parent = nullptr);
    ~StrFormatPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.strformat"; }
    QString name() const override { return tr("字符串格式化"); }
    QString category() const override { return "variable"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("格式化字符串并输出到变量"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    QString performFormat(const QString& format, const QStringList& values) const;
    QString resolveVarValue(const QString& varExpr) const;

    QString m_format;
    QStringList m_inputVariables;
    QString m_outputVariable;
};

} // namespace DeepLux
