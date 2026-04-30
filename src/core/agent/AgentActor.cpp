#include "AgentActor.h"
#include "ToolSchema.h"
#include "manager/ProjectManager.h"
#include "manager/PluginManager.h"
#include "engine/RunEngine.h"
#include "base/ModuleBase.h"
#include "model/Project.h"
#include "common/Logger.h"

#include <QUndoCommand>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
        inst.id = m_instanceName.isEmpty()
            ? QString("%1_%2").arg(m_moduleId).arg(QDateTime::currentDateTime().toMSecsSinceEpoch())
            : m_instanceName;
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
        if (proj && !m_backup.id.isEmpty()) proj->addModule(m_backup);
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) proj->removeModule(m_instanceId);
    }
private:
    QString m_instanceId;
    ModuleInstance m_backup;
};

class AgentSetParamCmd : public QUndoCommand
{
public:
    AgentSetParamCmd(const QString& instanceId, const QString& key,
                     const QJsonValue& oldVal, const QJsonValue& newVal,
                     QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_instanceId(instanceId), m_key(key)
        , m_oldVal(oldVal), m_newVal(newVal) {}

    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj) return;
        ModuleInstance* inst = proj->findModule(m_instanceId);
        if (inst) inst->params[m_key] = m_oldVal;
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj) return;
        ModuleInstance* inst = proj->findModule(m_instanceId);
        if (inst) inst->params[m_key] = m_newVal;
    }
private:
    QString m_instanceId, m_key;
    QJsonValue m_oldVal, m_newVal;
};

class AgentConnectCmd : public QUndoCommand
{
public:
    AgentConnectCmd(const QString& fromId, const QString& toId, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_fromId(fromId), m_toId(toId)
    { setText(QString("Connect %1 → %2").arg(fromId, toId)); }

    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) proj->removeConnection(m_fromId, m_toId);
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj) return;
        ModuleConnection conn;
        conn.fromModuleId = m_fromId;
        conn.toModuleId = m_toId;
        proj->addConnection(conn);
    }
private:
    QString m_fromId, m_toId;
};

class AgentDisconnectCmd : public QUndoCommand
{
public:
    AgentDisconnectCmd(const QString& fromId, const QString& toId, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_fromId(fromId), m_toId(toId)
    { setText(QString("Disconnect %1 → %2").arg(fromId, toId)); }

    void undo() override {
        // undo of disconnect = reconnect
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj) return;
        ModuleConnection conn;
        conn.fromModuleId = m_fromId;
        conn.toModuleId = m_toId;
        proj->addConnection(conn);
    }
    void redo() override {
        // redo of disconnect = disconnect again
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) proj->removeConnection(m_fromId, m_toId);
    }
private:
    QString m_fromId, m_toId;
};

// --- AgentActor ---

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
    if (toolName == "create_project")           result = createProject(params);
    else if (toolName == "add_module")           result = addModule(params);
    else if (toolName == "remove_module")         result = removeModule(params);
    else if (toolName == "set_param")             result = setParam(params);
    else if (toolName == "connect_modules")       result = connectModules(params);
    else if (toolName == "disconnect_modules")    result = disconnectModules(params);
    else if (toolName == "run_flow")              result = runFlow(params);
    else if (toolName == "stop_flow")             result = stopFlow(params);
    else if (toolName == "get_flow_state")        result = getFlowState(params);
    else if (toolName == "get_available_plugins") result = getAvailablePlugins(params);
    else if (toolName == "save_project")          result = saveProject(params);
    else if (toolName == "get_module_params_schema") result = getModuleParamsSchema(params);
    else if (toolName == "get_run_results")       result = getRunResults(params);
    else if (toolName == "open_project")          result = openProject(params);
    else if (toolName == "pause_flow")            result = pauseFlow(params);
    else if (toolName == "resume_flow")           result = resumeFlow(params);
    else if (toolName == "read_documentation")    result = readDocumentation(params);
    else {
        QString err = QString("Tool not yet implemented: %1").arg(toolName);
        emit toolError(toolName, err);
        return QJsonObject{{"error", err}};
    }

    emit toolExecuted(toolName, result);
    return result;
}

QJsonObject AgentActor::executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName)
{
    if (tools.isEmpty()) return QJsonObject{{"error", "No tools to execute"}};

    m_undoStack->beginMacro(macroName);

    QJsonArray results;
    for (const auto& pair : tools) {
        QJsonObject result = executeTool(pair.first, pair.second);
        QJsonObject entry;
        entry["tool"] = pair.first;
        entry["result"] = result;
        results.append(entry);
    }

    m_undoStack->endMacro();

    return QJsonObject{{"status", "completed"}, {"results", results}};
}

// --- Tool Handlers ---

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
    if (plugin.isEmpty()) return QJsonObject{{"error", "Missing 'plugin' parameter"}};
    m_undoStack->push(new AgentAddModuleCmd(plugin, instanceName));
    return QJsonObject{{"status", "added"}, {"plugin", plugin}};
}

QJsonObject AgentActor::removeModule(const QJsonObject& params)
{
    QString instanceId = params.value("instanceId").toString();
    if (instanceId.isEmpty()) return QJsonObject{{"error", "Missing 'instanceId' parameter"}};
    m_undoStack->push(new AgentRemoveModuleCmd(instanceId));
    return QJsonObject{{"status", "removed"}, {"instanceId", instanceId}};
}

QJsonObject AgentActor::setParam(const QJsonObject& params)
{
    QString instanceId = params.value("instanceId").toString();
    QString key = params.value("key").toString();
    QJsonValue value = params.value("value");

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj) return QJsonObject{{"error", "No project opened"}};

    ModuleInstance* inst = proj->findModule(instanceId);
    if (!inst) return QJsonObject{{"error", QString("Module not found: %1").arg(instanceId)}};

    QJsonValue oldVal = inst->params.value(key);
    inst->params[key] = value;
    m_undoStack->push(new AgentSetParamCmd(instanceId, key, oldVal, value));

    return QJsonObject{{"status", "param_set"}, {"instanceId", instanceId}, {"key", key}};
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
    m_undoStack->push(new AgentConnectCmd(fromId, toId));

    return QJsonObject{{"status", "connected"}, {"from", fromId}, {"to", toId}};
}

QJsonObject AgentActor::disconnectModules(const QJsonObject& params)
{
    QString fromId = params.value("fromId").toString();
    QString toId = params.value("toId").toString();

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj) return QJsonObject{{"error", "No project opened"}};

    proj->removeConnection(fromId, toId);
    m_undoStack->push(new AgentDisconnectCmd(fromId, toId));

    return QJsonObject{{"status", "disconnected"}, {"from", fromId}, {"to", toId}};
}

QJsonObject AgentActor::runFlow(const QJsonObject& params)
{
    QString mode = params.value("mode").toString("once");
    if (mode == "cycle") RunEngine::instance().start();
    else RunEngine::instance().runOnce();
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

QJsonObject AgentActor::getModuleParamsSchema(const QJsonObject& params)
{
    QString instanceId = params.value("instanceId").toString();
    if (instanceId.isEmpty()) {
        return QJsonObject{{"error", "Missing 'instanceId' parameter"}};
    }

    PluginManager& pm = PluginManager::instance();
    Project* proj = ProjectManager::instance().currentProject();
    if (!proj) return QJsonObject{{"error", "No project opened"}};

    ModuleInstance* inst = proj->findModule(instanceId);
    if (!inst) return QJsonObject{{"error", QString("Module not found: %1").arg(instanceId)}};

    // Create a temporary module instance to get default params
    IModule* mod = pm.createModule(inst->moduleId);
    if (!mod) return QJsonObject{{"error", QString("Cannot create module: %1").arg(inst->moduleId)}};

    QJsonObject schema;
    schema["moduleId"] = inst->moduleId;
    schema["name"] = mod->name();
    schema["description"] = mod->description();
    schema["defaultParams"] = mod->defaultParams();
    schema["currentParams"] = inst->params;

    delete mod;  // 临时实例用完即删
    return schema;
}

QJsonObject AgentActor::getRunResults(const QJsonObject& params)
{
    Q_UNUSED(params);
    RunEngine& engine = RunEngine::instance();
    return QJsonObject{
        {"totalRuns", engine.totalRuns()},
        {"successRuns", engine.successRuns()},
        {"failedRuns", engine.failedRuns()},
        {"lastElapsedMs", engine.lastElapsedMs()},
        {"isRunning", engine.isRunning()}
    };
}

QJsonObject AgentActor::openProject(const QJsonObject& params)
{
    QString path = params.value("path").toString();
    if (path.isEmpty()) return QJsonObject{{"error", "Missing 'path' parameter"}};

    bool ok = ProjectManager::instance().openProject(path);
    if (!ok) return QJsonObject{{"error", "Failed to open project"}};

    Project* proj = ProjectManager::instance().currentProject();
    return QJsonObject{
        {"status", "opened"},
        {"name", proj ? proj->name() : "unknown"}
    };
}

QJsonObject AgentActor::pauseFlow(const QJsonObject& params)
{
    Q_UNUSED(params);
    RunEngine::instance().pause();
    return QJsonObject{{"status", "paused"}};
}

QJsonObject AgentActor::resumeFlow(const QJsonObject& params)
{
    Q_UNUSED(params);
    RunEngine::instance().resume();
    return QJsonObject{{"status", "resumed"}};
}

QJsonObject AgentActor::readDocumentation(const QJsonObject& params)
{
    QString topic = params.value("topic").toString().toLower();
    if (topic.isEmpty()) {
        return QJsonObject{{"error", "Missing 'topic' parameter"}};
    }

    // 知识库路径（与源码同级）
    QString knowledgePath = QCoreApplication::applicationDirPath()
        + "/../src/core/agent/knowledge/DEEPLUX.md";
    // 开发模式 fallback
    if (!QFile::exists(knowledgePath)) {
        knowledgePath = QDir::currentPath() + "/src/core/agent/knowledge/DEEPLUX.md";
    }

    QFile file(knowledgePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // 内嵌摘要作为 fallback
        return QJsonObject{
            {"topic", topic},
            {"content", "Knowledge base not found. Use get_module_params_schema(instanceId) "
                        "to discover parameters for specific modules."}
        };
    }

    QString content = QString::fromUtf8(file.readAll());
    file.close();

    // 搜索相关段落：按 ## 分节，匹配 topic 关键词
    QStringList sections = content.split("\n## ");
    QStringList relevant;

    for (const QString& sec : sections) {
        if (sec.toLower().contains(topic) || topic == "all") {
            // 截断过长段落
            if (sec.length() > 2000) {
                relevant.append(sec.left(2000) + "\n... (truncated, use more specific topic)");
            } else {
                relevant.append(sec.trimmed());
            }
        }
    }

    if (relevant.isEmpty()) {
        return QJsonObject{
            {"topic", topic},
            {"content", QString("No documentation found for '%1'. "
                                "Try broader topics like 'workflow', 'modules', 'params', "
                                "or a module name like 'FindCircle'.").arg(topic)}
        };
    }

    return QJsonObject{
        {"topic", topic},
        {"sections", relevant.size()},
        {"content", relevant.join("\n\n")}
    };
}

} // namespace DeepLux
