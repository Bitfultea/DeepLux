#include "ConfigManager.h"
#include "../platform/PathUtils.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>

namespace DeepLux {

ConfigManager& ConfigManager::instance()
{
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager()
    : m_configPath(PathUtils::configPath() + "/config.json")
{
    setDefaults();
}

ConfigManager::~ConfigManager()
{
    if (m_modified) {
        save();
    }
}

bool ConfigManager::initialize()
{
    if (m_initialized) return true;
    
    if (load()) {
        m_initialized = true;
        return true;
    }
    
    m_config = m_defaults;
    if (save()) {
        m_initialized = true;
        return true;
    }
    return false;
}

bool ConfigManager::save()
{
    QFile file(m_configPath);
    if (!file.open(QIODevice::WriteOnly)) return false;
    
    QJsonDocument doc(m_config);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    
    m_modified = false;
    emit configSaved();
    return true;
}

bool ConfigManager::load()
{
    QFile file(m_configPath);
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly)) return false;
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) return false;
    
    m_config = doc.object();
    emit configLoaded();
    return true;
}

void ConfigManager::resetToDefaults()
{
    m_config = m_defaults;
    m_modified = true;
    save();
}

void ConfigManager::setDefaults()
{
    QJsonObject app;
    app["language"] = "zh_CN";
    app["theme"] = "dark";
    app["autoSave"] = true;
    app["autoSaveInterval"] = 300;
    m_defaults["application"] = app;
    
    QJsonObject ui;
    ui["showToolbar"] = true;
    ui["showStatusBar"] = true;
    ui["windowWidth"] = 1600;
    ui["windowHeight"] = 900;
    m_defaults["ui"] = ui;
    
    QJsonObject plugins;
    plugins["autoLoad"] = true;
    QJsonArray pluginPaths;
    pluginPaths.append(PathUtils::pluginPath());
    plugins["pluginPaths"] = pluginPaths;
    m_defaults["plugins"] = plugins;
    
    QJsonObject camera;
    camera["defaultExposure"] = 10000.0;
    camera["defaultGain"] = 1.0;
    m_defaults["camera"] = camera;
    
    m_config = m_defaults;
}

// 单键操作
void ConfigManager::setValue(const QString& key, const QVariant& value)
{
    m_config[key] = QJsonValue::fromVariant(value);
    m_modified = true;
    emit valueChanged(key, value);
}

QVariant ConfigManager::value(const QString& key, const QVariant& defaultValue) const
{
    return m_config.contains(key) ? m_config.value(key).toVariant() : defaultValue;
}

QString ConfigManager::string(const QString& key, const QString& defaultValue) const
{
    return value(key, defaultValue).toString();
}

int ConfigManager::intVal(const QString& key, int defaultValue) const
{
    return value(key, defaultValue).toInt();
}

double ConfigManager::doubleVal(const QString& key, double defaultValue) const
{
    return value(key, defaultValue).toDouble();
}

bool ConfigManager::boolVal(const QString& key, bool defaultValue) const
{
    return value(key, defaultValue).toBool();
}

bool ConfigManager::contains(const QString& key) const
{
    return m_config.contains(key);
}

void ConfigManager::remove(const QString& key)
{
    m_config.remove(key);
    m_modified = true;
}

// 分组操作
void ConfigManager::setGroupValue(const QString& group, const QString& key, const QVariant& value)
{
    QJsonObject groupObj = m_config.value(group).toObject();
    groupObj[key] = QJsonValue::fromVariant(value);
    m_config[group] = groupObj;
    m_modified = true;
}

QVariant ConfigManager::groupValue(const QString& group, const QString& key, const QVariant& defaultValue) const
{
    QJsonObject groupObj = m_config.value(group).toObject();
    if (groupObj.contains(key)) return groupObj.value(key).toVariant();
    
    QJsonObject defaultGroup = m_defaults.value(group).toObject();
    if (defaultGroup.contains(key)) return defaultGroup.value(key).toVariant();
    
    return defaultValue;
}

QString ConfigManager::groupString(const QString& group, const QString& key, const QString& defaultValue) const
{
    return groupValue(group, key, defaultValue).toString();
}

int ConfigManager::groupInt(const QString& group, const QString& key, int defaultValue) const
{
    return groupValue(group, key, defaultValue).toInt();
}

double ConfigManager::groupDouble(const QString& group, const QString& key, double defaultValue) const
{
    return groupValue(group, key, defaultValue).toDouble();
}

bool ConfigManager::groupBool(const QString& group, const QString& key, bool defaultValue) const
{
    return groupValue(group, key, defaultValue).toBool();
}

bool ConfigManager::containsGroupKey(const QString& group, const QString& key) const
{
    return m_config.value(group).toObject().contains(key);
}

void ConfigManager::removeGroupKey(const QString& group, const QString& key)
{
    QJsonObject groupObj = m_config.value(group).toObject();
    groupObj.remove(key);
    m_config[group] = groupObj;
    m_modified = true;
}

// 分组管理
QJsonObject ConfigManager::getGroup(const QString& groupName) const
{
    return m_config.value(groupName).toObject();
}

void ConfigManager::setGroup(const QString& groupName, const QJsonObject& values)
{
    m_config[groupName] = values;
    m_modified = true;
}

QStringList ConfigManager::groups() const
{
    return m_config.keys();
}

} // namespace DeepLux
