#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QIcon>
#include <QWidget>

#include "core/deeplux/ControlFlowType.h"

namespace DeepLux {

class ImageData;

/**
 * @brief 模块接口版本号
 * 当 IModule 接口发生变化（新增/修改/删除虚函数）时，必须递增此版本号，
 * 并重新编译所有插件，否则旧插件的虚表将与主程序不匹配，导致调用错位。
 */
constexpr int DEEPLUX_MODULE_INTERFACE_VERSION = 1;

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
    virtual void setIcon(const QIcon& icon) { Q_UNUSED(icon) }

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

    // 控制流类型 — 模块根据自身语义声明，RunEngine 据此决定执行顺序
    virtual ControlFlowType flowControlType() const { return ControlFlowType::Sequential; }

    /**
     * @brief 返回插件编译时使用的 IModule 接口版本号
     * PluginManager 加载插件后会校验此值，若不匹配则拒绝加载并给出明确警告。
     */
    virtual int interfaceVersion() const = 0;

signals:
    void progressChanged(int percent);
    void stateChanged(ModuleState state);
    void errorOccurred(const QString& error);
    void executionCompleted(bool success);
};

} // namespace DeepLux

Q_DECLARE_INTERFACE(DeepLux::IModule, "com.deeplux.IModule/2.0")
