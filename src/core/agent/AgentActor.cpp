#include "AgentActor.h"

#include "AgentRunWaiter.h"
#include "AgentToolImage.h"
#include "SamPredictor.h"
#include "ToolSchema.h"
#include "base/ModuleBase.h"
#include "common/Logger.h"
#include "engine/RunEngine.h"
#include "io/LabelMeImporter.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"
#include "model/Annotation.h"
#include "model/Project.h"
#include "platform/PathUtils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
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

QJsonObject errorObject(const QString& error, const QString& code) {
    QJsonObject result{{"error", error}};
    if (!code.isEmpty())
        result.insert(QStringLiteral("code"), code);
    return result;
}

/// tune_node_and_verify 的参数级失败（success:false + code:invalid_parameter）
QJsonObject invalidParameter(const QString& error, const QString& parameter) {
    QJsonObject result{{"success", false}, {"error", error}, {"code", QStringLiteral("invalid_parameter")}};
    if (!parameter.isEmpty())
        result.insert(QStringLiteral("parameter"), parameter);
    return result;
}

QString scalarToString(const QJsonValue& value) {
    if (value.isString())
        return value.toString();
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) {
        const double d = value.toDouble();
        if (d == static_cast<double>(qRound64(d)) && qAbs(d) < 1.0e15)
            return QString::number(qRound64(d));
        return QString::number(d);
    }
    if (value.isNull() || value.isUndefined())
        return QString();
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

/// 运行期语义的完整参数：插件默认值 + 工程参数覆盖
QJsonObject mergedParams(IModule* mod, const ModuleInstance& inst) {
    QJsonObject merged = mod->defaultParams();
    for (auto it = inst.params.constBegin(); it != inst.params.constEnd(); ++it)
        merged.insert(it.key(), it.value());
    return merged;
}

struct ParamNormalizeResult {
    bool ok = false;
    QJsonValue value;
    QString error;
};

/**
 * @brief set_param / tune_node_and_verify 共用的参数值归一化
 *
 * 键必须存在于插件默认参数或 ui.parameters；字符串按目标类型（既有值或默认值类型）
 * 转换；类型/范围校验由调用方对完整 merged 参数调用 validateParams 完成。
 */
ParamNormalizeResult normalizeParamValue(IModule* mod, const ModuleInstance& inst, const QString& key,
                                         const QJsonValue& raw) {
    ParamNormalizeResult result;
    const QJsonObject defaults = mod->defaultParams();
    const QJsonObject uiParams =
        PluginManager::instance().pluginInfo(inst.moduleId).ui.value(QStringLiteral("parameters")).toObject();
    if (!defaults.contains(key) && !uiParams.contains(key)) {
        result.error = QString("Unknown parameter '%1' for module %2").arg(key, inst.moduleId);
        return result;
    }
    QJsonValue newVal = raw;
    if (newVal.isString()) {
        const QString s = newVal.toString().trimmed();
        const QJsonValue ref = inst.params.contains(key) ? inst.params.value(key) : defaults.value(key);
        if (ref.isDouble()) {
            bool ok = false;
            const double d = s.toDouble(&ok);
            if (!ok) {
                result.error = QString("Value '%1' is not a number for key %2").arg(s, key);
                return result;
            }
            newVal = d;
        } else if (ref.isBool()) {
            if (s.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0) {
                newVal = true;
            } else if (s.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0) {
                newVal = false;
            } else {
                result.error = QString("Value '%1' is not a boolean for key %2").arg(s, key);
                return result;
            }
        }
    }
    result.ok = true;
    result.value = newVal;
    return result;
}

struct NodeRef {
    QString runtimeName; // 引擎侧运行名（§4.0 解析规则）
    QString instanceId;  // 工程实例 id
    QString nodeName;    // 工程显示名
    QString moduleId;
};

bool resolveProjectModule(const Project* project, const QString& nodeName, ModuleInstance* out) {
    if (!project || nodeName.trimmed().isEmpty())
        return false;
    for (const ModuleInstance& inst : project->modules()) {
        if (inst.name == nodeName) {
            *out = inst;
            return true;
        }
    }
    for (const ModuleInstance& inst : project->modules()) {
        if (inst.id == nodeName) {
            *out = inst;
            return true;
        }
    }
    return false;
}

/// 解析节点（名称优先、其次实例 id），并要求引擎已加载该模块
bool resolveNodeRef(const Project* project, const QString& nodeName, NodeRef* ref) {
    ModuleInstance inst;
    if (!resolveProjectModule(project, nodeName, &inst))
        return false;
    RunEngine& engine = RunEngine::instance();
    QString runtimeName;
    if (engine.getModule(inst.name))
        runtimeName = inst.name;
    else if (engine.getModule(inst.id))
        runtimeName = inst.id;
    else
        return false;
    ref->runtimeName = runtimeName;
    ref->instanceId = inst.id;
    ref->nodeName = inst.name;
    ref->moduleId = inst.moduleId;
    return true;
}

bool serializePortValue(const QVariant& value, QJsonValue* out) {
    const int typeId = value.userType();
    if (typeId == qMetaTypeId<ImageData>()) {
        const ImageData image = value.value<ImageData>();
        *out = QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                           {QStringLiteral("width"), image.width()},
                           {QStringLiteral("height"), image.height()},
                           {QStringLiteral("present"), image.isValid()}};
        return true;
    }
    switch (typeId) {
    case QMetaType::Bool:
        *out = value.toBool();
        return true;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        *out = static_cast<double>(value.toLongLong());
        return true;
    case QMetaType::Double:
    case QMetaType::Float:
        *out = value.toDouble();
        return true;
    case QMetaType::QString: {
        const QString s = value.toString();
        if (s.size() <= 512) {
            *out = s;
        } else {
            *out = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("value"), s.left(512)},
                               {QStringLiteral("truncated"), true}};
        }
        return true;
    }
    case QMetaType::QVariantList: {
        const QVariantList list = value.toList();
        QJsonObject obj{{QStringLiteral("type"), QStringLiteral("list")}, {QStringLiteral("count"), list.size()}};
        if (!list.isEmpty()) {
            QJsonValue first;
            if (serializePortValue(list.first(), &first))
                obj.insert(QStringLiteral("first"), first);
        }
        *out = obj;
        return true;
    }
    default:
        return false;
    }
}

struct PortOutputsPayload {
    QJsonObject outputs;
    QStringList unsupported;
};

PortOutputsPayload serializePortOutputs(const PortValueMap& ports) {
    PortOutputsPayload payload;
    for (auto it = ports.constBegin(); it != ports.constEnd(); ++it) {
        QJsonValue serialized;
        if (serializePortValue(it.value(), &serialized))
            payload.outputs.insert(it.key(), serialized);
        else
            payload.unsupported.append(it.key());
    }
    payload.unsupported.sort();
    return payload;
}

QString logLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return QStringLiteral("debug");
    case LogLevel::Info:
        return QStringLiteral("info");
    case LogLevel::Warning:
        return QStringLiteral("warning");
    case LogLevel::Error:
        return QStringLiteral("error");
    case LogLevel::Success:
        return QStringLiteral("success");
    }
    return QStringLiteral("info");
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

AgentActor::AgentActor(QObject* parent)
    : QObject(parent), m_undoStack(new QUndoStack(this)), m_runWaiter(std::make_unique<AgentRunWaiter>()) {
    ensureAgentResources();
    buildHandlers();
    // 阶7 Agent 评审修复（P2-7）：schema 成员缓存在插件加载/卸载时失效
    connect(&PluginManager::instance(), &PluginManager::pluginLoaded, this,
            [this](const QString&) { m_schemaCache.clear(); });
    connect(&PluginManager::instance(), &PluginManager::pluginUnloaded, this,
            [this](const QString&) { m_schemaCache.clear(); });
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, m_undoStack, &QUndoStack::clear);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this,
            [this]() { m_engineProjectDirty = true; });
}

AgentActor::~AgentActor() = default;

void AgentActor::buildHandlers() {
    m_handlers.clear();
    const auto add = [this](const QString& name, QJsonObject (AgentActor::*handler)(const QJsonObject&),
                            bool mutatesProject, bool undoable) {
        ToolHandler entry;
        entry.fn = [this, handler](const QJsonObject& params) { return (this->*handler)(params); };
        entry.mutatesProject = mutatesProject;
        entry.undoable = undoable;
        m_handlers.insert(name, entry);
    };

    add(QStringLiteral("create_project"), &AgentActor::createProject, true, false);
    add(QStringLiteral("add_module"), &AgentActor::addModule, true, true);
    add(QStringLiteral("remove_module"), &AgentActor::removeModule, true, true);
    add(QStringLiteral("set_param"), &AgentActor::setParam, true, true);
    add(QStringLiteral("connect_modules"), &AgentActor::connectModules, true, true);
    add(QStringLiteral("disconnect_modules"), &AgentActor::disconnectModules, true, true);
    add(QStringLiteral("run_flow"), &AgentActor::runFlow, false, false);
    add(QStringLiteral("stop_flow"), &AgentActor::stopFlow, false, false);
    add(QStringLiteral("get_flow_state"), &AgentActor::getFlowState, false, false);
    add(QStringLiteral("get_available_plugins"), &AgentActor::getAvailablePlugins, false, false);
    add(QStringLiteral("save_project"), &AgentActor::saveProject, false, false);
    add(QStringLiteral("get_module_params_schema"), &AgentActor::getModuleParamsSchema, false, false);
    add(QStringLiteral("get_run_results"), &AgentActor::getRunResults, false, false);
    add(QStringLiteral("open_project"), &AgentActor::openProject, true, false);
    add(QStringLiteral("pause_flow"), &AgentActor::pauseFlow, false, false);
    add(QStringLiteral("resume_flow"), &AgentActor::resumeFlow, false, false);
    add(QStringLiteral("read_documentation"), &AgentActor::readDocumentation, false, false);
    add(QStringLiteral("sam_predict"), &AgentActor::samPredict, false, false);
    add(QStringLiteral("sam_annotate"), &AgentActor::samAnnotate, false, false);
    // 第一迭代：闭环 / 观测工具
    add(QStringLiteral("validate_workflow"), &AgentActor::validateWorkflow, false, false);
    add(QStringLiteral("get_node_runtime_outputs"), &AgentActor::getNodeRuntimeOutputs, false, false);
    add(QStringLiteral("get_execution_logs"), &AgentActor::getExecutionLogs, false, false);
    add(QStringLiteral("tune_node_and_verify"), &AgentActor::tuneNodeAndVerify, true, true);
    add(QStringLiteral("capture_viewport_image"), &AgentActor::captureViewportImage, false, false);
    add(QStringLiteral("export_node_output"), &AgentActor::exportNodeOutput, false, false);
}

QStringList AgentActor::supportedTools() const {
    QStringList names = m_handlers.keys();
    names.sort();
    return names;
}

bool AgentActor::toolUndoable(const QString& toolName) const {
    const auto it = m_handlers.constFind(toolName);
    return it != m_handlers.constEnd() && it->undoable;
}

void AgentActor::setViewportImageProvider(ViewportImageProvider provider) {
    m_viewportImageProvider = std::move(provider);
}

void AgentActor::abortRunningTool() {
    if (m_runWaiter && m_runWaiter->isActive())
        m_runWaiter->abort();
    if (RunEngine::instance().isBusy())
        RunEngine::instance().stop();
}

QJsonObject AgentActor::executeTool(const QString& toolName, const QJsonObject& params) {
    QElapsedTimer timer;
    timer.start();
    QJsonObject result = executeToolInternal(toolName, params);
    // 内部字段（设计 §3.7）：消费方读取后必须立即剥离，不得进入模型上下文或 IPC 响应体。
    // toolExecuted 信号在 executeToolInternal 内发出，因此不含该字段。
    result.insert(QStringLiteral("_elapsedMs"), static_cast<int>(timer.elapsed()));
    return result;
}

QJsonObject AgentActor::executeToolInternal(const QString& toolName, const QJsonObject& params) {
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

    const auto it = m_handlers.constFind(toolName);
    if (it == m_handlers.constEnd()) {
        // schema 有、实现无的漂移必须显式失败，不得静默
        const QString error = QString("Tool not registered: %1").arg(toolName);
        emit toolError(toolName, error);
        return QJsonObject{{"error", error}, {"code", QStringLiteral("tool_not_registered")}};
    }

    if (it->mutatesProject && RunEngine::instance().isBusy()) {
        const QString error = QStringLiteral("Flow is running; stop it before changing the project");
        emit toolError(toolName, error);
        return QJsonObject{{"error", error}, {"code", QStringLiteral("engine_busy")}};
    }

    const QJsonObject result = it->fn(params);
    if (it->mutatesProject && resultSucceeded(result))
        m_engineProjectDirty = true;
    emit toolExecuted(toolName, result);
    return result;
}

bool AgentActor::resultSucceeded(const QJsonObject& result) {
    const QJsonValue success = result.value(QStringLiteral("success"));
    if (success.isBool())
        return success.toBool();
    return !result.contains(QStringLiteral("error"));
}

QString AgentActor::resultFailureCode(const QJsonObject& result) {
    if (resultSucceeded(result))
        return QString();
    const QString code = result.value(QStringLiteral("code")).toString();
    return code.isEmpty() ? QStringLiteral("tool_error") : code;
}

namespace {
/// 本批已有调用失败后为未执行调用补齐的结构化结果（不得记为执行成功）
QJsonObject skippedAfterFailure(const QString& failedTool, const QString& failedCode) {
    return QJsonObject{
        {QStringLiteral("success"), false},
        {QStringLiteral("error"), QStringLiteral("Skipped because %1 failed (%2)").arg(failedTool, failedCode)},
        {QStringLiteral("code"), QStringLiteral("skipped_after_failure")},
        {QStringLiteral("skipped"), true},
        {QStringLiteral("failedTool"), failedTool},
        {QStringLiteral("failedCode"), failedCode}};
}

QJsonObject blockedResult(const QString& tool, const QString& code) {
    return QJsonObject{{QStringLiteral("success"), false},
                       {QStringLiteral("error"), QStringLiteral("Tool was not executed: %1").arg(code)},
                       {QStringLiteral("code"), code},
                       {QStringLiteral("blocked"), true},
                       {QStringLiteral("tool"), tool}};
}

QJsonObject resultEntry(const QString& tool, const QJsonObject& result) {
    return QJsonObject{{QStringLiteral("tool"), tool}, {QStringLiteral("result"), result}};
}
} // namespace

QJsonObject AgentActor::executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName,
                                     const BatchGate& gate) {
    if (tools.isEmpty())
        return QJsonObject{{"error", "No tools to execute"}};

    // 参数预校验失败时本批零执行，但仍为每个调用生成结果，保证 tool_call 配对。
    int firstInvalid = -1;
    QString invalid;
    for (int i = 0; i < tools.size(); ++i) {
        invalid = validationError(tools.at(i).first, tools.at(i).second);
        if (!invalid.isEmpty()) {
            firstInvalid = i;
            break;
        }
    }
    if (firstInvalid >= 0) {
        const QString failedTool = tools.at(firstInvalid).first;
        QJsonArray results;
        for (int i = 0; i < tools.size(); ++i) {
            if (i == firstInvalid) {
                results.append(resultEntry(tools.at(i).first,
                                           QJsonObject{{QStringLiteral("success"), false},
                                                       {QStringLiteral("error"), invalid},
                                                       {QStringLiteral("code"), QStringLiteral("invalid_parameter")}}));
            } else {
                results.append(resultEntry(tools.at(i).first,
                                           skippedAfterFailure(failedTool, QStringLiteral("invalid_parameter"))));
            }
        }
        return QJsonObject{{"status", "partial_failed"},
                           {"results", results},
                           {"executed", 0},
                           {"failedTool", failedTool},
                           {"failedCode", QStringLiteral("invalid_parameter")},
                           {"error", invalid}};
    }

    QJsonArray results;
    int executed = 0;
    bool failed = false;
    QString failedTool;
    QString failedCode;
    QString error;
    bool macroOpen = false;
    for (int i = 0; i < tools.size(); ++i) {
        const auto& pair = tools.at(i);
        if (failed) {
            // 首次失败后不再执行，但必须补齐结果（跳过不算执行成功）
            results.append(resultEntry(pair.first, skippedAfterFailure(failedTool, failedCode)));
            continue;
        }

        // 执行前逐项门禁：中止/工程切换/权限降级后不得继续旧批次
        const QString gateCode = gate ? gate(i, pair.first, pair.second) : QString();
        if (!gateCode.isEmpty()) {
            results.append(resultEntry(pair.first, blockedResult(pair.first, gateCode)));
            failed = true;
            failedTool = pair.first;
            failedCode = gateCode;
            error = QStringLiteral("Tool execution blocked: %1").arg(gateCode);
            continue;
        }

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
        results.append(resultEntry(pair.first, result));
        if (!resultSucceeded(result)) {
            failed = true;
            failedTool = pair.first;
            failedCode = resultFailureCode(result);
            error = result.value(QStringLiteral("error")).toString();
            if (error.isEmpty())
                error = QStringLiteral("%1 failed (%2)").arg(failedTool, failedCode);
            continue;
        }
        ++executed;

        // create/open 成功后工程身份已变化：保守终止旧批次，让模型读取新工程再规划
        if (switchesProject && i + 1 < tools.size()) {
            failed = true;
            failedTool = pair.first;
            failedCode = QStringLiteral("project_switched");
            error = QStringLiteral("Project changed by %1; remaining tools were skipped").arg(pair.first);
        }
    }
    if (macroOpen)
        m_undoStack->endMacro();

    if (failed) {
        return QJsonObject{{"status", "partial_failed"}, {"results", results},       {"executed", executed},
                           {"failedTool", failedTool},   {"failedCode", failedCode}, {"error", error}};
    }
    return QJsonObject{{"status", "completed"}, {"results", results}, {"executed", executed}};
}

// --- Tool Handlers ---

QJsonObject AgentActor::createProject(const QJsonObject& params) {
    QString name = params.value("name").toString("unnamed");
    Project* project = ProjectManager::instance().newProject();
    if (!project) {
        // R2：保留既有 error 文案，仅追加机器可读 code 与诊断 message
        return QJsonObject{{"error", "Failed to create project"},
                           {"code", QStringLiteral("project_create_failed")},
                           {"message", QStringLiteral("ProjectManager::newProject returned null")}};
    }
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
    // R1：先校验实例存在——对不存在的实例不得压入撤销栈（否则会假成功，且审计
    // 会记录"可撤销但什么都没删"）。error 文案与既有 set_param 保持一致，仅追加 code。
    Project* project = ProjectManager::instance().currentProject();
    if (!project || !project->moduleById(instanceId)) {
        return QJsonObject{{"error", QString("Module not found: %1").arg(instanceId)},
                           {"code", QStringLiteral("node_not_found")}};
    }
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
    PluginManager& pm = PluginManager::instance();
    std::unique_ptr<IModule> mod(pm.createModule(inst->moduleId));
    if (!mod) {
        return QJsonObject{{"error", QString("Cannot create module for validation: %1").arg(inst->moduleId)}};
    }
    const ParamNormalizeResult normalized = normalizeParamValue(mod.get(), *inst, key, value);
    if (!normalized.ok)
        return QJsonObject{{"error", normalized.error}};

    // 镜像运行期语义：模块运行期参数 = 插件默认值 + 工程参数覆盖 + 本次新值
    QJsonObject merged = mergedParams(mod.get(), *inst);
    merged.insert(key, normalized.value);
    QString verr;
    if (!mod->validateParams(merged, verr)) {
        emit toolError("set_param", verr);
        return QJsonObject{{"error", verr}};
    }
    const bool hadOldValue = inst->params.contains(key);
    const QJsonValue oldVal = inst->params.value(key);

    m_undoStack->push(new AgentSetParamCmd(instanceId, key, hadOldValue, oldVal, normalized.value));

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
    const QString mode = params.value("mode").toString("once");
    const bool wait = params.value("wait").toBool(true);
    const int timeoutMs = qBound(1000, params.value("timeoutMs").toInt(60000), 300000);
    RunEngine& engine = RunEngine::instance();

    if (mode == QStringLiteral("cycle")) {
        // 连续运行没有"完成"概念：保持旧启动语义，wait 只追加说明
        Project* project = ProjectManager::instance().currentProject();
        if (project && !engine.isRunning()) {
            QString loadError;
            QMetaObject::Connection errorConn = QObject::connect(
                &engine, &RunEngine::errorOccurred, &engine, [&loadError](const QString& error) { loadError = error; });
            const bool loaded = engine.loadProject(project);
            QObject::disconnect(errorConn);
            if (!loaded) {
                return QJsonObject{{"error", loadError.isEmpty() ? QString("Failed to load project") : loadError},
                                   {"code", QStringLiteral("engine_load_failed")}};
            }
        }
        if (engine.modules().isEmpty())
            return QJsonObject{{"error", "No modules to run"}};
        engine.start();
        QJsonObject result{{"status", "started"}, {"mode", mode}, {"async", true}};
        if (wait)
            result.insert(QStringLiteral("note"), QStringLiteral("cycle mode ignores wait"));
        return result;
    }

    if (!wait) {
        // 旧语义逐字保留（提示词与既有测试依赖）
        Project* project = ProjectManager::instance().currentProject();
        if (project && !engine.isRunning()) {
            QString loadError;
            QMetaObject::Connection errorConn = QObject::connect(
                &engine, &RunEngine::errorOccurred, &engine, [&loadError](const QString& error) { loadError = error; });
            const bool loaded = engine.loadProject(project);
            QObject::disconnect(errorConn);
            if (!loaded) {
                return QJsonObject{{"error", loadError.isEmpty() ? QString("Failed to load project") : loadError}};
            }
        }
        if (engine.modules().isEmpty())
            return QJsonObject{{"error", "No modules to run"}};
        if (!engine.runOnceAsync())
            return QJsonObject{{"error", "Flow is already running"}, {"code", QStringLiteral("already_running")}};
        return QJsonObject{{"status", "started"}, {"mode", mode}, {"async", true}};
    }

    // wait=true：等待真实执行完成（嵌套事件循环，不阻塞 GUI）
    if (engine.isBusy())
        return QJsonObject{{"success", false},
                           {"code", QStringLiteral("already_running")},
                           {"error", QStringLiteral("Flow is already running")}};

    const RunWaitResult run = runAndWait(QStringLiteral("once"), timeoutMs, true);
    if (run.reentrant) {
        return QJsonObject{{"success", false},
                           {"code", QStringLiteral("reentrant_wait")},
                           {"error", QStringLiteral("Another run wait is already active on this thread")}};
    }
    if (run.rejectedStart) {
        return QJsonObject{{"success", false},
                           {"code", QStringLiteral("already_running")},
                           {"error", QStringLiteral("Flow is already running")}};
    }
    if (!run.started) {
        return QJsonObject{{"success", false},
                           {"mode", mode},
                           {"code", QStringLiteral("run_failed")},
                           {"failedModule", run.failedModule},
                           {"errorMessage", run.error},
                           {"error", run.error},
                           {"elapsedMs", run.elapsedMs},
                           {"waitTimedOut", false}};
    }
    if (run.timedOut) {
        return QJsonObject{
            {"success", false},
            {"mode", mode},
            {"code", QStringLiteral("timeout")},
            {"waitTimedOut", true},
            {"error", QStringLiteral("Flow did not finish within %1 ms; a stop was requested").arg(timeoutMs)}};
    }
    if (run.aborted) {
        return QJsonObject{{"success", false},
                           {"mode", mode},
                           {"code", QStringLiteral("cancelled")},
                           {"error", QStringLiteral("Run aborted by user")},
                           {"waitTimedOut", false}};
    }
    if (run.pausedAtBreakpoint) {
        return QJsonObject{{"success", false},
                           {"mode", mode},
                           {"code", QStringLiteral("paused_at_breakpoint")},
                           {"error", QStringLiteral("Flow paused at a breakpoint; continue or stop it from the UI")},
                           {"waitTimedOut", false}};
    }
    if (run.finished && run.ok) {
        return QJsonObject{{"success", true},
                           {"mode", mode},
                           {"elapsedMs", run.elapsedMs},
                           {"failedModule", QString()},
                           {"errorMessage", QString()},
                           {"waitTimedOut", false},
                           {"totalRuns", run.totalRuns},
                           {"successRuns", run.successRuns}};
    }
    return QJsonObject{{"success", false},
                       {"mode", mode},
                       {"code", QStringLiteral("run_failed")},
                       {"failedModule", run.failedModule},
                       {"errorMessage", run.error},
                       {"elapsedMs", run.elapsedMs},
                       {"waitTimedOut", false}};
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

QString AgentActor::userKnowledgeDir() {
    const QString dir = PathUtils::appDataPath() + QStringLiteral("/knowledge");
    PathUtils::ensureDirExists(dir);
    return dir;
}

QStringList AgentActor::knowledgeDocumentPaths() {
    ensureAgentResources();

    QStringList paths;

    // 内置知识库（编译进二进制的 qrc 资源，只读）
    const QDir builtin(QStringLiteral(":/agent/knowledge"));
    const QStringList builtinNames = builtin.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
    for (const QString& name : builtinNames) {
        paths.append(builtin.filePath(name));
    }
    if (paths.isEmpty()) {
        // 开发构建的资源异常兜底：尝试源码目录
        for (const QString& path :
             {QDir::currentPath() + "/src/core/agent/knowledge/DEEPLUX.md",
              QCoreApplication::applicationDirPath() + "/../src/core/agent/knowledge/DEEPLUX.md"}) {
            if (QFileInfo::exists(path)) {
                paths.append(path);
            }
        }
    }

    // 用户知识库目录（Agent 设置 → 知识库 添加的文档）
    const QDir userDir(userKnowledgeDir());
    const QStringList userNames = userDir.entryList(
        {QStringLiteral("*.md"), QStringLiteral("*.markdown"), QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
    for (const QString& name : userNames) {
        paths.append(userDir.filePath(name));
    }

    return paths;
}

QJsonObject AgentActor::readDocumentation(const QJsonObject& params) {
    QString topic = params.value("topic").toString().toLower();
    if (topic.isEmpty()) {
        return QJsonObject{{"error", "Missing 'topic' parameter"}};
    }

    const QStringList knowledgePaths = knowledgeDocumentPaths();
    if (knowledgePaths.isEmpty()) {
        // 内嵌摘要作为 fallback
        return QJsonObject{{"topic", topic},
                           {"content", "Knowledge base not found. Use get_module_params_schema(instanceId) "
                                       "to discover parameters for specific modules."}};
    }

    // 遍历全部知识库文档（内置 + 用户添加），按 ## 分节，匹配 topic 关键词
    QStringList relevant;
    for (const QString& path : knowledgePaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        const QString content = QString::fromUtf8(file.readAll());
        file.close();

        const QString docName = QFileInfo(path).fileName();
        const QStringList sections = content.split("\n## ");
        for (const QString& sec : sections) {
            if (sec.toLower().contains(topic) || topic == "all") {
                // 截断过长段落
                QString body =
                    sec.length() > 2000 ? sec.left(2000) + "\n... (truncated, use more specific topic)" : sec.trimmed();
                relevant.append(QStringLiteral("[来源: %1]\n%2").arg(docName, body));
            }
        }
    }

    if (relevant.isEmpty()) {
        return QJsonObject{{"topic", topic},
                           {"content", QString("No documentation found for '%1'. "
                                               "Try broader topics like 'workflow', 'modules', 'params', "
                                               "or a module name like 'FindCircle'. Users can add custom "
                                               "documents via Agent Settings → Knowledge Base.")
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

// --- 第一迭代：闭环 / 观测工具 ---

bool AgentActor::engineMatchesProject(const Project* project) const {
    if (!project)
        return false;
    RunEngine& engine = RunEngine::instance();
    const QList<ModuleInstance> modules = project->modules();
    const QList<ModuleBase*> engineModules = engine.modules();
    if (modules.size() != engineModules.size())
        return false;
    QSet<QString> expected;
    for (const ModuleInstance& inst : modules)
        expected.insert(inst.id);
    QSet<QString> actual;
    for (const ModuleBase* module : engineModules) {
        const QString name = module->instanceName().isEmpty() ? module->name() : module->instanceName();
        actual.insert(name);
    }
    return expected == actual;
}

bool AgentActor::ensureEngineLoaded(QString* error, bool forceReload) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project) {
        if (error)
            *error = QStringLiteral("No project opened");
        return false;
    }
    RunEngine& engine = RunEngine::instance();
    if (!forceReload && !m_engineProjectDirty && engineMatchesProject(project))
        return true;
    if (engine.isBusy()) {
        if (error)
            *error = QStringLiteral("Flow is running");
        return false;
    }
    QString loadError;
    QMetaObject::Connection errorConn = QObject::connect(&engine, &RunEngine::errorOccurred, &engine,
                                                         [&loadError](const QString& err) { loadError = err; });
    const bool loaded = engine.loadProject(project);
    QObject::disconnect(errorConn);
    if (!loaded) {
        if (error)
            *error = loadError.isEmpty() ? QStringLiteral("Failed to load project") : loadError;
        return false;
    }
    m_engineProjectDirty = false;
    return true;
}

AgentActor::RunWaitResult AgentActor::runAndWait(const QString& mode, int timeoutMs, bool forceReload) {
    Q_UNUSED(mode)
    RunWaitResult out;
    RunEngine& engine = RunEngine::instance();
    Project* project = ProjectManager::instance().currentProject();
    if (project) {
        QString loadError;
        if (!ensureEngineLoaded(&loadError, forceReload)) {
            out.error = loadError;
            return out;
        }
    }
    // 无工程时保持 run_flow 既有语义：直接使用引擎里已手动加载的模块
    if (engine.modules().isEmpty()) {
        out.error = project ? QStringLiteral("No modules to run") : QStringLiteral("No project opened");
        return out;
    }
    const AgentRunWaiter::Outcome outcome =
        m_runWaiter->waitForRun(engine, timeoutMs, [&engine]() { return engine.runOnceAsync(); });
    out.started = !outcome.rejectedStart && !outcome.reentrantRejected;
    out.finished = outcome.finished;
    out.timedOut = outcome.timedOut;
    out.aborted = outcome.aborted;
    out.pausedAtBreakpoint = outcome.pausedAtBreakpoint;
    out.reentrant = outcome.reentrantRejected;
    out.rejectedStart = outcome.rejectedStart;
    out.failedModule = outcome.failedModule;
    out.elapsedMs = outcome.result.elapsedMs;
    out.ok = outcome.finished && outcome.result.success;
    if (outcome.finished && !outcome.result.success)
        out.error = outcome.result.errorMessage;
    out.totalRuns = engine.totalRuns();
    out.successRuns = engine.successRuns();
    if (outcome.timedOut || outcome.aborted)
        engine.stop();
    return out;
}

QJsonObject AgentActor::validateWorkflow(const QJsonObject& params) {
    Q_UNUSED(params)
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorObject(QStringLiteral("No project opened"), QStringLiteral("no_project"));
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy())
        return errorObject(QStringLiteral("Flow is running; stop it before validating"), QStringLiteral("engine_busy"));
    QString loadError;
    if (!ensureEngineLoaded(&loadError))
        return errorObject(loadError, QStringLiteral("engine_load_failed"));

    const int moduleCount = project->modules().size();
    QJsonArray issues;
    if (moduleCount == 0) {
        issues.append(QJsonObject{{QStringLiteral("code"), QStringLiteral("no_modules")},
                                  {QStringLiteral("node"), QString()},
                                  {QStringLiteral("message"), QStringLiteral("Project has no modules")}});
        return QJsonObject{
            {QStringLiteral("ok"), false}, {QStringLiteral("issues"), issues}, {QStringLiteral("moduleCount"), 0}};
    }
    for (const RunEngine::FlowIssue& issue : engine.validateFlowIssues()) {
        issues.append(QJsonObject{{QStringLiteral("code"), issue.code},
                                  {QStringLiteral("node"), issue.node},
                                  {QStringLiteral("port"), issue.port},
                                  {QStringLiteral("message"), issue.message}});
    }
    return QJsonObject{{QStringLiteral("ok"), issues.isEmpty()},
                       {QStringLiteral("issues"), issues},
                       {QStringLiteral("moduleCount"), moduleCount}};
}

QJsonObject AgentActor::getNodeRuntimeOutputs(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorObject(QStringLiteral("No project opened"), QStringLiteral("no_project"));
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy()) {
        return errorObject(QStringLiteral("Flow is running; stop it before reading node outputs"),
                           QStringLiteral("engine_busy"));
    }
    QString loadError;
    if (!ensureEngineLoaded(&loadError))
        return errorObject(loadError, QStringLiteral("engine_load_failed"));

    const QString nodeName = params.value("nodeName").toString();
    NodeRef ref;
    if (!resolveNodeRef(project, nodeName, &ref))
        return errorObject(QString("Module not found: %1").arg(nodeName), QStringLiteral("node_not_found"));

    const QMap<QString, RunEngine::ModuleRuntimeInfo> snapshot = engine.moduleRuntimeSnapshot();
    const RunEngine::ModuleRuntimeInfo info = snapshot.value(ref.runtimeName);
    const PortOutputsPayload payload = serializePortOutputs(engine.nodePortOutputs(ref.runtimeName));

    QJsonObject result{{QStringLiteral("node"), ref.runtimeName},
                       {QStringLiteral("instanceId"), ref.instanceId},
                       {QStringLiteral("frameId"), static_cast<double>(engine.lastFrameId())},
                       {QStringLiteral("outputs"), payload.outputs}};
    if (!payload.unsupported.isEmpty()) {
        QJsonArray unsupported;
        for (const QString& port : payload.unsupported)
            unsupported.append(port);
        result.insert(QStringLiteral("unsupportedPorts"), unsupported);
    }
    if (!info.valid) {
        result.insert(QStringLiteral("lastStatus"), QStringLiteral("unknown"));
        result.insert(QStringLiteral("note"), QStringLiteral("node has no runtime output yet; run the flow first"));
        return result;
    }
    QString status = QStringLiteral("success");
    if (info.skipped)
        status = QStringLiteral("skipped");
    else if (!info.success)
        status = QStringLiteral("failed");
    result.insert(QStringLiteral("lastStatus"), status);
    result.insert(QStringLiteral("elapsedMs"), info.elapsedMs);
    result.insert(QStringLiteral("finishedAt"), info.finishedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")));
    if (status == QStringLiteral("failed"))
        result.insert(QStringLiteral("errorMessage"), info.errorMessage);
    return result;
}

QJsonObject AgentActor::getExecutionLogs(const QJsonObject& params) {
    const QString category = params.value("category").toString().trimmed();
    const QString levelName = params.value("level").toString(QStringLiteral("debug")).trimmed().toLower();
    const QString keyword = params.value("keyword").toString();
    const int limit = qBound(1, params.value("limit").toInt(50), 200);

    LogLevel minLevel = LogLevel::Debug;
    if (levelName == QStringLiteral("info"))
        minLevel = LogLevel::Info;
    else if (levelName == QStringLiteral("warning"))
        minLevel = LogLevel::Warning;
    else if (levelName == QStringLiteral("error"))
        minLevel = LogLevel::Error;
    else if (levelName == QStringLiteral("success"))
        minLevel = LogLevel::Success;

    QList<LogEntry> matched;
    const QList<LogEntry> all = Logger::instance().logs();
    for (const LogEntry& entry : all) {
        if (!category.isEmpty() && entry.category != category)
            continue;
        if (static_cast<int>(entry.level) < static_cast<int>(minLevel))
            continue;
        if (!keyword.isEmpty() && !entry.message.contains(keyword, Qt::CaseInsensitive))
            continue;
        matched.append(entry);
    }

    QJsonArray entries;
    const int first = qMax(0, matched.size() - limit);
    for (int i = first; i < matched.size(); ++i) {
        entries.append(QJsonObject{
            {QStringLiteral("time"), matched[i].timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))},
            {QStringLiteral("level"), logLevelName(matched[i].level)},
            {QStringLiteral("category"), matched[i].category},
            {QStringLiteral("message"), matched[i].message}});
    }
    return QJsonObject{{QStringLiteral("entries"), entries},
                       {QStringLiteral("total"), matched.size()},
                       {QStringLiteral("truncated"), matched.size() > entries.size()},
                       {QStringLiteral("source"), QStringLiteral("Logger::memory")}};
}

QJsonObject AgentActor::tuneNodeAndVerify(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return QJsonObject{{"success", false}, {"error", "No project opened"}, {"code", "no_project"}};

    const QString nodeName = params.value("nodeName").toString();
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy()) {
        return QJsonObject{
            {"success", false}, {"error", "Flow is running; stop it before tuning"}, {"code", "engine_busy"}};
    }

    QString loadError;
    if (!ensureEngineLoaded(&loadError))
        return QJsonObject{{"success", false}, {"error", loadError}, {"code", "engine_load_failed"}};

    NodeRef ref;
    if (!resolveNodeRef(project, nodeName, &ref)) {
        return QJsonObject{
            {"success", false}, {"error", QString("Module not found: %1").arg(nodeName)}, {"code", "node_not_found"}};
    }

    const QJsonValue parametersValue = params.value("parameters");
    const QJsonObject patch = parametersValue.toObject();
    if (!parametersValue.isObject() || patch.isEmpty() || patch.size() > 32)
        return invalidParameter(QStringLiteral("'parameters' must be a non-empty object with 1..32 keys"), QString());

    const QString runMode = params.value("runMode").toString(QStringLiteral("flow"));
    if (runMode != QStringLiteral("flow") && runMode != QStringLiteral("node"))
        return invalidParameter(QString("Invalid runMode: %1").arg(runMode), QString());
    const int timeoutMs = qBound(1000, params.value("timeoutMs").toInt(60000), 300000);

    const std::optional<ModuleInstance> instOpt = project->moduleById(ref.instanceId);
    if (!instOpt) {
        return QJsonObject{
            {"success", false}, {"error", QString("Module not found: %1").arg(nodeName)}, {"code", "node_not_found"}};
    }
    std::unique_ptr<IModule> mod(PluginManager::instance().createModule(instOpt->moduleId));
    if (!mod) {
        return QJsonObject{{"success", false},
                           {"error", QString("Cannot create module for validation: %1").arg(instOpt->moduleId)},
                           {"code", "invalid_parameter"}};
    }

    // 预校验全部键：任一键失败 → 工程参数零修改
    const QJsonObject currentParams = mergedParams(mod.get(), *instOpt);
    QJsonObject normalized;
    QJsonObject merged = currentParams;
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
        const ParamNormalizeResult conversion = normalizeParamValue(mod.get(), *instOpt, it.key(), it.value());
        if (!conversion.ok)
            return invalidParameter(conversion.error, it.key());
        normalized.insert(it.key(), conversion.value);
        merged.insert(it.key(), conversion.value);
        QString verr;
        if (!mod->validateParams(merged, verr))
            return invalidParameter(verr, it.key());
    }

    QJsonObject beforeValues;
    QJsonObject afterValues;
    for (auto it = normalized.constBegin(); it != normalized.constEnd(); ++it) {
        beforeValues.insert(it.key(), scalarToString(currentParams.value(it.key())));
        afterValues.insert(it.key(), scalarToString(merged.value(it.key())));
    }

    // 原子写入：宏内多条 set 命令，一次撤销回滚整组
    m_undoStack->beginMacro(QStringLiteral("Agent tune %1").arg(ref.runtimeName));
    for (auto it = normalized.constBegin(); it != normalized.constEnd(); ++it) {
        const bool hadOldValue = instOpt->params.contains(it.key());
        const QJsonValue oldVal = instOpt->params.value(it.key());
        m_undoStack->push(new AgentSetParamCmd(ref.instanceId, it.key(), hadOldValue, oldVal, it.value()));
    }
    m_undoStack->endMacro();
    m_engineProjectDirty = true;

    QJsonObject result{{QStringLiteral("node"), ref.runtimeName}, {QStringLiteral("instanceId"), ref.instanceId},
                       {QStringLiteral("runMode"), runMode},      {QStringLiteral("applied"), normalized.size()},
                       {QStringLiteral("before"), beforeValues},  {QStringLiteral("after"), afterValues},
                       {QStringLiteral("undoable"), true}};

    if (runMode == QStringLiteral("node")) {
        // 零副作用改参核对：不执行节点、不触发运行，返回上一帧输出
        const PortOutputsPayload payload = serializePortOutputs(engine.nodePortOutputs(ref.runtimeName));
        const RunEngine::ModuleRuntimeInfo info = engine.moduleRuntimeSnapshot().value(ref.runtimeName);
        QString status = QStringLiteral("unknown");
        if (info.valid)
            status = info.skipped ? QStringLiteral("skipped")
                                  : (info.success ? QStringLiteral("success") : QStringLiteral("failed"));
        result.insert(QStringLiteral("success"), true);
        result.insert(QStringLiteral("executed"), false);
        result.insert(QStringLiteral("staleOutputs"), payload.outputs);
        result.insert(QStringLiteral("lastStatus"), status);
        result.insert(QStringLiteral("note"), QStringLiteral("runMode=node does not execute the flow; outputs are from "
                                                             "the previous run. Use runMode=flow to verify."));
        return result;
    }

    const auto tuneFailure = [&](const QString& code, const QString& error, const QString& failedModule) {
        QJsonObject failure{
            {"success", false},
            {"code", code},
            {"error", error},
            {"node", ref.runtimeName},
            {"runMode", runMode},
            {"applied", normalized.size()},
            {"parametersRolledBack", false},
            {"note", QStringLiteral("Parameters were applied and kept; undo one step to roll back the whole patch.")},
            {"before", beforeValues},
            {"after", afterValues}};
        if (!failedModule.isEmpty())
            failure.insert(QStringLiteral("failedModule"), failedModule);
        return failure;
    };

    const RunWaitResult run = runAndWait(QStringLiteral("once"), timeoutMs, true);
    if (run.timedOut)
        return tuneFailure(QStringLiteral("timeout"), QString("Flow did not finish within %1 ms").arg(timeoutMs),
                           QString());
    if (run.aborted)
        return tuneFailure(QStringLiteral("cancelled"), QStringLiteral("Run aborted by user"), QString());
    if (run.reentrant) {
        return tuneFailure(QStringLiteral("reentrant_wait"),
                           QStringLiteral("Another run wait is already active on this thread"), QString());
    }
    if (run.pausedAtBreakpoint) {
        return tuneFailure(QStringLiteral("paused_at_breakpoint"),
                           QStringLiteral("Flow paused at a breakpoint; continue or stop it from the UI"), QString());
    }
    if (!run.started)
        return tuneFailure(QStringLiteral("run_failed"), run.error, run.failedModule);

    const PortOutputsPayload payload = serializePortOutputs(engine.nodePortOutputs(ref.runtimeName));
    const RunEngine::ModuleRuntimeInfo info = engine.moduleRuntimeSnapshot().value(ref.runtimeName);
    QString status = QStringLiteral("unknown");
    if (info.valid)
        status = info.skipped ? QStringLiteral("skipped")
                              : (info.success ? QStringLiteral("success") : QStringLiteral("failed"));
    result.insert(QStringLiteral("success"), run.ok);
    result.insert(QStringLiteral("elapsedMs"), run.elapsedMs);
    result.insert(QStringLiteral("outputs"), payload.outputs);
    result.insert(QStringLiteral("lastStatus"), status);
    if (!run.ok) {
        result.insert(QStringLiteral("code"), QStringLiteral("run_failed"));
        result.insert(QStringLiteral("failedModule"), run.failedModule);
        result.insert(QStringLiteral("errorMessage"), run.error);
        result.insert(QStringLiteral("error"), run.error);
        // 设计取舍：参数保留（不静默回滚），一次撤销可回滚整组；用显式字段告知模型
        result.insert(QStringLiteral("parametersRolledBack"), false);
        result.insert(QStringLiteral("note"),
                      QStringLiteral("Parameters were applied and kept; undo one step to roll back the whole patch."));
    }
    return result;
}

QJsonObject AgentActor::captureViewportImage(const QJsonObject& params) {
    Q_UNUSED(params)
    QImage image;
    QString source;
    QString providerError;
    if (m_viewportImageProvider) {
        image = m_viewportImageProvider(&providerError);
        if (!image.isNull())
            source = QStringLiteral("viewport");
    }
    if (image.isNull()) {
        const ImageData last = RunEngine::instance().lastOutput();
        if (last.isValid()) {
            image = last.toQImage();
            source = QStringLiteral("last_output");
        }
    }
    if (image.isNull()) {
        return errorObject(providerError.isEmpty() ? QStringLiteral("No image available in the viewport")
                                                   : providerError,
                           QStringLiteral("no_image"));
    }
    const QString path = AgentToolImage::writeImage(image, QStringLiteral("capture_viewport_image"));
    if (path.isEmpty())
        return errorObject(QStringLiteral("Failed to write image: cannot save PNG"),
                           QStringLiteral("image_write_failed"));
    return QJsonObject{{QStringLiteral("imagePath"), path},
                       {QStringLiteral("width"), image.width()},
                       {QStringLiteral("height"), image.height()},
                       {QStringLiteral("source"), source},
                       {QStringLiteral("bytes"), static_cast<double>(QFileInfo(path).size())}};
}

QJsonObject AgentActor::exportNodeOutput(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorObject(QStringLiteral("No project opened"), QStringLiteral("no_project"));
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy()) {
        return errorObject(QStringLiteral("Flow is running; stop it before exporting node output"),
                           QStringLiteral("engine_busy"));
    }
    QString loadError;
    if (!ensureEngineLoaded(&loadError))
        return errorObject(loadError, QStringLiteral("engine_load_failed"));

    const QString nodeName = params.value("nodeName").toString();
    NodeRef ref;
    if (!resolveNodeRef(project, nodeName, &ref))
        return errorObject(QString("Module not found: %1").arg(nodeName), QStringLiteral("node_not_found"));

    const QString format = params.value("format").toString(QStringLiteral("png")).toLower();
    QImage image;
    QString source = QStringLiteral("module_output");
    const PortValueMap ports = engine.nodePortOutputs(ref.runtimeName);
    const QVariant imageValue = ports.value(QStringLiteral("image"));
    if (imageValue.userType() == qMetaTypeId<ImageData>()) {
        const ImageData data = imageValue.value<ImageData>();
        if (data.isValid())
            image = data.toQImage();
    }
    if (image.isNull()) {
        const ImageData moduleImage = engine.moduleOutput(ref.runtimeName);
        if (moduleImage.isValid())
            image = moduleImage.toQImage();
    }
    // R3（设计 §4.6）：本工具只接受节点运行输出，禁止回退到视口/上一帧——
    // 回退会把"节点无图像"变成假成功，且与 capture_viewport_image 的职责混淆。
    if (image.isNull())
        return errorObject(QString("Node '%1' produced no image output").arg(ref.runtimeName),
                           QStringLiteral("no_image_output"));

    const QString path = AgentToolImage::writeImage(image, QStringLiteral("export_node_output"), format);
    if (path.isEmpty())
        return errorObject(QStringLiteral("Failed to write image"), QStringLiteral("image_write_failed"));
    return QJsonObject{{QStringLiteral("imagePath"), path},
                       {QStringLiteral("width"), image.width()},
                       {QStringLiteral("height"), image.height()},
                       {QStringLiteral("source"), source},
                       {QStringLiteral("format"), format},
                       {QStringLiteral("bytes"), static_cast<double>(QFileInfo(path).size())}};
}

} // namespace DeepLux
