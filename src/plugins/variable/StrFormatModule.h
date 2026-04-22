#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class StrFormatModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit StrFormatModule(QObject* parent = nullptr);
    ~StrFormatModule() override = default;

    QString category() const override { return "变量工具"; }
    QString description() const override { return "字符串格式化模块"; }

    QJsonObject defaultParams() const override;
    void setParams(const QJsonObject& params) override;

    QString inputString() const { return m_inputString; }
    void setInputString(const QString& str) { m_inputString = str; }

    QString formatPattern() const { return m_formatPattern; }
    void setFormatPattern(const QString& pattern) { m_formatPattern = pattern; }

signals:
    void inputStringChanged();
    void formatPatternChanged();

protected:
    bool process(const ImageData& input, ImageData& output) override;

private:
    QString formatString(const QString& pattern, const QString& input);

    QString m_inputString;
    QString m_formatPattern;
};

} // namespace DeepLux