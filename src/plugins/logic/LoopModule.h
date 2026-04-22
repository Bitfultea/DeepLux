#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class LoopModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit LoopModule(QObject* parent = nullptr);
    ~LoopModule() override = default;

    QString category() const override { return "逻辑工具"; }
    QString description() const override { return "循环开始模块"; }

    QJsonObject defaultParams() const override;
    void setParams(const QJsonObject& params) override;

    int cycleCount() const { return m_cycleCount; }
    void setCycleCount(int count) { m_cycleCount = count; }

    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int index) { m_currentIndex = index; }

signals:
    void cycleCountChanged();
    void currentIndexChanged();

protected:
    bool process(const ImageData& input, ImageData& output) override;

private:
    int m_cycleCount = 1;
    int m_currentIndex = -1;
};

class LoopEndModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit LoopEndModule(QObject* parent = nullptr);
    ~LoopEndModule() override = default;

    QString category() const override { return "逻辑工具"; }
    QString description() const override { return "循环结束模块"; }

protected:
    bool process(const ImageData& input, ImageData& output) override;
};

} // namespace DeepLux