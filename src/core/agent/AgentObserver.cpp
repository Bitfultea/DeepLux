#include "AgentObserver.h"
#include "GuiEvent.h"
#include "manager/ProjectManager.h"
#include "engine/RunEngine.h"
#include "manager/PluginManager.h"
#include "model/Project.h"
#include "common/Logger.h"

#include <QDebug>
#include <QJsonObject>

namespace DeepLux {

AgentObserver::AgentObserver(QObject* parent)
    : QObject(parent)
{
    m_eventRing.reserve(MAX_EVENTS);
}

AgentObserver::~AgentObserver() = default;

bool AgentObserver::initialize()
{
    if (m_initialized) return true;

    connect(&ProjectManager::instance(), &ProjectManager::projectCreated,
            this, &AgentObserver::onProjectCreated);
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened,
            this, &AgentObserver::onProjectOpened);
    connect(&ProjectManager::instance(), &ProjectManager::projectSaved,
            this, &AgentObserver::onProjectSaved);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
            this, &AgentObserver::onProjectClosed);

    connect(&RunEngine::instance(), &RunEngine::runStarted,
            this, &AgentObserver::onRunStarted);
    connect(&RunEngine::instance(), &RunEngine::runFinished,
            this, &AgentObserver::onRunFinished);
    connect(&RunEngine::instance(), &RunEngine::moduleStarted,
            this, &AgentObserver::onModuleStarted);
    connect(&RunEngine::instance(), &RunEngine::moduleFinished,
            this, &AgentObserver::onModuleFinished);

    connect(&PluginManager::instance(), &PluginManager::pluginLoaded,
            this, &AgentObserver::onPluginLoaded);
    connect(&PluginManager::instance(), &PluginManager::pluginUnloaded,
            this, &AgentObserver::onPluginUnloaded);

    Project* proj = ProjectManager::instance().currentProject();
    if (proj) {
        connectProjectSignals(proj);
    }

    connect(&ProjectManager::instance(), &ProjectManager::projectOpened,
            this, [this](Project* newProj) {
                if (newProj) connectProjectSignals(newProj);
            });
    connect(&ProjectManager::instance(), &ProjectManager::projectCreated,
            this, [this](Project* newProj) {
                if (newProj) connectProjectSignals(newProj);
            });

    // Logger
    connect(&Logger::instance(), &Logger::logAdded,
            this, &AgentObserver::onLogAdded);

    m_initialized = true;
    return true;
}

void AgentObserver::connectProjectSignals(Project* proj)
{
    if (!proj) return;
    connect(proj, &Project::moduleAdded,
            this, &AgentObserver::onModuleAdded);
    connect(proj, &Project::moduleRemoved,
            this, &AgentObserver::onModuleRemoved);
    connect(proj, &Project::connectionAdded,
            this, &AgentObserver::onConnectionAdded);
    connect(proj, &Project::connectionRemoved,
            this, &AgentObserver::onConnectionRemoved);
}

void AgentObserver::shutdown()
{
    if (!m_initialized) return;
    disconnect(this, nullptr, nullptr, nullptr);
    m_initialized = false;
}

QList<GuiEvent> AgentObserver::recentEvents(int count) const
{
    QList<GuiEvent> result;
    int total = m_ringFull ? MAX_EVENTS : m_ringHead;
    int start = qMax(0, total - count);
    for (int i = start; i < total; ++i) {
        int idx = m_ringFull ? (m_ringHead + i) % MAX_EVENTS : i;
        result.append(m_eventRing[idx]);
    }
    return result;
}

QList<GuiEvent> AgentObserver::allEvents() const
{
    return recentEvents(MAX_EVENTS);
}

void AgentObserver::pushEvent(const GuiEvent& event)
{
    if (m_eventRing.size() < MAX_EVENTS) {
        m_eventRing.append(event);
        m_ringHead = m_eventRing.size();
    } else {
        m_eventRing[m_ringHead % MAX_EVENTS] = event;
        m_ringHead++;
        m_ringFull = true;
    }
    emit guiEventOccurred(event);
}

void AgentObserver::onProjectCreated(Project* project)
{
    QJsonObject det;
    if (project) {
        det["name"] = project->name();
        det["path"] = project->filePath();
    }
    pushEvent(GuiEvent(GuiEventType::ProjectCreated, "ProjectManager", det));
}

void AgentObserver::onProjectOpened(Project* project)
{
    QJsonObject det;
    if (project) {
        det["name"] = project->name();
        det["path"] = project->filePath();
    }
    pushEvent(GuiEvent(GuiEventType::ProjectOpened, "ProjectManager", det));
}

void AgentObserver::onProjectSaved(Project* project)
{
    QJsonObject det;
    if (project) {
        det["name"] = project->name();
        det["path"] = project->filePath();
    }
    pushEvent(GuiEvent(GuiEventType::ProjectSaved, "ProjectManager", det));
}

void AgentObserver::onProjectClosed()
{
    pushEvent(GuiEvent(GuiEventType::ProjectClosed, "ProjectManager"));
}

void AgentObserver::onRunStarted()
{
    pushEvent(GuiEvent(GuiEventType::RunStarted, "RunEngine"));
}

void AgentObserver::onRunFinished(const RunResult& result)
{
    QJsonObject det;
    det["success"] = result.success;
    det["elapsedMs"] = static_cast<int>(result.elapsedMs);
    if (!result.success) {
        det["error"] = result.errorMessage;
    }
    pushEvent(GuiEvent(GuiEventType::RunFinished, "RunEngine", det));
}

void AgentObserver::onModuleStarted(const QString& moduleId)
{
    QJsonObject det;
    det["moduleId"] = moduleId;
    pushEvent(GuiEvent(GuiEventType::ModuleStarted, "RunEngine", det));
}

void AgentObserver::onModuleFinished(const QString& moduleId, bool success)
{
    QJsonObject det;
    det["moduleId"] = moduleId;
    det["success"] = success;
    pushEvent(GuiEvent(GuiEventType::ModuleFinished, "RunEngine", det));
}

void AgentObserver::onPluginLoaded(const QString& name)
{
    QJsonObject det;
    det["name"] = name;
    pushEvent(GuiEvent(GuiEventType::PluginLoaded, "PluginManager", det));
}

void AgentObserver::onPluginUnloaded(const QString& name)
{
    QJsonObject det;
    det["name"] = name;
    pushEvent(GuiEvent(GuiEventType::PluginUnloaded, "PluginManager", det));
}

void AgentObserver::onModuleAdded(const ModuleInstance& module)
{
    QJsonObject det;
    det["id"] = module.id;
    det["moduleId"] = module.moduleId;
    det["name"] = module.name;
    pushEvent(GuiEvent(GuiEventType::ModuleAdded, "Project", det));
}

void AgentObserver::onModuleRemoved(const QString& instanceId)
{
    QJsonObject det;
    det["id"] = instanceId;
    pushEvent(GuiEvent(GuiEventType::ModuleRemoved, "Project", det));
}

void AgentObserver::onConnectionAdded(const ModuleConnection& conn)
{
    QJsonObject det;
    det["from"] = conn.fromModuleId;
    det["to"] = conn.toModuleId;
    pushEvent(GuiEvent(GuiEventType::ConnectionCreated, "Project", det));
}

void AgentObserver::onConnectionRemoved(const QString& fromId, const QString& toId)
{
    QJsonObject det;
    det["from"] = fromId;
    det["to"] = toId;
    pushEvent(GuiEvent(GuiEventType::ConnectionRemoved, "Project", det));
}

void AgentObserver::setPropertyPanel(QObject* panel)
{
    if (m_propertyPanel) {
        disconnect(m_propertyPanel, nullptr, this, nullptr);
    }
    m_propertyPanel = panel;
    if (m_propertyPanel) {
        connect(m_propertyPanel, SIGNAL(paramsChanged(QString,QString,QVariant)),
                this, SLOT(onPropertyChanged(QString,QString,QVariant)));
    }
}

void AgentObserver::setFlowCanvas(QObject* canvas)
{
    if (m_flowCanvas) {
        disconnect(m_flowCanvas, nullptr, this, nullptr);
    }
    m_flowCanvas = canvas;
    if (m_flowCanvas) {
        connect(m_flowCanvas, SIGNAL(nodeAdded(QString)),
                this, SLOT(onNodeAdded(QString)));
        connect(m_flowCanvas, SIGNAL(nodeRemoved(QString)),
                this, SLOT(onNodeRemoved(QString)));
        connect(m_flowCanvas, SIGNAL(connectionCreated(QString,QString)),
                this, SLOT(onConnectionCreated(QString,QString)));
        connect(m_flowCanvas, SIGNAL(connectionRemoved(QString,QString)),
                this, SLOT(onConnectionRemovedCanvas(QString,QString)));
    }
}

void AgentObserver::onLogAdded(const LogEntry& entry)
{
    QJsonObject det;
    det["level"] = static_cast<int>(entry.level);
    det["message"] = entry.message;
    det["category"] = entry.category;
    pushEvent(GuiEvent(GuiEventType::LogAdded, "Logger", det));
}

void AgentObserver::onPropertyChanged(const QString& moduleId, const QString& key, const QVariant& value)
{
    QJsonObject det;
    det["moduleId"] = moduleId;
    det["key"] = key;
    det["value"] = value.toString();
    pushEvent(GuiEvent(GuiEventType::PropertyChanged, "PropertyPanel", det));
}

void AgentObserver::onNodeAdded(const QString& nodeId)
{
    QJsonObject det;
    det["nodeId"] = nodeId;
    pushEvent(GuiEvent(GuiEventType::ModuleAdded, "FlowCanvas", det));
}

void AgentObserver::onNodeRemoved(const QString& nodeId)
{
    QJsonObject det;
    det["nodeId"] = nodeId;
    pushEvent(GuiEvent(GuiEventType::ModuleRemoved, "FlowCanvas", det));
}

void AgentObserver::onConnectionCreated(const QString& fromId, const QString& toId)
{
    QJsonObject det;
    det["from"] = fromId;
    det["to"] = toId;
    pushEvent(GuiEvent(GuiEventType::ConnectionCreated, "FlowCanvas", det));
}

void AgentObserver::onConnectionRemovedCanvas(const QString& fromId, const QString& toId)
{
    QJsonObject det;
    det["from"] = fromId;
    det["to"] = toId;
    pushEvent(GuiEvent(GuiEventType::ConnectionRemoved, "FlowCanvas", det));
}

} // namespace DeepLux
