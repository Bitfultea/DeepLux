#ifndef DEEPLUX_GUI_EVENT_H
#define DEEPLUX_GUI_EVENT_H

#include <QString>
#include <QJsonObject>
#include <QDateTime>

namespace DeepLux {

/**
 * @brief GUI 事件类型枚举
 *
 * Observer 将 Qt 信号转换为结构化的 GuiEvent，供 AgentController 消费。
 */
enum class GuiEventType
{
    Unknown,

    // 项目生命周期
    ProjectCreated,
    ProjectOpened,
    ProjectSaved,
    ProjectClosed,

    // 模块变更
    ModuleAdded,
    ModuleRemoved,
    ModuleReordered,

    // 属性变更
    PropertyChanged,

    // 连接变更
    ConnectionCreated,
    ConnectionRemoved,

    // 运行状态
    RunStarted,
    RunFinished,
    ModuleStarted,
    ModuleFinished,

    // 插件
    PluginLoaded,
    PluginUnloaded,

    // 图像/显示
    ImageDisplayed,

    // 日志
    LogAdded
};

/**
 * @brief GUI 事件结构体
 *
 * 所有 GUI 状态变化都序列化为 GuiEvent，放入环形缓冲区供 LLM 上下文使用。
 */
struct GuiEvent
{
    QDateTime timestamp;
    GuiEventType type = GuiEventType::Unknown;
    QString source;      // 信号来源类名
    QJsonObject details; // 事件详细参数

    GuiEvent() = default;
    GuiEvent(GuiEventType t, const QString& src, const QJsonObject& det = QJsonObject())
        : timestamp(QDateTime::currentDateTime())
        , type(t)
        , source(src)
        , details(det)
    {}

    QString typeString() const;
    QJsonObject toJson() const;
};


/**
 * @brief Agent 操作日志条目
 */
struct AgentActionLogEntry
{
    QDateTime timestamp;
    QString actor;      // "Agent" or "User"
    QString action;     // action name
    QString params;     // JSON string of params
    QString result;     // success / error / pending
    bool undoable = false;
};

} // namespace DeepLux

#endif // DEEPLUX_GUI_EVENT_H
