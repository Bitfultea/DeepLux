#include "Project.h"

#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace DeepLux {

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
    inst.note = json["note"].toString();
    inst.enabled = json["enabled"].toBool(true);      // 旧工程缺省为启用
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
    : QObject(parent), m_id(QUuid::createUuid().toString(QUuid::WithoutBraces)), m_name(tr("未命名项目")),
      m_created(QDateTime::currentDateTime()), m_modifiedTime(QDateTime::currentDateTime()) {}

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

void Project::addModule(const ModuleInstance& module) {
    m_modules.append(module);
    touch();
    emit moduleAdded(module);
}

void Project::removeModule(const QString& instanceId) {
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            m_modules.removeAt(i);
            touch();
            emit moduleRemoved(instanceId);
            return;
        }
    }
}

void Project::updateModule(const QString& instanceId, const ModuleInstance& module) {
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            m_modules[i] = module;
            touch();
            emit moduleUpdated(module);
            return;
        }
    }
}

bool Project::setModuleParam(const QString& instanceId, const QString& key, const QJsonValue& value) {
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            m_modules[i].params[key] = value;
            touch();
            emit moduleUpdated(m_modules[i]);
            return true;
        }
    }
    return false;
}

bool Project::moveModule(const QString& instanceId, int newIndex) {
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
    touch();
    return true;
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

void Project::addConnection(const ModuleConnection& conn) {
    m_connections.append(conn);
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

void Project::removeConnectionWithPorts(const QString& fromId, const QString& fromPort,
                                          const QString& toId, const QString& toPort) {
    for (int i = 0; i < m_connections.size(); i++) {
        const ModuleConnection& c = m_connections[i];
        if (c.fromModuleId == fromId && c.toModuleId == toId &&
            c.fromPort == fromPort && c.toPort == toPort) {
            m_connections.removeAt(i);
            touch();
            emit connectionRemoved(fromId, toId);
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
    touch();
    emit dataSourceAdded(ds);
}

void Project::removeDataSource(const QString& id) {
    for (int i = 0; i < m_dataSources.size(); i++) {
        if (m_dataSources[i].id == id) {
            m_dataSources.removeAt(i);
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

    return json;
}

bool Project::fromJson(const QJsonObject& json) {
    m_formatVersion = json["version"].toString(QStringLiteral("2.0"));
    m_id = json["id"].toString();
    m_name = json["name"].toString();
    m_created = QDateTime::fromString(json["created"].toString(), Qt::ISODate);
    m_modifiedTime = QDateTime::fromString(json["modified"].toString(), Qt::ISODate);

    m_modules.clear();
    QJsonArray modulesArray = json["modules"].toArray();
    for (const auto& val : modulesArray) {
        m_modules.append(ModuleInstance::fromJson(val.toObject()));
    }

    m_connections.clear();
    QJsonArray connectionsArray = json["connections"].toArray();
    for (const auto& val : connectionsArray) {
        m_connections.append(ModuleConnection::fromJson(val.toObject()));
    }

    // 3.0 资源在 resources 下；2.0 在顶层 cameras/dataSources
    QJsonObject resources = json["resources"].toObject();
    QJsonArray camerasArray = resources.contains("cameras") ? resources["cameras"].toArray() : json["cameras"].toArray();
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

    m_hasUnsavedChanges = false;
    return true;
}

bool Project::save(const QString& path) {
    QString savePath = path.isEmpty() ? m_filePath : path;
    if (savePath.isEmpty()) {
        qWarning() << "No file path specified";
        return false;
    }

    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Cannot open file for writing:" << savePath;
        return false;
    }

    QJsonDocument doc(toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    m_filePath = savePath;
    m_hasUnsavedChanges = false;
    emit modifiedChanged(false);

    qDebug() << "Project saved to:" << savePath;
    return true;
}

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

    if (!fromJson(doc.object())) {
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
