#include "Project.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSaveFile>
#include <QStringList>
#include <QUuid>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace DeepLux {

namespace {
std::atomic<quint64> g_nextProjectRuntimeIdentity{1};
constexpr auto kMeasurementInputPickCoordinateFrame = "pick_coordinate_frame";
constexpr auto kUnspecifiedCoordinateFrame = "Unspecified";

bool isMeasurementInput(const ModuleInstance& module) {
    return module.moduleId == QLatin1String("MeasurementInput") ||
           module.moduleId == QLatin1String("com.deeplux.plugin.measurementinput");
}

bool invalidatesMeasurementInputPickFrame(const QString& key) {
    return key == QLatin1String("mode") || key == QLatin1String("point") || key == QLatin1String("line") ||
           key == QLatin1String("line1") || key == QLatin1String("line2");
}
#ifdef DEEPLUX_ENABLE_TEST_HOOKS
std::atomic<qint64> g_failNextProjectSaveAfterBytes{-1};
#endif
} // namespace

// ========== ModuleConnection ==========

QJsonObject ModuleConnection::toJson() const {
    QJsonObject json;
    json["fromModuleId"] = fromModuleId;
    json["toModuleId"] = toModuleId;
    json["fromOutput"] = fromOutput;
    json["toInput"] = toInput;
    if (!fromPort.isEmpty())
        json["fromPort"] = fromPort;
    if (!toPort.isEmpty())
        json["toPort"] = toPort;
    if (!edgeType.isEmpty())
        json["edgeType"] = edgeType;
    return json;
}

ModuleConnection ModuleConnection::fromJson(const QJsonObject& json) {
    ModuleConnection conn;
    conn.fromModuleId = json["fromModuleId"].toString();
    conn.toModuleId = json["toModuleId"].toString();
    conn.fromOutput = json["fromOutput"].toInt(0);
    conn.toInput = json["toInput"].toInt(0);
    conn.fromPort = json["fromPort"].toString();
    conn.toPort = json["toPort"].toString();
    // 阶段 C 复核(P1): 缺失 edgeType 默认为空，交给引擎推断（不假设 data）
    conn.edgeType = json["edgeType"].toString();
    return conn;
}

QJsonObject ProjectFlow::toJson() const {
    QJsonObject json;
    json["id"] = id;
    json["name"] = name;
    QJsonArray nodes;
    for (const QString& n : nodeIds)
        nodes.append(n);
    json["nodes"] = nodes;
    return json;
}

ProjectFlow ProjectFlow::fromJson(const QJsonObject& json) {
    ProjectFlow flow;
    flow.id = json["id"].toString(QStringLiteral("main"));
    flow.name = json["name"].toString();
    for (const QJsonValue& v : json["nodes"].toArray())
        flow.nodeIds.append(v.toString());
    return flow;
}

QJsonObject MigrationRecord::toJson() const {
    QJsonObject json;
    json["originalVersion"] = originalVersion;
    json["migratedAt"] = migratedAt.toString(Qt::ISODate);
    QJsonArray warns;
    for (const QString& w : warnings)
        warns.append(w);
    json["warnings"] = warns;
    return json;
}

MigrationRecord MigrationRecord::fromJson(const QJsonObject& json) {
    MigrationRecord rec;
    rec.originalVersion = json["originalVersion"].toString();
    rec.migratedAt = QDateTime::fromString(json["migratedAt"].toString(), Qt::ISODate);
    for (const QJsonValue& v : json["warnings"].toArray())
        rec.warnings.append(v.toString());
    return rec;
}

// ========== ModuleInstance ==========

namespace {
bool migrateLegacyCalibrationParams(QJsonObject& params, const QString& moduleId) {
    if ((moduleId != QStringLiteral("com.deeplux.plugin.npointcalibration") &&
         moduleId != QStringLiteral("NPointCalibration") && moduleId != QStringLiteral("N点标定")) ||
        params.contains(QStringLiteral("calibrationPoints")) || !params.contains(QStringLiteral("pointCount")))
        return true;
    const QJsonValue countValue = params.value(QStringLiteral("pointCount"));
    const double countNumber = countValue.toDouble();
    if (!countValue.isDouble() || !std::isfinite(countNumber) || std::floor(countNumber) != countNumber ||
        countNumber < 0 || countNumber > 64)
        return false;
    QJsonArray points;
    for (int i = 0; i < static_cast<int>(countNumber); ++i) {
        const QString suffix = QString::number(i);
        QJsonArray coordinates;
        for (const char* prefix : {"imgX_", "imgY_", "worldX_", "worldY_"}) {
            const QJsonValue value = params.value(QString::fromLatin1(prefix) + suffix);
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 1e7)
                return false;
            coordinates.append(value);
        }
        points.append(QJsonObject{{"image", QJsonArray{coordinates[0], coordinates[1]}},
                                  {"world", QJsonArray{coordinates[2], coordinates[3]}}});
    }
    for (int i = 0; i < static_cast<int>(countNumber); ++i) {
        const QString suffix = QString::number(i);
        for (const char* prefix : {"imgX_", "imgY_", "worldX_", "worldY_"})
            params.remove(QString::fromLatin1(prefix) + suffix);
    }
    params.remove(QStringLiteral("pointCount"));
    params.insert(QStringLiteral("calibrationPoints"), points);
    return true;
}
} // namespace

QJsonObject ModuleInstance::toJson() const {
    QJsonObject json;
    json["id"] = id;
    json["moduleId"] = moduleId;
    json["name"] = name;
    json["posX"] = posX;
    json["posY"] = posY;
    json["params"] = params;
    json["note"] = note;
    json["enabled"] = enabled;
    json["breakpoint"] = breakpoint;
    return json;
}

ModuleInstance ModuleInstance::fromJson(const QJsonObject& json) {
    ModuleInstance inst;
    inst.id = json["id"].toString();
    inst.moduleId = json["moduleId"].toString();
    inst.name = json["name"].toString();
    inst.posX = json["posX"].toInt(0);
    inst.posY = json["posY"].toInt(0);
    inst.params = json["params"].toObject();
    migrateLegacyCalibrationParams(inst.params, inst.moduleId);
    inst.note = json["note"].toString();
    inst.enabled = json["enabled"].toBool(true); // 旧工程缺省为启用
    inst.breakpoint = json["breakpoint"].toBool(false);
    return inst;
}

// ========== CameraConfig ==========

QJsonObject CameraConfig::toJson() const {
    QJsonObject json;
    json["id"] = id;
    json["type"] = type;
    json["serialNumber"] = serialNumber;
    json["config"] = config;
    return json;
}

CameraConfig CameraConfig::fromJson(const QJsonObject& json) {
    CameraConfig cfg;
    cfg.id = json["id"].toString();
    cfg.type = json["type"].toString();
    cfg.serialNumber = json["serialNumber"].toString();
    cfg.config = json["config"].toObject();
    return cfg;
}

// ========== Project ==========

Project::Project(QObject* parent)
    : QObject(parent), m_id(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_runtimeIdentity(g_nextProjectRuntimeIdentity.fetch_add(1, std::memory_order_relaxed)), m_name(tr("未命名项目")),
      m_created(QDateTime::currentDateTime()), m_modifiedTime(QDateTime::currentDateTime()),
      m_sessionToken(QUuid::createUuid()) {
    qRegisterMetaType<StructureEditOrigin>("StructureEditOrigin");
    qRegisterMetaType<StructureEditOrigin>("DeepLux::StructureEditOrigin");
}

Project::~Project() {}

void Project::setName(const QString& name) {
    if (m_name != name) {
        m_name = name;
        touch();
        emit nameChanged(name);
    }
}

void Project::setFilePath(const QString& path) {
    m_filePath = path;
}

void Project::addModule(const ModuleInstance& module, int index, ModuleOrderChangeSource source) {
    if (!module.id.isEmpty() && moduleById(module.id))
        return;
    m_modules.insert(index < 0 ? m_modules.size() : qMin(index, m_modules.size()), module);
    if (source == ModuleOrderChangeSource::External)
        ++m_moduleOrderExternalGeneration;
    if (!m_positionGenerations.contains(module.id))
        m_positionGenerations.insert(module.id, PositionGenerations{});
    ++m_parameterInstanceGenerations[module.id];
    QSet<QString> parameterKeys;
    for (auto it = module.params.constBegin(); it != module.params.constEnd(); ++it)
        parameterKeys.insert(it.key());
    recordParameterChanges(module.id, parameterKeys, ParameterChangeSource::Unknown);
    touch();
    emit moduleAdded(module);
}

void Project::removeModule(const QString& instanceId, ModuleOrderChangeSource source) {
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            QSet<QString> parameterKeys;
            for (auto it = m_modules[i].params.constBegin(); it != m_modules[i].params.constEnd(); ++it)
                parameterKeys.insert(it.key());
            recordParameterChanges(instanceId, parameterKeys, ParameterChangeSource::Unknown);
            ++m_parameterInstanceGenerations[instanceId];
            m_modules.removeAt(i);
            if (source == ModuleOrderChangeSource::External)
                ++m_moduleOrderExternalGeneration;
            touch();
            emit moduleRemoved(instanceId);
            return;
        }
    }
}

void Project::updateModule(const QString& instanceId, const ModuleInstance& module, PositionChangeSource positionSource,
                           ParameterChangeSource parameterSource, RuntimeFieldChangeSource runtimeFieldSource) {
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            ModuleInstance updated = module;
            if (isMeasurementInput(m_modules[i]) && isMeasurementInput(updated) &&
                m_modules[i].params.contains(kMeasurementInputPickCoordinateFrame)) {
                bool coordinatesChanged = false;
                for (const QString& key : {QStringLiteral("mode"), QStringLiteral("point"), QStringLiteral("line"),
                                           QStringLiteral("line1"), QStringLiteral("line2")})
                    coordinatesChanged =
                        coordinatesChanged || m_modules[i].params.value(key) != updated.params.value(key);
                if (coordinatesChanged)
                    updated.params.insert(kMeasurementInputPickCoordinateFrame, kUnspecifiedCoordinateFrame);
            }
            const bool positionChanged = m_modules[i].posX != updated.posX || m_modules[i].posY != updated.posY;
            const bool noteChanged = m_modules[i].note != updated.note;
            const bool enabledChanged = m_modules[i].enabled != updated.enabled;
            const bool breakpointChanged = m_modules[i].breakpoint != updated.breakpoint;
            QSet<QString> changedParameterKeys;
            for (auto it = m_modules[i].params.constBegin(); it != m_modules[i].params.constEnd(); ++it) {
                if (!updated.params.contains(it.key()) || updated.params.value(it.key()) != it.value())
                    changedParameterKeys.insert(it.key());
            }
            for (auto it = updated.params.constBegin(); it != updated.params.constEnd(); ++it) {
                if (!m_modules[i].params.contains(it.key()))
                    changedParameterKeys.insert(it.key());
            }
            if (positionChanged) {
                PositionGenerations& generations = m_positionGenerations[instanceId];
                switch (positionSource) {
                case PositionChangeSource::Gui:
                    ++generations.agent;
                    break;
                case PositionChangeSource::Agent:
                    ++generations.gui;
                    break;
                case PositionChangeSource::Unspecified:
                    ++generations.gui;
                    ++generations.agent;
                    break;
                }
            }
            if (runtimeFieldSource == RuntimeFieldChangeSource::External) {
                RuntimeFieldExternalGenerations& generations = m_runtimeFieldExternalGenerations[instanceId];
                if (noteChanged)
                    ++generations.note;
                if (enabledChanged)
                    ++generations.enabled;
                if (breakpointChanged)
                    ++generations.breakpoint;
            }
            recordParameterChanges(instanceId, changedParameterKeys, parameterSource);
            m_modules[i] = updated;
            touch();
            emit moduleUpdated(updated);
            if (positionChanged)
                emit modulePositionChanged(instanceId, module.posX, module.posY);
            return;
        }
    }
}

Project::PositionGenerations Project::positionGenerations(const QString& instanceId) const {
    return m_positionGenerations.value(instanceId);
}

Project::ParameterGenerations Project::parameterGenerations(const QString& instanceId, const QString& key) const {
    return m_parameterGenerations.value(instanceId).value(key);
}

quint64 Project::parameterInstanceGeneration(const QString& instanceId) const {
    return m_parameterInstanceGenerations.value(instanceId);
}

quint64 Project::runtimeFieldExternalGeneration(const QString& instanceId, RuntimeField field) const {
    const RuntimeFieldExternalGenerations generations = m_runtimeFieldExternalGenerations.value(instanceId);
    switch (field) {
    case RuntimeField::Note:
        return generations.note;
    case RuntimeField::Enabled:
        return generations.enabled;
    case RuntimeField::Breakpoint:
        return generations.breakpoint;
    }
    return 0;
}

void Project::recordParameterChanges(const QString& instanceId, const QSet<QString>& keys,
                                     ParameterChangeSource source) {
    for (const QString& key : keys) {
        ParameterGenerations& generations = m_parameterGenerations[instanceId][key];
        if (source == ParameterChangeSource::Gui || source == ParameterChangeSource::Unknown)
            ++generations.gui;
        if (source == ParameterChangeSource::Agent || source == ParameterChangeSource::Unknown)
            ++generations.agent;
    }
}

bool Project::setModuleParam(const QString& instanceId, const QString& key, const QJsonValue& value,
                             ParameterChangeSource source) {
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            const bool wasPresent = m_modules[i].params.contains(key);
            const QJsonValue oldValue = m_modules[i].params.value(key);
            if (value.isUndefined()) {
                if (!wasPresent)
                    return true;
                m_modules[i].params.remove(key);
            } else {
                if (wasPresent && oldValue == value)
                    return true;
                m_modules[i].params.insert(key, value);
            }
            QSet<QString> changedKeys{key};
            if (isMeasurementInput(m_modules[i]) && invalidatesMeasurementInputPickFrame(key) &&
                m_modules[i].params.contains(kMeasurementInputPickCoordinateFrame)) {
                m_modules[i].params.insert(kMeasurementInputPickCoordinateFrame, kUnspecifiedCoordinateFrame);
                changedKeys.insert(kMeasurementInputPickCoordinateFrame);
            }
            recordParameterChanges(instanceId, changedKeys, source);
            touch();
            emit moduleUpdated(m_modules[i]);
            return true;
        }
    }
    return false;
}

bool Project::removeModuleParam(const QString& instanceId, const QString& key, ParameterChangeSource source) {
    for (int i = 0; i < m_modules.size(); ++i) {
        if (m_modules[i].id == instanceId && m_modules[i].params.contains(key)) {
            m_modules[i].params.remove(key);
            QSet<QString> changedKeys{key};
            if (isMeasurementInput(m_modules[i]) && invalidatesMeasurementInputPickFrame(key) &&
                m_modules[i].params.contains(kMeasurementInputPickCoordinateFrame)) {
                m_modules[i].params.insert(kMeasurementInputPickCoordinateFrame, kUnspecifiedCoordinateFrame);
                changedKeys.insert(kMeasurementInputPickCoordinateFrame);
            }
            recordParameterChanges(instanceId, changedKeys, source);
            touch();
            emit moduleUpdated(m_modules[i]);
            return true;
        }
    }
    return false;
}

bool Project::moveModule(const QString& instanceId, int newIndex, ModuleOrderChangeSource source) {
    if (m_modules.isEmpty()) {
        return false;
    }

    int oldIndex = -1;
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            oldIndex = i;
            break;
        }
    }
    if (oldIndex < 0) {
        return false;
    }

    newIndex = qMax(0, qMin(newIndex, m_modules.size() - 1));
    if (oldIndex == newIndex) {
        return true;
    }

    m_modules.move(oldIndex, newIndex);
    if (source == ModuleOrderChangeSource::External)
        ++m_moduleOrderExternalGeneration;
    touch();
    emit moduleMoved(instanceId, newIndex);
    return true;
}

void Project::setModulePosition(const QString& instanceId, int x, int y) {
    for (ModuleInstance& module : m_modules) {
        if (module.id != instanceId)
            continue;
        if (module.posX == x && module.posY == y)
            return;
        module.posX = x;
        module.posY = y;
        touch();
        emit modulePositionChanged(instanceId, x, y);
        return;
    }
}

bool Project::reorderModules(const QStringList& orderedIds) {
    if (orderedIds.size() != m_modules.size())
        return false;

    QSet<QString> seen;
    QList<ModuleInstance> reordered;
    reordered.reserve(m_modules.size());
    for (const QString& id : orderedIds) {
        if (id.isEmpty() || seen.contains(id))
            return false;
        auto module = moduleById(id);
        if (!module)
            return false;
        seen.insert(id);
        reordered.append(*module);
    }

    bool changed = false;
    for (int i = 0; i < reordered.size(); ++i) {
        if (reordered.at(i).id != m_modules.at(i).id) {
            changed = true;
            break;
        }
    }
    if (!changed)
        return true;

    m_modules = reordered;
    touch();
    emit modulesReordered();
    return true;
}

bool Project::applyStructureTransaction(const FlowGraphCandidate& candidate, StructureEditOrigin origin,
                                        QString* error) {
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };

    // 1) orderedIds 必须是当前模块集合的完整排列
    QSet<QString> existingIds;
    for (const ModuleInstance& module : m_modules)
        existingIds.insert(module.id);

    if (candidate.orderedIds.size() != m_modules.size()) {
        return fail(QStringLiteral("Module order must contain exactly %1 ids").arg(m_modules.size()));
    }
    QSet<QString> seenIds;
    for (const QString& id : candidate.orderedIds) {
        if (id.isEmpty())
            return fail(QStringLiteral("Module order contains an empty id"));
        if (seenIds.contains(id))
            return fail(QStringLiteral("Module order contains duplicate id: %1").arg(id));
        if (!existingIds.contains(id))
            return fail(QStringLiteral("Module order references unknown module: %1").arg(id));
        seenIds.insert(id);
    }
    if (seenIds.size() != existingIds.size())
        return fail(QStringLiteral("Module order is not a complete permutation"));

    // 2) 连接端点必须存在，且四元组不得重复（声明顺序保留）
    QSet<QString> edgeKeys;
    for (const ModuleConnection& connection : candidate.connections) {
        if (!existingIds.contains(connection.fromModuleId)) {
            return fail(QStringLiteral("Connection references missing source module: %1").arg(connection.fromModuleId));
        }
        if (!existingIds.contains(connection.toModuleId)) {
            return fail(QStringLiteral("Connection references missing target module: %1").arg(connection.toModuleId));
        }
        const QString key = connection.fromModuleId + QLatin1Char('\x1f') + connection.fromPort + QLatin1Char('\x1f') +
                            connection.toModuleId + QLatin1Char('\x1f') + connection.toPort;
        if (edgeKeys.contains(key)) {
            return fail(
                QStringLiteral("Duplicate connection %1.%2 -> %3.%4")
                    .arg(connection.fromModuleId, connection.fromPort, connection.toModuleId, connection.toPort));
        }
        edgeKeys.insert(key);
    }

    // 3) 校验全部通过后再原子替换
    QList<ModuleInstance> reordered;
    reordered.reserve(candidate.orderedIds.size());
    for (const QString& id : candidate.orderedIds)
        reordered.append(*moduleById(id));

    m_modules = reordered;
    m_connections = candidate.connections;
    if (error)
        error->clear();
    touch();
    emit structureChanged(origin);
    return true;
}

QString Project::structureFingerprint() const {
    QStringList orderedIds;
    orderedIds.reserve(m_modules.size());
    for (const ModuleInstance& module : m_modules)
        orderedIds.append(module.id);
    return structureFingerprintFor(orderedIds, m_connections);
}

QString Project::structureFingerprintFor(const QStringList& orderedIds, const QList<ModuleConnection>& connections) {
    QByteArray payload;
    payload.append("modules\n");
    for (const QString& id : orderedIds) {
        payload.append(id.toUtf8());
        payload.append('\n');
    }
    payload.append("connections\n");
    for (int i = 0; i < connections.size(); ++i) {
        const ModuleConnection& connection = connections.at(i);
        payload.append(QByteArray::number(i));
        payload.append('|');
        payload.append(connection.fromModuleId.toUtf8());
        payload.append('|');
        payload.append(connection.fromPort.toUtf8());
        payload.append('|');
        payload.append(connection.toModuleId.toUtf8());
        payload.append('|');
        payload.append(connection.toPort.toUtf8());
        payload.append('|');
        payload.append(connection.edgeType.toUtf8());
        payload.append('\n');
    }
    return QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex());
}

void Project::notifyStructureChange(StructureEditOrigin origin) {
    emit structureChanged(origin);
}

QUuid Project::sessionToken() const {
    return m_sessionToken;
}

void Project::setLayoutInitialized(bool initialized) {
    if (m_layoutInitialized == initialized)
        return;
    m_layoutInitialized = initialized;
    touch();
}

ModuleInstance* Project::findModule(const QString& instanceId) {
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            return &m_modules[i];
        }
    }
    return nullptr;
}

std::optional<ModuleInstance> Project::moduleById(const QString& instanceId) const {
    for (const ModuleInstance& module : m_modules) {
        if (module.id == instanceId) {
            return module;
        }
    }
    return std::nullopt;
}

void Project::addConnection(const ModuleConnection& conn, int index) {
    // P1-fix: 四元组幂等去重——模型作为唯一事实源
    for (const ModuleConnection& existing : m_connections) {
        if (existing.fromModuleId == conn.fromModuleId && existing.toModuleId == conn.toModuleId &&
            existing.fromPort == conn.fromPort && existing.toPort == conn.toPort) {
            return; // 已存在，幂等
        }
    }
    m_connections.insert(index < 0 ? m_connections.size() : qMin(index, m_connections.size()), conn);
    touch();
    emit connectionAdded(conn);
}

void Project::removeConnection(const QString& fromId, const QString& toId) {
    for (int i = 0; i < m_connections.size(); i++) {
        if (m_connections[i].fromModuleId == fromId && m_connections[i].toModuleId == toId) {
            m_connections.removeAt(i);
            touch();
            emit connectionRemoved(fromId, toId);
            return;
        }
    }
}

void Project::removeConnectionWithPorts(const QString& fromId, const QString& fromPort, const QString& toId,
                                        const QString& toPort) {
    for (int i = 0; i < m_connections.size(); i++) {
        const ModuleConnection& c = m_connections[i];
        if (c.fromModuleId == fromId && c.toModuleId == toId && c.fromPort == fromPort && c.toPort == toPort) {
            m_connections.removeAt(i);
            touch();
            // P1-fix: 发射携带完整四元组的信号
            emit connectionRemovedWithPorts(fromId, fromPort, toId, toPort);
            return;
        }
    }
}

void Project::setConnections(const QList<ModuleConnection>& conns) {
    m_connections = conns;
    touch();
}

void Project::addCamera(const CameraConfig& camera) {
    m_cameras.append(camera);
    touch();
}

void Project::removeCamera(const QString& cameraId) {
    for (int i = 0; i < m_cameras.size(); i++) {
        if (m_cameras[i].id == cameraId) {
            m_cameras.removeAt(i);
            touch();
            return;
        }
    }
}

const CameraConfig* Project::findCamera(const QString& cameraId) const {
    for (int i = 0; i < m_cameras.size(); i++) {
        if (m_cameras[i].id == cameraId) {
            return &m_cameras[i];
        }
    }
    return nullptr;
}

void Project::addDataSource(const DataSource& ds) {
    for (const auto& existing : m_dataSources) {
        if (existing.filePath == ds.filePath) {
            return;
        }
    }
    m_dataSources.append(ds);
    ++m_dataSourceCollectionGeneration;
    touch();
    emit dataSourceAdded(ds);
}

void Project::removeDataSource(const QString& id) {
    for (int i = 0; i < m_dataSources.size(); i++) {
        if (m_dataSources[i].id == id) {
            m_dataSources.removeAt(i);
            ++m_dataSourceCollectionGeneration;
            touch();
            emit dataSourceRemoved(id);
            return;
        }
    }
}

std::optional<DataSource> Project::findDataSource(const QString& id) const {
    for (int i = 0; i < m_dataSources.size(); i++) {
        if (m_dataSources[i].id == id) {
            return m_dataSources[i];
        }
    }
    return std::nullopt;
}

void Project::setFlows(const QList<ProjectFlow>& flows) {
    m_flows = flows;
    touch();
}

ProjectFlow Project::flow(const QString& flowId) const {
    for (const ProjectFlow& f : m_flows) {
        if (f.id == flowId)
            return f;
    }
    return ProjectFlow();
}

void Project::setRecipes(const QJsonObject& recipes) {
    m_recipes = recipes;
    touch();
}

void Project::setDashboard(const QJsonObject& dashboard) {
    m_dashboard = dashboard;
    touch();
}

void Project::setMigration(const MigrationRecord& rec) {
    m_migration = rec;
    touch();
}

QJsonObject Project::toJson() const {
    QJsonObject json;
    json["version"] = m_formatVersion;
    json["id"] = m_id;
    json["name"] = m_name;
    json["created"] = m_created.toString(Qt::ISODate);
    json["modified"] = m_modifiedTime.toString(Qt::ISODate);

    // 模块
    QJsonArray modulesArray;
    for (const auto& module : m_modules) {
        modulesArray.append(module.toJson());
    }
    json["modules"] = modulesArray;

    // 连接
    QJsonArray connectionsArray;
    for (const auto& conn : m_connections) {
        connectionsArray.append(conn.toJson());
    }
    json["connections"] = connectionsArray;

    // 流程（3.0）：至少包含 main
    QJsonArray flowsArray;
    QList<ProjectFlow> flows = m_flows;
    if (flows.isEmpty()) {
        ProjectFlow main;
        main.id = QStringLiteral("main");
        main.name = QStringLiteral("主流程");
        for (const auto& m : m_modules)
            main.nodeIds.append(m.id);
        flows.append(main);
    }
    for (const auto& f : flows) {
        flowsArray.append(f.toJson());
    }
    json["flows"] = flowsArray;

    // 资源（3.0）：相机 + 数据源
    QJsonObject resources;
    QJsonArray camerasArray;
    for (const auto& camera : m_cameras) {
        camerasArray.append(camera.toJson());
    }
    resources["cameras"] = camerasArray;
    QJsonArray dataSourcesArray;
    for (const auto& ds : m_dataSources) {
        dataSourcesArray.append(ds.toJson());
    }
    resources["dataSources"] = dataSourcesArray;
    json["resources"] = resources;

    // recipes / dashboard / migration（3.0）
    json["recipes"] = m_recipes;
    json["dashboard"] = m_dashboard;
    if (!m_migration.originalVersion.isEmpty()) {
        json["migration"] = m_migration.toJson();
    }

    // B2：布局初始化标记（旧工程缺省 false）
    json["layoutInitialized"] = m_layoutInitialized;

    return json;
}

bool Project::fromJson(const QJsonObject& json) {
    QSet<QString> affectedModuleIds;
    for (const ModuleInstance& module : m_modules)
        affectedModuleIds.insert(module.id);
    m_formatVersion = json["version"].toString(QStringLiteral("2.0"));
    m_id = json["id"].toString();
    m_name = json["name"].toString();
    m_created = QDateTime::fromString(json["created"].toString(), Qt::ISODate);
    m_modifiedTime = QDateTime::fromString(json["modified"].toString(), Qt::ISODate);

    m_modules.clear();
    m_positionGenerations.clear();
    m_parameterGenerations.clear();
    QJsonArray modulesArray = json["modules"].toArray();
    for (const auto& val : modulesArray) {
        const ModuleInstance module = ModuleInstance::fromJson(val.toObject());
        m_modules.append(module);
        affectedModuleIds.insert(module.id);
    }
    for (const QString& instanceId : affectedModuleIds)
        ++m_parameterInstanceGenerations[instanceId];
    ++m_moduleOrderExternalGeneration;

    m_connections.clear();
    QJsonArray connectionsArray = json["connections"].toArray();
    for (const auto& val : connectionsArray) {
        m_connections.append(ModuleConnection::fromJson(val.toObject()));
    }

    // 3.0 资源在 resources 下；2.0 在顶层 cameras/dataSources
    QJsonObject resources = json["resources"].toObject();
    QJsonArray camerasArray =
        resources.contains("cameras") ? resources["cameras"].toArray() : json["cameras"].toArray();
    m_cameras.clear();
    for (const auto& val : camerasArray) {
        m_cameras.append(CameraConfig::fromJson(val.toObject()));
    }
    QJsonArray dataSourcesArray =
        resources.contains("dataSources") ? resources["dataSources"].toArray() : json["dataSources"].toArray();
    m_dataSources.clear();
    for (const auto& val : dataSourcesArray) {
        m_dataSources.append(DataSource::fromJson(val.toObject()));
    }
    ++m_dataSourceCollectionGeneration;

    // 3.0 流程/recipes/dashboard/migration
    m_flows.clear();
    for (const auto& val : json["flows"].toArray()) {
        m_flows.append(ProjectFlow::fromJson(val.toObject()));
    }
    m_recipes = json["recipes"].toObject();
    m_dashboard = json["dashboard"].toObject();
    if (json.contains("migration")) {
        m_migration = MigrationRecord::fromJson(json["migration"].toObject());
    }

    // B2：旧工程无 layoutInitialized 时默认 false；每次加载生成新会话标识
    m_layoutInitialized = json.value("layoutInitialized").toBool(false);
    m_sessionToken = QUuid::createUuid();

    m_hasUnsavedChanges = false;
    return true;
}

bool Project::save(const QString& path) {
    QString savePath = path.isEmpty() ? m_filePath : path;
    if (savePath.isEmpty()) {
        qWarning() << "No file path specified";
        return false;
    }

    QString writePath = savePath;
    const QFileInfo targetInfo(savePath);
    if (targetInfo.isSymLink()) {
        writePath = targetInfo.canonicalFilePath();
        if (writePath.isEmpty()) {
            qWarning() << "Cannot resolve project save symlink target:" << savePath;
            return false;
        }
    }

    QSaveFile file(writePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Cannot open file for writing:" << savePath;
        return false;
    }

    const QByteArray contents = QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
#ifdef DEEPLUX_ENABLE_TEST_HOOKS
    const qint64 failAfterBytes = g_failNextProjectSaveAfterBytes.exchange(-1, std::memory_order_acq_rel);
    if (failAfterBytes >= 0 && failAfterBytes < contents.size()) {
        file.write(contents.constData(), failAfterBytes);
        file.cancelWriting();
        return false;
    }
#endif
    const qint64 bytesWritten = file.write(contents);
    if (bytesWritten != contents.size()) {
        file.cancelWriting();
        qWarning() << "Cannot write complete project file:" << savePath;
        return false;
    }
    if (!file.commit()) {
        qWarning() << "Cannot commit project file:" << savePath << file.errorString();
        return false;
    }

    m_filePath = savePath;
    m_hasUnsavedChanges = false;
    emit modifiedChanged(false);

    qDebug() << "Project saved to:" << savePath;
    return true;
}

#ifdef DEEPLUX_ENABLE_TEST_HOOKS
void Project::failNextSaveAfterBytesForTesting(qint64 bytes) {
    g_failNextProjectSaveAfterBytes.store(qMax<qint64>(0, bytes), std::memory_order_release);
}
#endif

bool Project::load(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot open file for reading:" << path;
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "JSON parse error:" << error.errorString();
        return false;
    }

    // 解析成功不代表是工程文件。保留旧版缺省 version/id 的兼容性，但拒绝
    // 空对象、错误类型及不支持的显式版本，避免候选替换时丢掉当前工程。
    const QJsonObject json = doc.object();
    const QJsonValue version = json.value(QStringLiteral("version"));
    if (!doc.isObject() || !json.value(QStringLiteral("name")).isString() ||
        json.value(QStringLiteral("name")).toString().trimmed().isEmpty() ||
        !json.value(QStringLiteral("modules")).isArray() || !json.value(QStringLiteral("connections")).isArray() ||
        (!version.isUndefined() && (!version.isString() || (version.toString() != QLatin1String("2.0") &&
                                                            version.toString() != QLatin1String("3.0"))))) {
        qWarning() << "Invalid project structure:" << path;
        return false;
    }

    // fromJson 对类型不匹配的字段使用 Qt 的空值转换；文件入口必须先拒绝这些
    // 损坏字段，否则候选工程会带着静默丢失的数据替换当前工程。
    const auto validTypes = [](const QJsonObject& record,
                               std::initializer_list<std::pair<const char*, QJsonValue::Type>> fields) {
        for (const auto& field : fields) {
            const QJsonValue value = record.value(QLatin1String(field.first));
            if (!value.isUndefined() && value.type() != field.second)
                return false;
        }
        return true;
    };
    const auto objectArray = [](const QJsonValue& value) {
        if (value.isUndefined())
            return true; // 旧版工程允许缺省新字段
        if (!value.isArray())
            return false;
        for (const QJsonValue& item : value.toArray()) {
            if (!item.isObject())
                return false;
        }
        return true;
    };
    const auto validDate = [](const QJsonObject& record, const char* key) {
        const QJsonValue value = record.value(QLatin1String(key));
        if (value.isUndefined())
            return true;
        if (!value.isString())
            return false;
        const QString date = value.toString();
        return date.isEmpty() || QDateTime::fromString(date, Qt::ISODate).isValid();
    };
    const auto validInt32 = [](const QJsonObject& record, const char* key) {
        const QJsonValue value = record.value(QLatin1String(key));
        if (value.isUndefined())
            return true;
        const double number = value.toDouble();
        return value.isDouble() && std::isfinite(number) && std::trunc(number) == number &&
               number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max();
    };
    const QJsonObject resources = json.value(QStringLiteral("resources")).toObject();
    if (!validTypes(json, {{"id", QJsonValue::String},
                           {"created", QJsonValue::String},
                           {"modified", QJsonValue::String},
                           {"resources", QJsonValue::Object},
                           {"recipes", QJsonValue::Object},
                           {"dashboard", QJsonValue::Object},
                           {"migration", QJsonValue::Object}}) ||
        !objectArray(json.value(QStringLiteral("flows"))) || !objectArray(json.value(QStringLiteral("cameras"))) ||
        !objectArray(json.value(QStringLiteral("dataSources"))) || !validDate(json, "created") ||
        !validDate(json, "modified") || !objectArray(resources.value(QStringLiteral("cameras"))) ||
        !objectArray(resources.value(QStringLiteral("dataSources")))) {
        qWarning() << "Invalid project field type:" << path;
        return false;
    }

    const auto validRecords = [](const QJsonArray& records, const QStringList& requiredKeys) {
        for (const QJsonValue& value : records) {
            if (!value.isObject())
                return false;
            const QJsonObject record = value.toObject();
            for (const QString& key : requiredKeys) {
                if (!record.value(key).isString() || record.value(key).toString().trimmed().isEmpty())
                    return false;
            }
        }
        return true;
    };
    if (!validRecords(json.value(QStringLiteral("modules")).toArray(),
                      {QStringLiteral("id"), QStringLiteral("moduleId")}) ||
        !validRecords(json.value(QStringLiteral("connections")).toArray(),
                      {QStringLiteral("fromModuleId"), QStringLiteral("toModuleId")})) {
        qWarning() << "Invalid project module or connection:" << path;
        return false;
    }
    for (const QJsonValue& value : json.value(QStringLiteral("modules")).toArray()) {
        if (!validTypes(value.toObject(), {{"name", QJsonValue::String},
                                           {"posX", QJsonValue::Double},
                                           {"posY", QJsonValue::Double},
                                           {"params", QJsonValue::Object},
                                           {"note", QJsonValue::String},
                                           {"enabled", QJsonValue::Bool},
                                           {"breakpoint", QJsonValue::Bool}}) ||
            !validInt32(value.toObject(), "posX") || !validInt32(value.toObject(), "posY")) {
            qWarning() << "Invalid project module field type:" << path;
            return false;
        }
        QJsonObject legacyParams = value.toObject().value(QStringLiteral("params")).toObject();
        if (!migrateLegacyCalibrationParams(legacyParams,
                                            value.toObject().value(QStringLiteral("moduleId")).toString())) {
            qWarning() << "Invalid legacy calibration parameters:" << path;
            return false;
        }
    }
    for (const QJsonValue& value : json.value(QStringLiteral("connections")).toArray()) {
        if (!validTypes(value.toObject(), {{"fromOutput", QJsonValue::Double},
                                           {"toInput", QJsonValue::Double},
                                           {"fromPort", QJsonValue::String},
                                           {"toPort", QJsonValue::String},
                                           {"edgeType", QJsonValue::String}}) ||
            !validInt32(value.toObject(), "fromOutput") || !validInt32(value.toObject(), "toInput")) {
            qWarning() << "Invalid project connection field type:" << path;
            return false;
        }
    }
    for (const QJsonValue& value : json.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject flow = value.toObject();
        if (!validTypes(flow,
                        {{"id", QJsonValue::String}, {"name", QJsonValue::String}, {"nodes", QJsonValue::Array}})) {
            qWarning() << "Invalid project flow field type:" << path;
            return false;
        }
        for (const QJsonValue& node : flow.value(QStringLiteral("nodes")).toArray()) {
            if (!node.isString()) {
                qWarning() << "Invalid project flow node:" << path;
                return false;
            }
        }
    }
    for (const QJsonValue& cameras :
         {json.value(QStringLiteral("cameras")), resources.value(QStringLiteral("cameras"))}) {
        for (const QJsonValue& value : cameras.toArray()) {
            if (!validTypes(value.toObject(), {{"id", QJsonValue::String},
                                               {"type", QJsonValue::String},
                                               {"serialNumber", QJsonValue::String},
                                               {"config", QJsonValue::Object}})) {
                qWarning() << "Invalid project camera field type:" << path;
                return false;
            }
        }
    }
    for (const QJsonValue& sources :
         {json.value(QStringLiteral("dataSources")), resources.value(QStringLiteral("dataSources"))}) {
        for (const QJsonValue& value : sources.toArray()) {
            const QJsonObject source = value.toObject();
            const QJsonValue importTime = source.value(QStringLiteral("importTime"));
            bool validImportTime = importTime.isUndefined();
            if (importTime.isString()) {
                bool ok = false;
                importTime.toString().toLongLong(&ok);
                validImportTime = ok;
            } else if (importTime.isDouble()) {
                const double number = importTime.toDouble();
                validImportTime = std::isfinite(number) && std::trunc(number) == number &&
                                  number >= -std::ldexp(1.0, 63) && number < std::ldexp(1.0, 63);
            }
            if (!validTypes(source, {{"id", QJsonValue::String},
                                     {"name", QJsonValue::String},
                                     {"filePath", QJsonValue::String},
                                     {"type", QJsonValue::String},
                                     {"metadata", QJsonValue::Object}}) ||
                !validImportTime) {
                qWarning() << "Invalid project data source field type:" << path;
                return false;
            }
        }
    }
    const QJsonObject migration = json.value(QStringLiteral("migration")).toObject();
    const QJsonValue warnings = migration.value(QStringLiteral("warnings"));
    if (!validTypes(migration, {{"originalVersion", QJsonValue::String}, {"migratedAt", QJsonValue::String}}) ||
        !validDate(migration, "migratedAt") || (!warnings.isUndefined() && !warnings.isArray())) {
        qWarning() << "Invalid project migration field type:" << path;
        return false;
    }
    for (const QJsonValue& warning : warnings.toArray()) {
        if (!warning.isString()) {
            qWarning() << "Invalid project migration warning:" << path;
            return false;
        }
    }

    if (!fromJson(json)) {
        return false;
    }

    m_filePath = path;
    m_hasUnsavedChanges = false;

    qDebug() << "Project loaded from:" << path;
    return true;
}

void Project::setModified(bool modified) {
    if (m_hasUnsavedChanges != modified) {
        m_hasUnsavedChanges = modified;
        if (modified) {
            touch();
        }
        emit modifiedChanged(modified);
    }
}

void Project::touch() {
    m_modifiedTime = QDateTime::currentDateTime();
    m_hasUnsavedChanges = true;
}

} // namespace DeepLux
