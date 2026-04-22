#pragma once

#include "core/base/ModuleBase.h"
#include <QStringList>

namespace DeepLux {

class SplitStringPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit SplitStringPlugin(QObject* parent = nullptr);
    ~SplitStringPlugin() override = default;

    QString moduleId() const override { return "com.deeplux.plugin.splitstring"; }
    QString name() const override { return tr("分割字符串"); }
    QString category() const override { return "variable"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("按分隔符分割字符串"); }

    bool initialize() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    QString m_inputString;
    QString m_separator;
    bool m_useRegex;
    QString m_outputPrefix;
    int m_maxSplits;
};

} // namespace DeepLux
