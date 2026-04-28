#ifndef DEEPLUX_AGENT_OBSERVER_H
#define DEEPLUX_AGENT_OBSERVER_H

#include <QObject>
#include <QList>
#include <QDateTime>

namespace DeepLux {

struct GuiEvent;
class Project;
class RunResult;

/**
 * @brief Agent GUI 状态感知器
 *
 * 以非侵入方式连接现有 Manager 的 Qt 信号，将 GUI 状态变化转换为
 * 结构化的 GuiEvent 序列，供 AgentController 消费。
 *
 * 设计原则：
 * - 不修改被观察类的接口
 * - 只监听信号，不调用方法
 * - 事件存入环形缓冲区（默认保留最近 100 个）
 */
class AgentObserver : public QObject
{
    Q_OBJECT

public:
    explicit AgentObserver(QObject* parent = nullptr);
    ~AgentObserver() override;

    bool initialize();
    void shutdown();

    // 可选：连接 PropertyPanel 和 FlowCanvas 信号（当它们可用时）
    void setPropertyPanel(QObject* panel);
    void setFlowCanvas(QObject* canvas);

    // 获取最近 N 个事件（供 LLM 上下文使用）
    QList<GuiEvent> recentEvents(int count = 50) const;

    // 获取所有事件（供审计日志使用）
    QList<GuiEvent> allEvents() const;

signals:
    void guiEventOccurred(const GuiEvent& event);

private slots:
    // ProjectManager
    void onProjectCreated(Project* project);
    void onProjectOpened(Project* project);
    void onProjectSaved(Project* project);
    void onProjectClosed();

    // RunEngine
    void onRunStarted();
    void onRunFinished(const RunResult& result);
    void onModuleStarted(const QString& moduleId);
    void onModuleFinished(const QString& moduleId, bool success);

    // PluginManager
    void onPluginLoaded(const QString& name);
    void onPluginUnloaded(const QString& name);

    // Project (data model)
    void onModuleAdded(const struct ModuleInstance& module);
    void onModuleRemoved(const QString& instanceId);
    void onConnectionAdded(const struct ModuleConnection& conn);
    void onConnectionRemoved(const QString& fromId, const QString& toId);

    // Logger
    void onLogAdded(const struct LogEntry& entry);

    // PropertyPanel (optional)
    void onPropertyChanged(const QString& moduleId, const QString& key, const QVariant& value);

    // FlowCanvas (optional)
    void onNodeAdded(const QString& nodeId);
    void onNodeRemoved(const QString& nodeId);
    void onConnectionCreated(const QString& fromId, const QString& toId);
    void onConnectionRemovedCanvas(const QString& fromId, const QString& toId);

private:
    void pushEvent(const GuiEvent& event);
    void connectProjectSignals(Project* proj);

    QObject* m_propertyPanel = nullptr;
    QObject* m_flowCanvas = nullptr;

    bool m_initialized = false;

    // 环形缓冲区：最近 100 个事件
    static constexpr int MAX_EVENTS = 100;
    QList<GuiEvent> m_eventRing;
    int m_ringHead = 0;
    bool m_ringFull = false;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_OBSERVER_H
