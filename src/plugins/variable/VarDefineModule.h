#pragma once

#include "core/base/ModuleBase.h"
#include "core/common/VarModel.h"

namespace DeepLux {

class VarDefineModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit VarDefineModule(QObject* parent = nullptr);
    ~VarDefineModule() override = default;

    QString category() const override { return "变量工具"; }
    QString description() const override { return "变量定义模块"; }

    QJsonObject defaultParams() const override;
    void setParams(const QJsonObject& params) override;

    bool isAlwaysExe() const { return m_isAlwaysExe; }
    void setIsAlwaysExe(bool always) { m_isAlwaysExe = always; }

    QList<VarModel*>& localVars() { return m_localVars; }

signals:
    void isAlwaysExeChanged();

protected:
    bool process(const ImageData& input, ImageData& output) override;

private:
    bool m_isAlwaysExe = true;
    QList<VarModel*> m_localVars;
};

} // namespace DeepLux