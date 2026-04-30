#include "AgentActor.h"
#include "ToolSchema.h"
#include "manager/ProjectManager.h"
#include "manager/PluginManager.h"
#include "engine/RunEngine.h"
#include "model/Project.h"
#include "common/Logger.h"

#include <QUndoCommand>
#include <QDebug>

namespace DeepLux {

// --- Undo Commands ---

class AgentCreateProjectCmd : public QUndoCommand
{
public:
    AgentCreateProjectCmd(const QString& name, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_name(name) {}

    void undo() override {
        ProjectManager::instance().closeProject();
    }
    void redo() override {
        Project* proj = ProjectManager::instance().newProject();
        if (proj) proj->setName(m_name);
    }

private:
    QString m_name;
};

class AgentAddModuleCmd : public QUndoCommand
{
public:
    AgentAddModuleCmd(const QString& moduleId, const QString& instanceName, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_moduleId(moduleId), m_instanceName(instanceName) {}

    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) proj->removeModule(m_instanceName);
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj) return;
        ModuleInstance inst;
        inst.id = m_instanceName.isEmpty() ? QString("%1_%2").arg(m_moduleId).arg(QDateTime::currentDateTime().toMSecsSinceEpoch()) : m_instanceName;
        inst.moduleId = m_moduleId;
        inst.name = m_instanceName.isEmpty() ? inst.id : m_instanceName;
        proj->addModule(inst);
    }

private:
    QString m_moduleId;
    QString m_instanceName;
};

class AgentRemoveModuleCmd : public QUndoCommand
{
public:
    AgentRemoveModuleCmd(const QString& instanceId, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_instanceId(instanceId) {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) {
            ModuleInstance* inst = proj->findModule(instanceId);
            if (inst) m_backup = *inst;
        }
    }

    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj && !m_backup.id.isEmpty()) {
            proj->addModule(m_backup);
        }
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) proj->removeModule(m_instanceId);
    }

private:
    QString m_instanceId;
    ModuleInstance m_backup;
};

AgentActor::AgentActor(QObject* parent)
    : QObject(parent)
    , m_undoStack(new QUndoStack(this))
{
}

AgentActor::~AgentActor() = default;

QJsonObject AgentActor::executeTool(const QString& toolName, const QJsonObject& params)
{
    ToolSchema& schema = ToolSchema::instance();
    if (!schema.hasTool(toolName)) {
        QString err = QString("Unknown tool: %1").arg(toolName);
        emit toolError(toolName, err);
        return QJsonObject{{"error", err}};
    }

    QJsonObject result;
    if (toolName == "create_project") {
        result = createProject(params);
    } else if (toolName == "add_module") {
        result = addModule(params);
    } else if (toolName == "remove_module") {
        result = removeModule(params);
    } else if (toolName == "set_param") {
        result = setParam(params);
    } else if (toolName == "connect_modules") {
        result = connectModules(params);
    } else if (toolName == "disconnect_modules") {
        result = disconnectModules(params);
    } else if (toolName == "run_flow") {
        result = runFlow(params);
    } else if (toolName == "stop_flow") {
        result = stopFlow(params);
    } else if (toolName == "get_flow_state") {
        result = getFlowState(params);
    } else if (toolName == "get_available_plugins") {
        result = getAvailablePlugins(params);
    } else if (toolName == "save_project") {
        result = saveProject(params);
    } else {
        QString err = QString("Tool not yet implemented: %1").arg(toolName);
        emit toolError(toolName, err);
        return QJsonObject{{"error", err}};
    }

    emit toolExecuted(toolName, result);
    return result;
}

QJsonObject AgentActor::executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName)
{
    if (tools.isEmpty()) {
        return QJsonObject{{"error", "No tools to execute"}};
    }

    m_undoStack->beginMacro(macroName);

    QJsonArray results;
    for (const auto& pair : tools) {
        QJsonObject result = executeTool(pair.first, pair.second);
        QJsonObject entry;
        entry["tool"] = pair.first;
        entry["params"] = pair.second;
        entry["result"] = result;
        results.append(entry);
    }

    m_undoStack->endMacro();

    return QJsonObject{{"status", "completed"}, {"results", results}};
}

QJsonObject AgentActor::createProject(const QJsonObject& params)
{
    QString name = params.value("name").toString("unnamed");
    m_undoStack->push(new AgentCreateProjectCmd(name));
    return QJsonObject{{"status", "created"}, {"name", name}};
}

QJsonObject AgentActor::addModule(const QJsonObject& params)
{
    QString plugin = params.value("plugin").toString();
    QString instanceName = params.value("instanceName").toString();
    if (plugin.isEmpty()) {
        return QJsonObject{{"error", "Missing 'plugin' parameter"}};
    }
    m_undoStack->push(new AgentAddModuleCmd(plugin, instanceName));
    return QJsonObject{{"status", "added"}, {"plugin", plugin}};
}

QJsonObject AgentActor::removeModule(const QJsonObject& params)
{
    QString instanceId = params.value("instanceId").toString();
    if (instanceId.isEmpty()) {
        return QJsonObject{{"error", "Missing 'instanceId' parameter"}};
    }
    m_undoStack->push(new AgentRemoveModuleCmd(instanceId));
    return QJsonObject{{"status", "removed"}, {"instanceId", instanceId}};
}

QJsonObject AgentActor::setParam(const QJsonObject& params)
{
    QString instanceId = params.value("instanceId").toString();
    QString key = params.value("key").toString();
    QString value = params.value("value").toString();

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj) return QJsonObject{{"error", "No project opened"}};

    ModuleInstance* inst = proj->findModule(instanceId);
    if (!inst) return QJsonObject{{"error", QString("Module not found: %1").arg(instanceId)}};

    QJsonObject p = inst->params;
    p[key] = value;
    inst->params = p;

    return QJsonObject{{"status", "param_set"}, {"instanceId", instanceId}, {"key", key}, {"value", value}};
}

QJsonObject AgentActor::connectModules(const QJsonObject& params)
{
    QString fromId = params.value("fromId").toString();
    QString toId = params.value("toId").toString();

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj) return QJsonObject{{"error", "No project opened"}};

    ModuleConnection conn;
    conn.fromModuleId = fromId;
    conn.toModuleId = toId;
    proj->addConnection(conn);

    return QJsonObject{{"status", "connected"}, {"from", fromId}, {"to", toId}};
}

QJsonObject AgentActor::disconnectModules(const QJsonObject& params)
{
    QString fromId = params.value("fromId").toString();
    QString toId = params.value("toId").toString();

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj) return QJsonObject{{"error", "No project opened"}};

    proj->removeConnection(fromId, toId);
    return QJsonObject{{"status", "disconnected"}, {"from", fromId}, {"to", toId}};
}

QJsonObject AgentActor::runFlow(const QJsonObject& params)
{
    QString mode = params.value("mode").toString("once");
    if (mode == "cycle") {
        RunEngine::instance().start();
    } else {
        RunEngine::instance().runOnce();
    }
    return QJsonObject{{"status", "started"}, {"mode", mode}};
}

QJsonObject AgentActor::stopFlow(const QJsonObject& params)
{
    Q_UNUSED(params);
    RunEngine::instance().stop();
    return QJsonObject{{"status", "stopped"}};
}

QJsonObject AgentActor::getFlowState(const QJsonObject& params)
{
    Q_UNUSED(params);
    Project* proj = ProjectManager::instance().currentProject();
    if (!proj) return QJsonObject{{"error", "No project opened"}};

    QJsonArray modules;
    for (const ModuleInstance& m : proj->modules()) {
        QJsonObject obj;
        obj["id"] = m.id;
        obj["moduleId"] = m.moduleId;
        obj["name"] = m.name;
        obj["params"] = m.params;
        modules.append(obj);
    }

    QJsonArray connections;
    for (const ModuleConnection& c : proj->connections()) {
        QJsonObject obj;
        obj["from"] = c.fromModuleId;
        obj["to"] = c.toModuleId;
        connections.append(obj);
    }

    return QJsonObject{{"modules", modules}, {"connections", connections}};
}

QJsonObject AgentActor::getAvailablePlugins(const QJsonObject& params)
{
    Q_UNUSED(params);
    QStringList modules = PluginManager::instance().availableModules();
    QJsonArray arr;
    for (const QString& m : modules) arr.append(m);
    return QJsonObject{{"plugins", arr}};
}

QJsonObject AgentActor::saveProject(const QJsonObject& params)
{
    QString path = params.value("path").toString();
    bool ok = ProjectManager::instance().saveProject(path);
    return QJsonObject{{"status", ok ? "saved" : "failed"}};
}

} // namespace DeepLux
