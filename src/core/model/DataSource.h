#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

namespace DeepLux {

/**
 * @brief 数据源描述
 *
 * 不直接存储大体积数据（如 PointCloudData），只记录文件路径和元数据。
 * 视口显示时按需从文件加载，项目持久化时也仅保存路径和元数据。
 */
struct DataSource {
    QString id;            ///< UUID
    QString name;          ///< 显示名称（通常为文件名）
    QString filePath;      ///< 原始文件绝对路径
    QString type;          ///< "image" | "pointcloud"
    QVariantMap metadata;  ///< 附加元数据（点数、包围盒、文件大小等）
    qint64 importTime = 0; ///< 导入时间戳（毫秒）

    bool isValid() const {
        return !id.isEmpty() && !filePath.isEmpty();
    }
    bool isImage() const {
        return type == "image";
    }
    bool isPointCloud() const {
        return type == "pointcloud";
    }

    QJsonObject toJson() const;
    static DataSource fromJson(const QJsonObject& json);
};

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::DataSource)
