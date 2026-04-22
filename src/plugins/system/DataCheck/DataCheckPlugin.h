#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class DataCheckPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit DataCheckPlugin(QObject* parent = nullptr);
    ~DataCheckPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.datacheck"; }
    QString name() const override { return tr("数据校验"); }
    QString category() const override { return "system"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("校验数据格式和范围"); }

    enum class CheckType {
        Range,
        Length,
        Format,
        Null
    };

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool checkRange(const QVariant& value, double minVal, double maxVal);
    bool checkLength(const QString& value, int minLen, int maxLen);

    CheckType m_checkType = CheckType::Range;
    bool m_checkResult = false;
    QString m_errorMessage;
};

} // namespace DeepLux