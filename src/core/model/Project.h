#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QDateTime>
#include <QList>
#include <memory>

namespace DeepLux {

/**
 * @brief 模块连接
 */
struct ModuleConnection {
    QString fromModuleId;
    QString toModuleId;
    int fromOutput = 0;
    int toInput = 0;

    QJsonObject toJson() const;
    static ModuleConnection fromJson(const QJsonObject& json);
};

/**
 * @brief 模块实例
 */
struct ModuleInstance {
    QString id;
    QString moduleId;
    QString name;
    int posX = 0;
    int posY = 0;
    QJsonObject params;

    QJsonObject toJson() const;
    static ModuleInstance fromJson(const QJsonObject& json);
};

/**
 * @brief 相机配置
 */
struct CameraConfig {
    QString id;
    QString type;
    QString serialNumber;
    QJsonObject config;

    QJsonObject toJson() const;
    static CameraConfig fromJson(const QJsonObject& json);
};

/**
 * @brief 项目类
 */
class Project : public QObject
{
    Q_OBJECT

public:
    explicit Project(QObject* parent = nullptr);
    ~Project() override;

    // 基本信息
    QString id() const { return m_id; }
    QString name() const { return m_name; }
    void setName(const QString& name);
    
    QString filePath() const { return m_filePath; }
    void setFilePath(const QString& path);
    
    QDateTime created() const { return m_created; }
    QDateTime modifiedTime() const { return m_modifiedTime; }

    // 模块管理
    QList<ModuleInstance> modules() const { return m_modules; }
    void addModule(const ModuleInstance& module);
    void removeModule(const QString& instanceId);
    void updateModule(const QString& instanceId, const ModuleInstance& module);
    ModuleInstance* findModule(const QString& instanceId);

    // 连接管理
    QList<ModuleConnection> connections() const { return m_connections; }
    void addConnection(const ModuleConnection& conn);
    void removeConnection(const QString& fromId, const QString& toId);

    // 相机配置
    QList<CameraConfig> cameras() const { return m_cameras; }
    void addCamera(const CameraConfig& camera);
    void removeCamera(const QString& cameraId);
    CameraConfig* findCamera(const QString& cameraId);

    // 序列化
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& json);

    // 文件操作
    bool save(const QString& path = QString());
    bool load(const QString& path);
    
    bool isModified() const { return m_hasUnsavedChanges; }
    void setModified(bool modified);

signals:
    void nameChanged(const QString& name);
    void moduleAdded(const ModuleInstance& module);
    void moduleRemoved(const QString& instanceId);
    void connectionAdded(const ModuleConnection& conn);
    void connectionRemoved(const QString& fromId, const QString& toId);
    void modifiedChanged(bool modified);

private:
    void touch();  // 更新修改时间

    QString m_id;
    QString m_name;
    QString m_filePath;
    QDateTime m_created;
    QDateTime m_modifiedTime;
    bool m_hasUnsavedChanges = false;
    
    QList<ModuleInstance> m_modules;
    QList<ModuleConnection> m_connections;
    QList<CameraConfig> m_cameras;
};

} // namespace DeepLux
