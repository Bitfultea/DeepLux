#include "AgentActor.h"

#include "SamPredictor.h"
#include "ToolSchema.h"
#include "io/LabelMeImporter.h"
#include "base/ModuleBase.h"
#include "common/Logger.h"
#include "engine/RunEngine.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"
#include "model/Annotation.h"
#include "model/Project.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUndoCommand>
#include <QUuid>
#include <memory>

static void ensureAgentResources() {
    static const bool initialized = []() {
        Q_INIT_RESOURCE(agent_resources);
        return true;
    }();
    Q_UNUSED(initialized)
}

namespace DeepLux {

namespace {
QString validationError(const QString& toolName, const QJsonObject& params) {
    ToolDefinition def = ToolSchema::instance().findTool(toolName);
    if (def.name.isEmpty())
        return QString("Unknown tool: %1").arg(toolName);

    for (const ToolParam& param : def.params) {
        const QJsonValue value = params.value(param.name);
        if (param.required &&
            (value.isUndefined() || value.isNull() || (value.isString() && value.toString().trimmed().isEmpty()))) {
            return QString("Missing required parameter: %1").arg(param.name);
        }
        if (!param.enumValues.isEmpty() && !value.isUndefined() && !value.isNull() &&
            !param.enumValues.contains(value.toString())) {
            return QString("Invalid value for %1: %2").arg(param.name, value.toString());
        }
    }
    return QString();
}

const PortSpec* findPort(const QList<PortSpec>& ports, const QString& id) {
    for (const PortSpec& port : ports) {
        if (port.id == id)
            return &port;
    }
    return nullptr;
}

bool portTypesCompatible(const PortSpec& output, const PortSpec& input) {
    return output.control == input.control &&
           (output.type == DataType::Any || input.type == DataType::Any || output.type == input.type);
}
} // namespace

// --- Undo Commands ---

class AgentAddModuleCmd : public QUndoCommand {
public:
    AgentAddModuleCmd(const QString& moduleId, const QString& instanceId, const QString& displayName,
                      QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_moduleId(moduleId), m_instanceId(instanceId), m_displayName(displayName) {}

    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj)
            proj->removeModule(m_instanceId);
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        ModuleInstance inst;
        inst.id = m_instanceId;
        inst.moduleId = m_moduleId;
        inst.name = m_displayName;
        proj->addModule(inst);
    }

    QString instanceId() const {
        return m_instanceId;
    }

private:
    QString m_moduleId;
    QString m_instanceId;
    QString m_displayName;
};

class AgentRemoveModuleCmd : public QUndoCommand {
public:
    AgentRemoveModuleCmd(const QString& instanceId, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_instanceId(instanceId) {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) {
            const std::optional<ModuleInstance> inst = proj->moduleById(instanceId);
            if (inst)
                m_backup = *inst;
            // 阶7 Agent 评审修复（P1-3）：Project::removeModule 不级联删连接，
            // 由命令备份关联连接并在 redo/undo 两侧维护连接一致性
            for (const ModuleConnection& c : proj->connections()) {
                if (c.fromModuleId == instanceId || c.toModuleId == instanceId)
                    m_connBackup.append(c);
            }
        }
    }
    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj && !m_backup.id.isEmpty()) {
            proj->addModule(m_backup);
            for (const ModuleConnection& c : m_connBackup)
                proj->addConnection(c);
        }
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        for (const ModuleConnection& c : m_connBackup)
            proj->removeConnectionWithPorts(c.fromModuleId, c.fromPort, c.toModuleId, c.toPort);
        proj->removeModule(m_instanceId);
    }

private:
    QString m_instanceId;
    ModuleInstance m_backup;
    QList<ModuleConnection> m_connBackup;
};

class AgentSetParamCmd : public QUndoCommand {
public:
    AgentSetParamCmd(const QString& instanceId, const QString& key, bool hadOldValue, const QJsonValue& oldVal,
                     const QJsonValue& newVal, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_instanceId(instanceId), m_key(key), m_hadOldValue(hadOldValue), m_oldVal(oldVal),
          m_newVal(newVal) {}

    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        if (m_hadOldValue)
            proj->setModuleParam(m_instanceId, m_key, m_oldVal);
        else
            proj->removeModuleParam(m_instanceId, m_key);
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        proj->setModuleParam(m_instanceId, m_key, m_newVal);
    }

private:
    QString m_instanceId, m_key;
    bool m_hadOldValue;
    QJsonValue m_oldVal, m_newVal;
};

class AgentConnectCmd : public QUndoCommand {
public:
    AgentConnectCmd(const QString& fromId, const QString& fromPort, const QString& toId, const QString& toPort,
                    const QString& edgeType, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_fromId(fromId), m_toId(toId), m_fromPort(fromPort), m_toPort(toPort),
          m_edgeType(edgeType) {
        setText(QString("Connect %1 → %2").arg(fromId, toId));
    }

    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        proj->removeConnectionWithPorts(m_fromId, m_fromPort, m_toId, m_toPort);
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        ModuleConnection conn;
        conn.fromModuleId = m_fromId;
        conn.toModuleId = m_toId;
        conn.fromPort = m_fromPort;
        conn.toPort = m_toPort;
        conn.edgeType = m_edgeType;
        proj->addConnection(conn);
    }

private:
    QString m_fromId, m_toId, m_fromPort, m_toPort, m_edgeType;
};

class AgentDisconnectCmd : public QUndoCommand {
public:
    AgentDisconnectCmd(const ModuleConnection& connection, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_connection(connection) {
        setText(QString("Disconnect %1 → %2").arg(connection.fromModuleId, connection.toModuleId));
    }

    void undo() override {
        // undo of disconnect = reconnect
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        proj->addConnection(m_connection);
    }
    void redo() override {
        // redo of disconnect = disconnect again
        Project* proj = ProjectManager::instance().currentProject();
        if (proj)
            proj->removeConnectionWithPorts(m_connection.fromModuleId, m_connection.fromPort, m_connection.toModuleId,
                                            m_connection.toPort);
    }

private:
    ModuleConnection m_connection;
};

// --- AgentActor ---

AgentActor::AgentActor(QObject* parent) : QObject(parent), m_undoStack(new QUndoStack(this)) {
    ensureAgentResources();
    // 阶7 Agent 评审修复（P2-7）：schema 成员缓存在插件加载/卸载时失效
    connect(&PluginManager::instance(), &PluginManager::pluginLoaded, this,
            [this](const QString&) { m_schemaCache.clear(); });
    connect(&PluginManager::instance(), &PluginManager::pluginUnloaded, this,
            [this](const QString&) { m_schemaCache.clear(); });
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, m_undoStack, &QUndoStack::clear);
}

AgentActor::~AgentActor() = default;

QJsonObject AgentActor::executeTool(const QString& toolName, const QJsonObject& params) {
    ToolSchema& schema = ToolSchema::instance();
    if (!schema.hasTool(toolName)) {
        QString err = QString("Unknown tool: %1").arg(toolName);
        emit toolError(toolName, err);
        return QJsonObject{{"error", err}};
    }

    QString invalid = validationError(toolName, params);
    if (!invalid.isEmpty()) {
        emit toolError(toolName, invalid);
        return QJsonObject{{"error", invalid}};
    }

    const bool mutatesFlow = toolName == "create_project" || toolName == "open_project" || toolName == "add_module" ||
                             toolName == "remove_module" || toolName == "set_param" || toolName == "connect_modules" ||
                             toolName == "disconnect_modules";
    if (mutatesFlow && RunEngine::instance().isBusy()) {
        const QString error = QStringLiteral("Flow is running; stop it before changing the project");
        emit toolError(toolName, error);
        return QJsonObject{{"error", error}};
    }

    QJsonObject result;
    if (toolName == "create_project")
        result = createProject(params);
    else if (toolName == "add_module")
        result = addModule(params);
    else if (toolName == "remove_module")
        result = removeModule(params);
    else if (toolName == "set_param")
        result = setParam(params);
    else if (toolName == "connect_modules")
        result = connectModules(params);
    else if (toolName == "disconnect_modules")
        result = disconnectModules(params);
    else if (toolName == "run_flow")
        result = runFlow(params);
    else if (toolName == "stop_flow")
        result = stopFlow(params);
    else if (toolName == "get_flow_state")
        result = getFlowState(params);
    else if (toolName == "get_available_plugins")
        result = getAvailablePlugins(params);
    else if (toolName == "save_project")
        result = saveProject(params);
    else if (toolName == "get_module_params_schema")
        result = getModuleParamsSchema(params);
    else if (toolName == "get_run_results")
        result = getRunResults(params);
    else if (toolName == "open_project")
        result = openProject(params);
    else if (toolName == "pause_flow")
        result = pauseFlow(params);
    else if (toolName == "resume_flow")
        result = resumeFlow(params);
    else if (toolName == "read_documentation")
        result = readDocumentation(params);
    else if (toolName == "sam_predict")
        result = samPredict(params);
    else if (toolName == "sam_annotate")
        result = samAnnotate(params);
    else {
        QString err = QString("Tool not yet implemented: %1").arg(toolName);
        emit toolError(toolName, err);
        return QJsonObject{{"error", err}};
    }

    emit toolExecuted(toolName, result);
    return result;
}

QJsonObject AgentActor::executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName) {
    if (tools.isEmpty())
        return QJsonObject{{"error", "No tools to execute"}};

    for (const auto& pair : tools) {
        QString invalid = validationError(pair.first, pair.second);
        if (!invalid.isEmpty()) {
            return QJsonObject{
                {"status", "partial_failed"}, {"executed", 0}, {"failedTool", pair.first}, {"error", invalid}};
        }
    }

    QJsonArray results;
    int executed = 0;
    bool failed = false;
    QString failedTool;
    QString error;
    bool macroOpen = false;
    for (const auto& pair : tools) {
        const bool switchesProject =
            pair.first == QStringLiteral("create_project") || pair.first == QStringLiteral("open_project");
        if (switchesProject && macroOpen) {
            m_undoStack->endMacro();
            macroOpen = false;
        } else if (!switchesProject && !macroOpen) {
            m_undoStack->beginMacro(macroName);
            macroOpen = true;
        }
        QJsonObject result = executeTool(pair.first, pair.second);
        QJsonObject entry;
        entry["tool"] = pair.first;
        entry["result"] = result;
        results.append(entry);
        if (result.contains("error")) {
            failed = true;
            failedTool = pair.first;
            error = result["error"].toString();
            break;
        }
        ++executed;
    }
    if (macroOpen)
        m_undoStack->endMacro();

    if (failed) {
        return QJsonObject{{"status", "partial_failed"},
                           {"results", results},
                           {"executed", executed},
                           {"failedTool", failedTool},
                           {"error", error}};
    }
    return QJsonObject{{"status", "completed"}, {"results", results}, {"executed", executed}};
}

// --- Tool Handlers ---

QJsonObject AgentActor::createProject(const QJsonObject& params) {
    QString name = params.value("name").toString("unnamed");
    Project* project = ProjectManager::instance().newProject();
    if (!project)
        return QJsonObject{{"error", "Failed to create project"}};
    project->setName(name);
    return QJsonObject{{"status", "created"}, {"name", name}};
}

QJsonObject AgentActor::addModule(const QJsonObject& params) {
    QString plugin = params.value("plugin").toString();
    QString instanceName = params.value("instanceName").toString().trimmed();
    if (plugin.isEmpty())
        return QJsonObject{{"error", "Missing 'plugin' parameter"}};
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return QJsonObject{{"error", "No project opened"}};

    PluginManager& manager = PluginManager::instance();
    const PluginInfo info = manager.pluginInfo(plugin);
    if (info.name.isEmpty())
        return QJsonObject{{"error", QString("Plugin not found: %1").arg(plugin)}};
    std::unique_ptr<IModule> module(manager.createModule(info.name));
    if (!module)
        return QJsonObject{{"error", QString("Plugin is not loaded: %1").arg(plugin)}};

    const QString instanceId = instanceName.isEmpty()
                                   ? QString("%1_%2").arg(info.name, QUuid::createUuid().toString(QUuid::WithoutBraces))
                                   : instanceName;
    if (project->moduleById(instanceId))
        return QJsonObject{{"error", QString("Module instance already exists: %1").arg(instanceId)}};

    const QString displayName = instanceName.isEmpty() ? module->name() : instanceName;
    m_undoStack->push(new AgentAddModuleCmd(info.name, instanceId, displayName));
    return QJsonObject{{"status", "added"}, {"plugin", info.name}, {"instanceId", instanceId}};
}

QJsonObject AgentActor::removeModule(const QJsonObject& params) {
    QString instanceId = params.value("instanceId").toString();
    if (instanceId.isEmpty())
        return QJsonObject{{"error", "Missing 'instanceId' parameter"}};
    m_undoStack->push(new AgentRemoveModuleCmd(instanceId));
    return QJsonObject{{"status", "removed"}, {"instanceId", instanceId}};
}

QJsonObject AgentActor::setParam(const QJsonObject& params) {
    QString instanceId = params.value("instanceId").toString();
    QString key = params.value("key").toString();
    QJsonValue value = params.value("value");

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return QJsonObject{{"error", "No project opened"}};

    const std::optional<ModuleInstance> inst = proj->moduleById(instanceId);
    if (!inst)
        return QJsonObject{{"error", QString("Module not found: %1").arg(instanceId)}};

    // 阶7 Agent 评审修复（P1-1）：schema 声明 value 为字符串且承诺转换——在此按
    // 目标类型（既有值或插件默认值类型）转换字符串，并经插件 validateParams
    // 校验通过后才写入 undo 栈；工具层不再对非法值假成功
    QJsonValue newVal = value;
    PluginManager& pm = PluginManager::instance();
    IModule* mod = pm.createModule(inst->moduleId);
    if (!mod) {
        return QJsonObject{{"error", QString("Cannot create module for validation: %1").arg(inst->moduleId)}};
    }
    const QJsonObject defaults = mod->defaultParams();
    const QJsonObject uiParams = pm.pluginInfo(inst->moduleId).ui.value(QStringLiteral("parameters")).toObject();
    if (!defaults.contains(key) && !uiParams.contains(key)) {
        delete mod;
        return QJsonObject{{"error", QString("Unknown parameter '%1' for module %2").arg(key, inst->moduleId)}};
    }
    if (newVal.isString()) {
        const QString s = newVal.toString().trimmed();
        const QJsonValue ref = inst->params.contains(key) ? inst->params.value(key) : defaults.value(key);
        if (ref.isDouble()) {
            bool ok = false;
            const double d = s.toDouble(&ok);
            if (!ok) {
                delete mod;
                return QJsonObject{{"error", QString("Value '%1' is not a number for key %2").arg(s, key)}};
            }
            newVal = d;
        } else if (ref.isBool()) {
            if (s.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
                newVal = true;
            else if (s.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0)
                newVal = false;
            else {
                delete mod;
                return QJsonObject{{"error", QString("Value '%1' is not a boolean for key %2").arg(s, key)}};
            }
        }
    }
    // 镜像运行期语义：模块运行期参数 = 插件默认值 + 工程参数覆盖 + 本次新值
    QJsonObject merged = defaults;
    for (auto it = inst->params.constBegin(); it != inst->params.constEnd(); ++it)
        merged.insert(it.key(), it.value());
    merged.insert(key, newVal);
    QString verr;
    if (!mod->validateParams(merged, verr)) {
        delete mod;
        emit toolError("set_param", verr);
        return QJsonObject{{"error", verr}};
    }
    const bool hadOldValue = inst->params.contains(key);
    const QJsonValue oldVal = inst->params.value(key);
    delete mod;

    m_undoStack->push(new AgentSetParamCmd(instanceId, key, hadOldValue, oldVal, newVal));

    return QJsonObject{{"status", "param_set"}, {"instanceId", instanceId}, {"key", key}};
}

QJsonObject AgentActor::connectModules(const QJsonObject& params) {
    QString fromId = params.value("fromId").toString();
    QString toId = params.value("toId").toString();

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return QJsonObject{{"error", "No project opened"}};
    const std::optional<ModuleInstance> fromModule = proj->moduleById(fromId);
    const std::optional<ModuleInstance> toModule = proj->moduleById(toId);
    if (!fromModule)
        return QJsonObject{{"error", QString("Module not found: %1").arg(fromId)}};
    if (!toModule)
        return QJsonObject{{"error", QString("Module not found: %1").arg(toId)}};

    PluginManager& manager = PluginManager::instance();
    const PluginInfo fromInfo = manager.pluginInfo(fromModule->moduleId);
    const PluginInfo toInfo = manager.pluginInfo(toModule->moduleId);
    if (fromInfo.name.isEmpty() || toInfo.name.isEmpty())
        return QJsonObject{{"error", "Cannot resolve plugin metadata for connection"}};

    QString fromPort = params.value("fromPort").toString().trimmed();
    if (fromPort.isEmpty())
        fromPort = params.value("port").toString().trimmed();
    if (fromPort.isEmpty())
        fromPort = QStringLiteral("image");
    QString toPort = params.value("toPort").toString().trimmed();
    if (toPort.isEmpty())
        toPort = findPort(toInfo.inputPorts, fromPort) ? fromPort : QStringLiteral("image");

    const PortSpec* output = findPort(fromInfo.outputPorts, fromPort);
    const PortSpec* input = findPort(toInfo.inputPorts, toPort);
    if (!output)
        return QJsonObject{{"error", QString("Output port not found: %1.%2").arg(fromId, fromPort)}};
    if (!input)
        return QJsonObject{{"error", QString("Input port not found: %1.%2").arg(toId, toPort)}};
    if (!portTypesCompatible(*output, *input)) {
        return QJsonObject{
            {"error", QString("Incompatible ports: %1.%2 -> %3.%4").arg(fromId, fromPort, toId, toPort)}};
    }

    for (const ModuleConnection& connection : proj->connections()) {
        if (connection.fromModuleId == fromId && connection.fromPort == fromPort && connection.toModuleId == toId &&
            connection.toPort == toPort) {
            return QJsonObject{{"status", "already_connected"},
                               {"from", fromId},
                               {"to", toId},
                               {"fromPort", fromPort},
                               {"toPort", toPort}};
        }
        if (!input->multiple && connection.toModuleId == toId && connection.toPort == toPort)
            return QJsonObject{{"error", QString("Input port already connected: %1.%2").arg(toId, toPort)}};
    }

    const QString edgeType = output->control ? QStringLiteral("control") : QStringLiteral("data");
    m_undoStack->push(new AgentConnectCmd(fromId, fromPort, toId, toPort, edgeType));

    return QJsonObject{
        {"status", "connected"}, {"from", fromId}, {"to", toId}, {"fromPort", fromPort}, {"toPort", toPort}};
}

QJsonObject AgentActor::disconnectModules(const QJsonObject& params) {
    QString fromId = params.value("fromId").toString();
    QString toId = params.value("toId").toString();
    const QString fromPort = params.value("fromPort").toString().trimmed();
    const QString toPort = params.value("toPort").toString().trimmed();

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return QJsonObject{{"error", "No project opened"}};

    QList<ModuleConnection> matches;
    for (const ModuleConnection& connection : proj->connections()) {
        if (connection.fromModuleId == fromId && connection.toModuleId == toId &&
            (fromPort.isEmpty() || connection.fromPort == fromPort) &&
            (toPort.isEmpty() || connection.toPort == toPort)) {
            matches.append(connection);
        }
    }
    if (matches.isEmpty())
        return QJsonObject{{"error", "Connection not found"}};
    if (matches.size() > 1)
        return QJsonObject{{"error", "Multiple connections match; specify fromPort and toPort"}};

    const ModuleConnection connection = matches.first();
    m_undoStack->push(new AgentDisconnectCmd(connection));

    return QJsonObject{{"status", "disconnected"},
                       {"from", fromId},
                       {"to", toId},
                       {"fromPort", connection.fromPort},
                       {"toPort", connection.toPort}};
}

QJsonObject AgentActor::runFlow(const QJsonObject& params) {
    QString mode = params.value("mode").toString("once");
    RunEngine& engine = RunEngine::instance();
    Project* project = ProjectManager::instance().currentProject();
    if (project && !engine.isRunning()) {
        QString loadError;
        QMetaObject::Connection errorConn = QObject::connect(&engine, &RunEngine::errorOccurred, &engine,
                                                             [&loadError](const QString& error) { loadError = error; });
        bool loaded = engine.loadProject(project);
        QObject::disconnect(errorConn);
        if (!loaded) {
            return QJsonObject{{"error", loadError.isEmpty() ? QString("Failed to load project") : loadError}};
        }
    }
    if (engine.modules().isEmpty()) {
        return QJsonObject{{"error", "No modules to run"}};
    }
    if (mode == "cycle") {
        engine.start();
    } else {
        if (!engine.runOnceAsync())
            return QJsonObject{{"error", "Flow is already running"}};
    }
    return QJsonObject{{"status", "started"}, {"mode", mode}, {"async", true}};
}

QJsonObject AgentActor::stopFlow(const QJsonObject& params) {
    Q_UNUSED(params);
    RunEngine::instance().stop();
    return QJsonObject{{"status", "stopped"}};
}

QJsonObject AgentActor::getFlowState(const QJsonObject& params) {
    Q_UNUSED(params);
    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return QJsonObject{{"error", "No project opened"}};

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

QJsonObject AgentActor::getAvailablePlugins(const QJsonObject& params) {
    Q_UNUSED(params);
    QStringList modules = PluginManager::instance().availableModules();
    QJsonArray arr;
    for (const QString& m : modules)
        arr.append(m);
    return QJsonObject{{"plugins", arr}};
}

QJsonObject AgentActor::saveProject(const QJsonObject& params) {
    QString path = params.value("path").toString();
    bool ok = ProjectManager::instance().saveProject(path);
    return QJsonObject{{"status", ok ? "saved" : "failed"}};
}

QJsonObject AgentActor::getModuleParamsSchema(const QJsonObject& params) {
    QString instanceId = params.value("instanceId").toString();
    if (instanceId.isEmpty()) {
        return QJsonObject{{"error", "Missing 'instanceId' parameter"}};
    }

    PluginManager& pm = PluginManager::instance();
    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return QJsonObject{{"error", "No project opened"}};

    const std::optional<ModuleInstance> inst = proj->moduleById(instanceId);
    if (!inst)
        return QJsonObject{{"error", QString("Module not found: %1").arg(instanceId)}};

    // 成员缓存：相同 moduleId 的 schema 只生成一次，避免重复 createModule 加载动态库；
    // 阶7 Agent 评审修复（P2-7）：由 static 改为成员并在插件加载/卸载时失效，
    // 避免插件热更或 metadata 变更后返回陈旧 schema
    QJsonObject schema = m_schemaCache.value(inst->moduleId);
    if (schema.isEmpty()) {
        std::unique_ptr<IModule> mod(pm.createModule(inst->moduleId));
        if (!mod)
            return QJsonObject{{"error", QString("Cannot create module: %1").arg(inst->moduleId)}};
        schema["moduleId"] = inst->moduleId;
        schema["name"] = mod->name();
        schema["description"] = mod->description();
        schema["defaultParams"] = mod->defaultParams();
        m_schemaCache[inst->moduleId] = schema;
    }
    schema["currentParams"] = inst->params;
    return schema;
}

QJsonObject AgentActor::getRunResults(const QJsonObject& params) {
    Q_UNUSED(params);
    RunEngine& engine = RunEngine::instance();
    return QJsonObject{{"totalRuns", engine.totalRuns()},
                       {"successRuns", engine.successRuns()},
                       {"failedRuns", engine.failedRuns()},
                       {"lastElapsedMs", engine.lastElapsedMs()},
                       {"isRunning", engine.isRunning()}};
}

QJsonObject AgentActor::openProject(const QJsonObject& params) {
    QString path = params.value("path").toString();
    if (path.isEmpty())
        return QJsonObject{{"error", "Missing 'path' parameter"}};

    Project* opened = ProjectManager::instance().openProject(path);
    if (!opened)
        return QJsonObject{{"error", "Failed to open project"}};

    Project* proj = ProjectManager::instance().currentProject();
    return QJsonObject{{"status", "opened"}, {"name", proj ? proj->name() : "unknown"}};
}

QJsonObject AgentActor::pauseFlow(const QJsonObject& params) {
    Q_UNUSED(params);
    RunEngine::instance().pause();
    return QJsonObject{{"status", "paused"}};
}

QJsonObject AgentActor::resumeFlow(const QJsonObject& params) {
    Q_UNUSED(params);
    RunEngine::instance().resume();
    return QJsonObject{{"status", "resumed"}};
}

QJsonObject AgentActor::readDocumentation(const QJsonObject& params) {
    QString topic = params.value("topic").toString().toLower();
    if (topic.isEmpty()) {
        return QJsonObject{{"error", "Missing 'topic' parameter"}};
    }

    QStringList knowledgePaths{
        ":/agent/knowledge/DEEPLUX.md",
        QDir::currentPath() + "/src/core/agent/knowledge/DEEPLUX.md",
        QCoreApplication::applicationDirPath() + "/../src/core/agent/knowledge/DEEPLUX.md",
    };

    QFile file;
    for (const QString& path : knowledgePaths) {
        file.setFileName(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            break;
        }
    }

    if (!file.isOpen()) {
        // 内嵌摘要作为 fallback
        return QJsonObject{{"topic", topic},
                           {"content", "Knowledge base not found. Use get_module_params_schema(instanceId) "
                                       "to discover parameters for specific modules."}};
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
        return QJsonObject{{"topic", topic},
                           {"content", QString("No documentation found for '%1'. "
                                               "Try broader topics like 'workflow', 'modules', 'params', "
                                               "or a module name like 'FindCircle'.")
                                           .arg(topic)}};
    }

    return QJsonObject{{"topic", topic}, {"sections", relevant.size()}, {"content", relevant.join("\n\n")}};
}

// --- SAM 批3：Agent 工具化 SAM（共享 SamPredictor 同步预测核心）---

QJsonObject AgentActor::samPredict(const QJsonObject& params) {
    const QString serverUrl = params.value("serverUrl").toString(QStringLiteral("http://127.0.0.1:8990"));
    const int timeoutMs = params.value("timeoutMs").toInt(30000);

    SamPromptItem item;
    item.label = params.value("label").toString(QStringLiteral("object"));
    QString perr;
    if (!parseSamPointsJson(params.value("points"), item.positivePoints, true, perr))
        return QJsonObject{{"error", QString("points 非法: %1").arg(perr)}};
    if (!parseSamPointsJson(params.value("negativePoints"), item.negativePoints, true, perr))
        return QJsonObject{{"error", QString("negativePoints 非法: %1").arg(perr)}};
    if (!parseSamBoxJson(params.value("box"), item.box, perr))
        return QJsonObject{{"error", QString("box 非法: %1").arg(perr)}};
    if (item.positivePoints.isEmpty() && !item.box.isValid())
        return QJsonObject{{"error", QString("points 与 box 至少提供一个 prompt")}};
    // 批4（C）：候选选择（0=最佳；上界越界由 SamPredictor 按实际候选数失败关闭）
    const QJsonValue candIdx = params.value("candidateIndex");
    if (!candIdx.isUndefined() && !candIdx.isNull()) {
        if (!candIdx.isDouble() || candIdx.toDouble() < 0 || candIdx.toDouble() > 7 ||
            candIdx.toDouble() != double(qRound(candIdx.toDouble())))
            return QJsonObject{{"error", QString("candidateIndex 必须为 [0,7] 整数")}};
        item.candidateIndex = qRound(candIdx.toDouble());
    }

    // imagePath 存在性/serverUrl/timeoutMs 由 runSamPredictBatch 先行校验（不发网络请求）
    const SamPredictOutcome outcome =
        runSamPredictBatch(serverUrl, params.value("imagePath").toString(), {item}, timeoutMs);
    if (!outcome.ok)
        return QJsonObject{{"error", outcome.error}};

    const Region2D& region = outcome.regions.first();
    QJsonArray polygon;
    for (const QPointF& p : region.polygon)
        polygon.append(QJsonArray{p.x(), p.y()});
    // 批4（C）：候选摘要回传（score 列表，降序）——LLM 可据此改选 candidateIndex
    QJsonArray candidateScores;
    for (const double s : outcome.candidateScores.value(0))
        candidateScores.append(s);
    // 不回传 maskRle 本体（可达 MB 级，会撑爆 LLM 上下文），仅给出长度
    return QJsonObject{
        {"status", QStringLiteral("predicted")},
        {"label", region.label},
        {"score", region.score},
        {"bbox", QJsonArray{region.bbox.x(), region.bbox.y(), region.bbox.width(), region.bbox.height()}},
        {"polygon", polygon},
        {"polygonPoints", region.polygon.size()},
        {"maskRleLength", region.maskRle.size()},
        {"modelName", outcome.modelName},
        {"candidateCount", candidateScores.size()},
        {"candidateScores", candidateScores},
        {"candidateIndex", item.candidateIndex}};
}

QJsonObject AgentActor::samAnnotate(const QJsonObject& params) {
    const QString imagePath = params.value("imagePath").toString();
    const QString sessionPath = params.value("sessionPath").toString();
    const QString serverUrl = params.value("serverUrl").toString(QStringLiteral("http://127.0.0.1:8990"));
    const int timeoutMs = params.value("timeoutMs").toInt(30000);

    if (sessionPath.trimmed().isEmpty())
        return QJsonObject{{"error", QString("sessionPath 不能为空")}};
    // 批4（B）：prompt 来源二选一——items 或 labelMePath（互斥且必选其一）
    const QString labelMePath = params.value("labelMePath").toString().trimmed();
    const QJsonValue itemsValue = params.value("items");
    const bool hasItems = itemsValue.isArray();
    if (hasItems == !labelMePath.isEmpty())
        return QJsonObject{{"error", QString("items 与 labelMePath 必须恰好提供一个（互斥）")}};

    QList<SamPromptItem> prompts;
    int importedRectangles = 0;
    int skippedShapes = 0;
    if (!labelMePath.isEmpty()) {
        // LabelMe 矩形框 → box prompt 批量：严格导入失败关闭；非矩形/退化
        // 形状跳过并计数，结果中显式报告（不静默）
        QList<LabelMeShape> shapes;
        QString importError;
        if (!LabelMeImporter::loadShapes(labelMePath, shapes, &importError))
            return QJsonObject{{"error", QString("LabelMe 导入失败：%1").arg(importError)}};
        const QList<LabelMeBox> boxes = LabelMeImporter::rectangleBoxes(shapes, &skippedShapes);
        if (boxes.isEmpty())
            return QJsonObject{
                {"error", QString("LabelMe 无可用矩形框（跳过非矩形/退化形状 %1 个）").arg(skippedShapes)}};
        if (boxes.size() > 16)
            return QJsonObject{{"error", QString("LabelMe 矩形数 %1 超出 [1,16]").arg(boxes.size())}};
        for (const LabelMeBox& labelMeBox : boxes) {
            SamPromptItem item;
            item.label = labelMeBox.label;
            item.box = labelMeBox.box;
            prompts.append(item);
        }
        importedRectangles = boxes.size();
    } else {
        const QJsonArray items = itemsValue.toArray();
        if (items.isEmpty() || items.size() > 16)
            return QJsonObject{{"error", QString("items 数量必须在 [1,16]")}};
        for (int i = 0; i < items.size(); ++i) {
            if (!items[i].isObject())
                return QJsonObject{{"error", QString("items[%1] 必须为对象").arg(i)}};
            const QJsonObject itemObj = items[i].toObject();
            SamPromptItem item;
            item.label = itemObj.value("label").toString(QStringLiteral("object"));
            QString perr;
            if (!parseSamPointsJson(itemObj.value("points"), item.positivePoints, true, perr))
                return QJsonObject{{"error", QString("items[%1].points 非法: %2").arg(i).arg(perr)}};
            if (!parseSamPointsJson(itemObj.value("negativePoints"), item.negativePoints, true, perr))
                return QJsonObject{{"error", QString("items[%1].negativePoints 非法: %2").arg(i).arg(perr)}};
            if (!parseSamBoxJson(itemObj.value("box"), item.box, perr))
                return QJsonObject{{"error", QString("items[%1].box 非法: %2").arg(i).arg(perr)}};
            if (item.positivePoints.isEmpty() && !item.box.isValid())
                return QJsonObject{{"error", QString("items[%1]：points 与 box 至少提供一个 prompt").arg(i)}};
            // 批4（C）：每项可选 candidateIndex（0=最佳；越界由 SamPredictor 失败关闭）
            const QJsonValue candIdx = itemObj.value("candidateIndex");
            if (!candIdx.isUndefined() && !candIdx.isNull()) {
                if (!candIdx.isDouble() || candIdx.toDouble() < 0 || candIdx.toDouble() > 7 ||
                    candIdx.toDouble() != double(qRound(candIdx.toDouble())))
                    return QJsonObject{{"error", QString("items[%1].candidateIndex 必须为 [0,7] 整数").arg(i)}};
                item.candidateIndex = qRound(candIdx.toDouble());
            }
            prompts.append(item);
        }
    }

    // 图像必须可读：会话需要记录真实宽高（不伪造尺寸）
    const QImage image(imagePath);
    if (imagePath.isEmpty() || image.isNull())
        return QJsonObject{{"error", QString("无法读取图像：%1").arg(imagePath)}};

    // 既有会话：追加语义；加载失败、绑定异图、同路径尺寸不符或内容摘要不符
    // 均失败关闭（不覆盖用户数据，不把旧坐标系标注追加进新图）
    AnnotationSession session;
    const QString digest = AnnotationSession::computeImageDigest(image);
    if (QFileInfo::exists(sessionPath)) {
        QString loadError;
        session = AnnotationSession::load(sessionPath, &loadError);
        if (!loadError.isEmpty())
            return QJsonObject{{"error", QString("标注会话加载失败：%1").arg(loadError)}};
        if (!session.imagePath.isEmpty() && session.imagePath != imagePath)
            return QJsonObject{{"error", QString("会话已绑定其他图像（%1），拒绝混图追加").arg(session.imagePath)}};
        // 批3 复核二轮（P1-4）：同路径 ≠ 同图——文件被替换后旧标注属于旧坐标系，
        // 尺寸与像素内容摘要（AnnotationSession::computeImageDigest）双重校验
        if (session.imageWidth > 0 && session.imageHeight > 0 &&
            (session.imageWidth != image.width() || session.imageHeight != image.height()))
            return QJsonObject{{"error", QString("同路径图像尺寸与既有会话不一致（旧 %1x%2 vs 新 %3x%4），拒绝追加")
                                             .arg(session.imageWidth)
                                             .arg(session.imageHeight)
                                             .arg(image.width())
                                             .arg(image.height())}};
        // 批3 复核三轮（P1-1）：摘要策略收紧——有摘要必须一致；无摘要且已含
        // 标注 = 无法证明图像未被替换，拒绝追加（不静默把旧坐标系标注重绑到
        // 新图）；仅空会话允许补记摘要
        if (!session.imageDigest.isEmpty()) {
            if (session.imageDigest != digest)
                return QJsonObject{
                    {"error", QString("同路径图像内容已变化（摘要不符），拒绝把旧坐标系标注追加进新图")}};
        } else if (!session.annotations.isEmpty()) {
            return QJsonObject{{"error", QString("既有会话缺少图像摘要且已含标注，无法校验图像未被替换，拒绝追加")}};
        }
    }
    session.imageDigest = digest; // 仅三种路径到达：新文件 / 摘要一致 / 空会话补记

    // 全有或全无：任一 prompt 预测失败则不写文件
    const SamPredictOutcome outcome = runSamPredictBatch(serverUrl, imagePath, prompts, timeoutMs);
    if (!outcome.ok) {
        QJsonObject result{{"error", outcome.error}};
        if (outcome.failedIndex >= 0)
            result.insert(QStringLiteral("failedItem"), outcome.failedIndex);
        return result;
    }

    const QString modelName = outcome.modelName.isEmpty() ? QStringLiteral("sam") : outcome.modelName;
    QJsonArray objectIds;
    for (int i = 0; i < outcome.regions.size(); ++i) {
        const Region2D& region = outcome.regions[i];
        AnnotationObject obj;
        obj.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        obj.label = region.label;
        obj.bbox = region.bbox;
        obj.polygon = region.polygon;
        obj.maskRle = region.maskRle;
        obj.score = region.score;
        obj.prompts.pointsPos = prompts[i].positivePoints;
        obj.prompts.pointsNeg = prompts[i].negativePoints;
        if (prompts[i].box.isValid())
            obj.prompts.box = prompts[i].box;
        obj.modelName = modelName;
        session.annotations.append(obj);
        objectIds.append(obj.id);
    }
    session.imagePath = imagePath;
    session.imageWidth = image.width();
    session.imageHeight = image.height();
    if (session.modelName.isEmpty())
        session.modelName = modelName;

    QString saveError;
    if (!session.save(sessionPath, &saveError))
        return QJsonObject{{"error", QString("标注会话保存失败：%1").arg(saveError)}};

    QJsonObject result{{"status", QStringLiteral("annotated")},
                       {"added", objectIds.size()},
                       {"total", session.annotations.size()},
                       {"sessionPath", sessionPath},
                       {"objectIds", objectIds}};
    if (importedRectangles > 0) {
        // 批4（B）：LabelMe 导入统计显式回传（跳过数不为 0 时一并报告，不静默）
        result.insert(QStringLiteral("importedRectangles"), importedRectangles);
        result.insert(QStringLiteral("skippedShapes"), skippedShapes);
    }
    return result;
}

} // namespace DeepLux
