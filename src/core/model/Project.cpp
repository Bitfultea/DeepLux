#include "Project.h"
#include <QUuid>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>

namespace DeepLux {

// ========== ModuleConnection ==========

QJsonObject ModuleConnection::toJson() const
{
    QJsonObject json;
    json["fromModuleId"] = fromModuleId;
    json["toModuleId"] = toModuleId;
    json["fromOutput"] = fromOutput;
    json["toInput"] = toInput;
    return json;
}

ModuleConnection ModuleConnection::fromJson(const QJsonObject& json)
{
    ModuleConnection conn;
    conn.fromModuleId = json["fromModuleId"].toString();
    conn.toModuleId = json["toModuleId"].toString();
    conn.fromOutput = json["fromOutput"].toInt(0);
    conn.toInput = json["toInput"].toInt(0);
    return conn;
}

// ========== ModuleInstance ==========

QJsonObject ModuleInstance::toJson() const
{
    QJsonObject json;
    json["id"] = id;
    json["moduleId"] = moduleId;
    json["name"] = name;
    json["posX"] = posX;
    json["posY"] = posY;
    json["params"] = params;
    return json;
}

ModuleInstance ModuleInstance::fromJson(const QJsonObject& json)
{
    ModuleInstance inst;
    inst.id = json["id"].toString();
    inst.moduleId = json["moduleId"].toString();
    inst.name = json["name"].toString();
    inst.posX = json["posX"].toInt(0);
    inst.posY = json["posY"].toInt(0);
    inst.params = json["params"].toObject();
    return inst;
}

// ========== CameraConfig ==========

QJsonObject CameraConfig::toJson() const
{
    QJsonObject json;
    json["id"] = id;
    json["type"] = type;
    json["serialNumber"] = serialNumber;
    json["config"] = config;
    return json;
}

CameraConfig CameraConfig::fromJson(const QJsonObject& json)
{
    CameraConfig cfg;
    cfg.id = json["id"].toString();
    cfg.type = json["type"].toString();
    cfg.serialNumber = json["serialNumber"].toString();
    cfg.config = json["config"].toObject();
    return cfg;
}

// ========== Project ==========

Project::Project(QObject* parent)
    : QObject(parent)
    , m_id(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_name(tr("未命名项目"))
    , m_created(QDateTime::currentDateTime())
    , m_modifiedTime(QDateTime::currentDateTime())
{
}

Project::~Project()
{
}

void Project::setName(const QString& name)
{
    if (m_name != name) {
        m_name = name;
        touch();
        emit nameChanged(name);
    }
}

void Project::setFilePath(const QString& path)
{
    m_filePath = path;
}

void Project::addModule(const ModuleInstance& module)
{
    m_modules.append(module);
    touch();
    emit moduleAdded(module);
}

void Project::removeModule(const QString& instanceId)
{
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            m_modules.removeAt(i);
            touch();
            emit moduleRemoved(instanceId);
            return;
        }
    }
}

void Project::updateModule(const QString& instanceId, const ModuleInstance& module)
{
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            m_modules[i] = module;
            touch();
            return;
        }
    }
}

ModuleInstance* Project::findModule(const QString& instanceId)
{
    for (int i = 0; i < m_modules.size(); i++) {
        if (m_modules[i].id == instanceId) {
            return &m_modules[i];
        }
    }
    return nullptr;
}

void Project::addConnection(const ModuleConnection& conn)
{
    m_connections.append(conn);
    touch();
    emit connectionAdded(conn);
}

void Project::removeConnection(const QString& fromId, const QString& toId)
{
    for (int i = 0; i < m_connections.size(); i++) {
        if (m_connections[i].fromModuleId == fromId && 
            m_connections[i].toModuleId == toId) {
            m_connections.removeAt(i);
            touch();
            emit connectionRemoved(fromId, toId);
            return;
        }
    }
}

void Project::addCamera(const CameraConfig& camera)
{
    m_cameras.append(camera);
    touch();
}

void Project::removeCamera(const QString& cameraId)
{
    for (int i = 0; i < m_cameras.size(); i++) {
        if (m_cameras[i].id == cameraId) {
            m_cameras.removeAt(i);
            touch();
            return;
        }
    }
}

CameraConfig* Project::findCamera(const QString& cameraId)
{
    for (int i = 0; i < m_cameras.size(); i++) {
        if (m_cameras[i].id == cameraId) {
            return &m_cameras[i];
        }
    }
    return nullptr;
}

QJsonObject Project::toJson() const
{
    QJsonObject json;
    json["version"] = "2.0";
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
    
    // 相机
    QJsonArray camerasArray;
    for (const auto& camera : m_cameras) {
        camerasArray.append(camera.toJson());
    }
    json["cameras"] = camerasArray;
    
    return json;
}

bool Project::fromJson(const QJsonObject& json)
{
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
    
    m_cameras.clear();
    QJsonArray camerasArray = json["cameras"].toArray();
    for (const auto& val : camerasArray) {
        m_cameras.append(CameraConfig::fromJson(val.toObject()));
    }
    
    m_hasUnsavedChanges = false;
    return true;
}

bool Project::save(const QString& path)
{
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

bool Project::load(const QString& path)
{
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

void Project::setModified(bool modified)
{
    if (m_hasUnsavedChanges != modified) {
        m_hasUnsavedChanges = modified;
        if (modified) {
            touch();
        }
        emit modifiedChanged(modified);
    }
}

void Project::touch()
{
    m_modifiedTime = QDateTime::currentDateTime();
    m_hasUnsavedChanges = true;
}

} // namespace DeepLux
