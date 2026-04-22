#pragma once

#include "core/base/ModuleBase.h"

namespace DeepLux {

class ParallelModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit ParallelModule(QObject* parent = nullptr);
    ~ParallelModule() override = default;

    QString category() const override { return "逻辑工具"; }
    QString description() const override { return "并行执行模块"; }

    QJsonObject defaultParams() const override;
    void setParams(const QJsonObject& params) override;

    int parallelCount() const { return m_parallelCount; }
    void setParallelCount(int count) { m_parallelCount = count; }

signals:
    void parallelCountChanged();

protected:
    bool process(const ImageData& input, ImageData& output) override;

private:
    int m_parallelCount = 1;
};

class ParallelEndModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit ParallelEndModule(QObject* parent = nullptr);
    ~ParallelEndModule() override = default;

    QString category() const override { return "逻辑工具"; }
    QString description() const override { return "并行结束模块"; }

protected:
    bool process(const ImageData& input, ImageData& output) override;
};

} // namespace DeepLux