#pragma once

#include "DataSource.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <memory>
#include <optional>

namespace DeepLux {

/**
 * @brief 模块连接
 *
 * 格式 3.0 使用稳定字符串端口 ID（fromPort/toPort）。
 * fromOutput/toInput 保留为旧 2.0 整数序号，仅用于迁移与兼容。
 */
struct ModuleConnection {
    QString fromModuleId;
    QString toModuleId;
    int fromOutput = 0;
    int toInput = 0;
    QString fromPort; // 3.0: 源端口 ID（如 "image"）
    QString toPort;   // 3.0: 目标端口 ID
    QString edgeType; // 3.0: "data" 或 "control"

    QJsonObject toJson() const;
    static ModuleConnection fromJson(const QJsonObject& json);
};

/**
 * @brief 流程（3.0）：一个工程可含多个流程，现阶段至少 main
 */
struct ProjectFlow {
    QString id = QStringLiteral("main");
    QString name;
    QStringList nodeIds; // 该流程包含的模块实例 ID（有序）

    QJsonObject toJson() const;
    static ProjectFlow fromJson(const QJsonObject& json);
};

/**
 * @brief 迁移记录（3.0）
 */
struct MigrationRecord {
    QString originalVersion;
    QDateTime migratedAt;
    QStringList warnings; // 自动修复/警告/阻塞项

    QJsonObject toJson() const;
    static MigrationRecord fromJson(const QJsonObject& json);
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
    QString note;            // 3.0: 备注
    bool enabled = true;     // 3.0: 启用状态
    bool breakpoint = false; // 3.0: 断点

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
class Project : public QObject {
    Q_OBJECT

public:
    explicit Project(QObject* parent = nullptr);
    ~Project() override;

    // 基本信息
    QString id() const {
        return m_id;
    }
    QString name() const {
        return m_name;
    }
    void setName(const QString& name);

    QString filePath() const {
        return m_filePath;
    }
    void setFilePath(const QString& path);

    QDateTime created() const {
        return m_created;
    }
    QDateTime modifiedTime() const {
        return m_modifiedTime;
    }

    // 模块管理
    QList<ModuleInstance> modules() const {
        return m_modules;
    }
    void addModule(const ModuleInstance& module);
    void removeModule(const QString& instanceId);
    void updateModule(const QString& instanceId, const ModuleInstance& module);
    bool setModuleParam(const QString& instanceId, const QString& key, const QJsonValue& value);
    bool moveModule(const QString& instanceId, int newIndex);
    /// 返回指向 QList 内部元素的指针——调用方不得跨 addModule/removeModule 持有此指针
    ModuleInstance* findModule(const QString& instanceId);
    /// 按值返回模块实例，适合跨 undo/redo、add/remove 等模型变更边界使用
    std::optional<ModuleInstance> moduleById(const QString& instanceId) const;

    // 连接管理
    QList<ModuleConnection> connections() const {
        return m_connections;
    }
    void addConnection(const ModuleConnection& conn);
    void removeConnection(const QString& fromId, const QString& toId);
    void removeConnectionWithPorts(const QString& fromId, const QString& fromPort, const QString& toId,
                                   const QString& toPort);
    void setConnections(const QList<ModuleConnection>& conns);

    // 相机配置
    QList<CameraConfig> cameras() const {
        return m_cameras;
    }
    void addCamera(const CameraConfig& camera);
    void removeCamera(const QString& cameraId);
    const CameraConfig* findCamera(const QString& cameraId) const;

    // 数据源管理
    QList<DataSource> dataSources() const {
        return m_dataSources;
    }
    void addDataSource(const DataSource& ds);
    void removeDataSource(const QString& id);
    /// 按值返回拷贝以避免悬垂指针——DataSource 是轻量结构体，拷贝开销可忽略
    std::optional<DataSource> findDataSource(const QString& id) const;

    // 流程（3.0）
    QList<ProjectFlow> flows() const {
        return m_flows;
    }
    void setFlows(const QList<ProjectFlow>& flows);
    ProjectFlow flow(const QString& flowId) const;

    // 参数覆盖集合（3.0 recipes）
    QJsonObject recipes() const {
        return m_recipes;
    }
    void setRecipes(const QJsonObject& recipes);

    // 运行看板配置（3.0 dashboard）
    QJsonObject dashboard() const {
        return m_dashboard;
    }
    void setDashboard(const QJsonObject& dashboard);

    // 迁移记录（3.0）
    MigrationRecord migration() const {
        return m_migration;
    }
    void setMigration(const MigrationRecord& rec);

    // 格式版本
    QString formatVersion() const {
        return m_formatVersion;
    }
    void setFormatVersion(const QString& v) {
        m_formatVersion = v;
    }

    // 序列化
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& json);

    // 文件操作
    bool save(const QString& path = QString());
    bool load(const QString& path);

    bool isModified() const {
        return m_hasUnsavedChanges;
    }
    void setModified(bool modified);

signals:
    void nameChanged(const QString& name);
    void moduleAdded(const ModuleInstance& module);
    void moduleUpdated(const ModuleInstance& module);
    void moduleRemoved(const QString& instanceId);
    void connectionAdded(const ModuleConnection& conn);
    void connectionRemoved(const QString& fromId, const QString& toId);
    void connectionRemovedWithPorts(const QString& fromId, const QString& fromPort, const QString& toId,
                                    const QString& toPort);
    void dataSourceAdded(const DataSource& ds);
    void dataSourceRemoved(const QString& id);
    void modifiedChanged(bool modified);

private:
    void touch(); // 更新修改时间

    QString m_id;
    QString m_name;
    QString m_filePath;
    QDateTime m_created;
    QDateTime m_modifiedTime;
    bool m_hasUnsavedChanges = false;

    QList<ModuleInstance> m_modules;
    QList<ModuleConnection> m_connections;
    QList<CameraConfig> m_cameras;
    QList<DataSource> m_dataSources;

    // 3.0 新增
    QString m_formatVersion = QStringLiteral("3.0");
    QList<ProjectFlow> m_flows;
    QJsonObject m_recipes;
    QJsonObject m_dashboard;
    MigrationRecord m_migration;
};

} // namespace DeepLux
