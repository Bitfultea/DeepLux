#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class StopLoopModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit StopLoopModule(QObject* parent = nullptr);
    ~StopLoopModule() override = default;

    QString category() const override { return "逻辑工具"; }
    QString description() const override { return "停止循环模块"; }

    QJsonObject defaultParams() const override;
    void setParams(const QJsonObject& params) override;

    bool stopCondition() const { return m_stopCondition; }
    void setStopCondition(bool cond) { m_stopCondition = cond; }

signals:
    void stopConditionChanged();

protected:
    bool process(const ImageData& input, ImageData& output) override;

private:
    bool m_stopCondition = true;
};

} // namespace DeepLux