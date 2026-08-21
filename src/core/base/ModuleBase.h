#pragma once

#include "../interface/IModule.h"
#include "../model/ImageData.h"

#include <memory>
#include <QMutex>

namespace DeepLux {

class CancellationToken;

/**
 * @brief 模块参数基类
 */
struct ModuleParam
{
    QString name;
    bool enabled = true;
    int posX = 0;
    int posY = 0;
    
    virtual QJsonObject toJson() const;
    virtual bool fromJson(const QJsonObject& json);
};

/**
 * @brief 模块基类
 */
class ModuleBase : public IModule
{
    Q_OBJECT

public:
    explicit ModuleBase(QObject* parent = nullptr);
    ~ModuleBase() override;

    QString id() const { return m_moduleId; }
    QString moduleId() const override { return m_moduleId; }
    QString name() const override { return m_name; }
    QString instanceName() const { return m_instanceName; }
    void setInstanceName(const QString& name) { m_instanceName = name; }
    QString category() const override { return m_category; }
    QString version() const override { return m_version; }
    QString author() const override { return m_author; }
    QString description() const override { return m_description; }
    QIcon icon() const override { return m_icon; }
    void setIcon(const QIcon& icon) override { m_icon = icon; }

    bool initialize() override;
    void shutdown() override;
    bool isInitialized() const override { return m_initialized; }

    int interfaceVersion() const override { return DEEPLUX_MODULE_INTERFACE_VERSION; }

    // 旧 process() 插件的源代码兼容入口，不属于 IModule ABI。
    bool execute(const ImageData& input, ImageData& output);
    ExecutionResult execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) override;
    void setCancellationToken(CancellationToken* token);
    CancellationToken* cancellationToken() const;

    // 端口声明（由 PluginManager 从 metadata.json 注入）
    QList<PortSpec> inputPorts() const override {
        return m_inputPorts;
    }
    QList<PortSpec> outputPorts() const override {
        return m_outputPorts;
    }
    void setPorts(const QList<PortSpec>& inputs, const QList<PortSpec>& outputs);
    bool isThreadSafe() const { return m_threadSafe; }
    void setThreadSafe(bool safe) { m_threadSafe = safe; }

    QJsonObject defaultParams() const override;
    QJsonObject currentParams() const override;
    void setParams(const QJsonObject& params) override;
    void setParam(const QString& key, const QVariant& value) override;
    bool validateParams(const QJsonObject& params, QString& error) const override;

    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;

    // Clone this module instance. cloneImpl() creates the derived object;
    // ModuleBase::clone() copies the common metadata and parameter state.
    IModule* clone() const override;

protected:
    bool isCancellationRequested() const;

    // Derived classes should override this to provide proper cloning.
    // Default implementation returns nullptr (plugin doesn't support multiple instances).
    virtual IModule* cloneImpl() const;


    virtual bool process(const ImageData& input, ImageData& output) = 0;
    virtual bool doValidateParams(const QJsonObject& params, QString& error) const;

protected:
    QString m_moduleId;
    QString m_name;
    QString m_instanceName;
    QString m_category;
    QString m_version = "1.0.0";
    QString m_author;
    QString m_description;
    QIcon m_icon;

    QJsonObject m_params;
    QJsonObject m_defaultParams;

    QList<PortSpec> m_inputPorts;
    QList<PortSpec> m_outputPorts;
    bool m_threadSafe = false;

    bool m_initialized = false;
    ModuleState m_state = ModuleState::Idle;

    mutable QMutex m_paramsMutex;
};

} // namespace DeepLux
