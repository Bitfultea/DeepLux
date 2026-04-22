#pragma once

#include <QObject>
#include <QJsonObject>
#include <QVariant>

namespace DeepLux {

class ConfigManager : public QObject
{
    Q_OBJECT

public:
    static ConfigManager& instance();
    
    bool initialize();
    bool save();
    void resetToDefaults();
    
    // 单键操作
    void setValue(const QString& key, const QVariant& value);
    QVariant value(const QString& key, const QVariant& defaultValue = QVariant()) const;
    QString string(const QString& key, const QString& defaultValue = QString()) const;
    int intVal(const QString& key, int defaultValue = 0) const;
    double doubleVal(const QString& key, double defaultValue = 0.0) const;
    bool boolVal(const QString& key, bool defaultValue = false) const;
    bool contains(const QString& key) const;
    void remove(const QString& key);
    
    // 分组操作
    void setGroupValue(const QString& group, const QString& key, const QVariant& value);
    QVariant groupValue(const QString& group, const QString& key, const QVariant& defaultValue = QVariant()) const;
    QString groupString(const QString& group, const QString& key, const QString& defaultValue = QString()) const;
    int groupInt(const QString& group, const QString& key, int defaultValue = 0) const;
    double groupDouble(const QString& group, const QString& key, double defaultValue = 0.0) const;
    bool groupBool(const QString& group, const QString& key, bool defaultValue = false) const;
    bool containsGroupKey(const QString& group, const QString& key) const;
    void removeGroupKey(const QString& group, const QString& key);
    
    // 分组管理
    QJsonObject getGroup(const QString& groupName) const;
    void setGroup(const QString& groupName, const QJsonObject& values);
    QStringList groups() const;
    
    QString configFilePath() const { return m_configPath; }
    bool isInitialized() const { return m_initialized; }

signals:
    void valueChanged(const QString& key, const QVariant& value);
    void configSaved();
    void configLoaded();

private:
    ConfigManager();
    ~ConfigManager();
    
    bool load();
    void setDefaults();

    QString m_configPath;
    QJsonObject m_config;
    QJsonObject m_defaults;
    bool m_initialized = false;
    bool m_modified = false;
};

} // namespace DeepLux
