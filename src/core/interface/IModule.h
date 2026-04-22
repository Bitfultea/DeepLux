#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QIcon>
#include <QWidget>

namespace DeepLux {

class ImageData;

/**
 * @brief 模块状态枚举
 */
enum class ModuleState {
    Idle,
    Running,
    Error,
    Disabled
};

/**
 * @brief 模块接口
 */
class IModule : public QObject
{
    Q_OBJECT

public:
    explicit IModule(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~IModule() = default;

    virtual QString moduleId() const = 0;
    virtual QString name() const = 0;
    virtual QString category() const = 0;
    virtual QString version() const = 0;
    virtual QString author() const = 0;
    virtual QString description() const = 0;
    virtual QIcon icon() const { return QIcon(); }

    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual bool isInitialized() const = 0;

    virtual bool execute(const ImageData& input, ImageData& output) = 0;

    virtual QJsonObject defaultParams() const = 0;
    virtual QJsonObject currentParams() const = 0;
    virtual void setParams(const QJsonObject& params) = 0;
    virtual void setParam(const QString& key, const QVariant& value) = 0;
    virtual bool validateParams(const QJsonObject& params, QString& error) const = 0;

    virtual QJsonObject toJson() const = 0;
    virtual bool fromJson(const QJsonObject& json) = 0;

    virtual QWidget* createConfigWidget() = 0;

    // Create a new instance of this module
    virtual IModule* clone() const = 0;

signals:
    void progressChanged(int percent);
    void stateChanged(ModuleState state);
    void errorOccurred(const QString& error);
    void executionCompleted(bool success);
};

} // namespace DeepLux

Q_DECLARE_INTERFACE(DeepLux::IModule, "com.deeplux.IModule/2.0")
