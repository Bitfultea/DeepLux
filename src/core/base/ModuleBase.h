#pragma once

#include "../interface/IModule.h"
#include "../model/ImageData.h"

#include <memory>
#include <QMutex>

namespace DeepLux {

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
    QString category() const override { return m_category; }
    QString version() const override { return m_version; }
    QString author() const override { return m_author; }
    QString description() const override { return m_description; }
    QIcon icon() const override { return m_icon; }

    bool initialize() override;
    void shutdown() override;
    bool isInitialized() const override { return m_initialized; }

    bool execute(const ImageData& input, ImageData& output) override;

    QJsonObject defaultParams() const override;
    QJsonObject currentParams() const override;
    void setParams(const QJsonObject& params) override;
    void setParam(const QString& key, const QVariant& value) override;
    bool validateParams(const QJsonObject& params, QString& error) const override;

    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject& json) override;

    // Clone this module instance - each derived class must implement cloneImpl()
    IModule* clone() const override;

protected:
    // Derived classes should override this to provide proper cloning
    // Default implementation returns nullptr (plugin doesn't support multiple instances)
    virtual IModule* cloneImpl() const;


    virtual bool process(const ImageData& input, ImageData& output) = 0;
    virtual bool doValidateParams(const QJsonObject& params, QString& error) const;

protected:
    QString m_moduleId;
    QString m_name;
    QString m_category;
    QString m_version = "1.0.0";
    QString m_author;
    QString m_description;
    QIcon m_icon;

    QJsonObject m_params;
    QJsonObject m_defaultParams;

    bool m_initialized = false;
    ModuleState m_state = ModuleState::Idle;

    mutable QMutex m_paramsMutex;
};

} // namespace DeepLux
