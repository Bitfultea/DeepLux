#pragma once

#include <variant>
#include <vector>
#include <QString>
#include <QVariantMap>
#include <QMetaType>

// Eigen for 3D math
#include <Eigen/Core>

// Must include ImageData to make std::variant work (requires complete types)
#include "model/ImageData.h"

namespace DeepLux {

/**
 * @brief 3D点云数据结构
 *
 * 使用 Eigen::Vector3d 存储点坐标，与 HymsonVision3D 兼容
 */
struct PointCloudData {
    // 点坐标
    std::vector<Eigen::Vector3d> points;
    // 法向量
    std::vector<Eigen::Vector3d> normals;
    // 颜色 (RGB)
    std::vector<Eigen::Vector3d> colors;
    // 标签 (用于缺陷标注)
    std::vector<int> labels;
    // 强度
    std::vector<float> intensities;

    bool isEmpty() const { return points.empty(); }
    size_t size() const { return points.size(); }

    // 清空数据
    void clear() {
        points.clear();
        normals.clear();
        colors.clear();
        labels.clear();
        intensities.clear();
    }

    // 检查是否有法向量
    bool hasNormals() const { return normals.size() == points.size(); }
    // 检查是否有颜色
    bool hasColors() const { return colors.size() == points.size(); }
    // 检查是否有标签
    bool hasLabels() const { return labels.size() == points.size(); }
};

/**
 * @brief Placeholder for 3D mesh data (Future Phase)
 */
struct MeshData {
    // To be defined when mesh support is needed
    // Will contain vertices and faces
    bool isEmpty() const { return true; }
};

/**
 * @brief Display data using std::variant for type-safe union
 *
 * Phase 1: ImageData and PointCloudData supported
 * Future: MeshData can be added
 *
 * Usage:
 *   DisplayData data;
 *   if (auto* img = std::get_if<ImageData>(&data.variant())) {
 *       // Use img
 *   }
 */
struct DisplayData {
    using Variant = std::variant<
        std::monostate,    // Empty/invalid state
        ImageData,         // 2D image data
        PointCloudData,    // 3D point cloud (Phase 1)
        MeshData           // 3D mesh data (Future)
    >;

    DisplayData() : m_variant(std::monostate{}) {}

    // Construct from ImageData
    explicit DisplayData(const ImageData& img);

    // Check if valid
    bool isValid() const {
        return !std::holds_alternative<std::monostate>(m_variant);
    }

    // Get the underlying variant
    const Variant& variant() const { return m_variant; }
    Variant& variant() { return m_variant; }

    // Convenience accessors - returns nullptr if wrong type
    const ImageData* imageData() const {
        return std::get_if<ImageData>(&m_variant);
    }

    ImageData* imageData() {
        return std::get_if<ImageData>(&m_variant);
    }

    // PointCloudData accessors
    const PointCloudData* pointCloudData() const {
        return std::get_if<PointCloudData>(&m_variant);
    }

    PointCloudData* pointCloudData() {
        return std::get_if<PointCloudData>(&m_variant);
    }

    // Metadata accessors
    const QString& viewportId() const { return m_viewportId; }
    void setViewportId(const QString& id) { m_viewportId = id; }

    qint64 timestamp() const { return m_timestamp; }
    void setTimestamp(qint64 ts) { m_timestamp = ts; }

    const QVariantMap& metadata() const { return m_metadata; }
    QVariantMap& metadata() { return m_metadata; }
    void setMetadata(const QVariantMap& meta) { m_metadata = meta; }

private:
    Variant m_variant;
    QString m_viewportId;      // Target viewport ID (empty = auto)
    qint64 m_timestamp = 0;    // Timestamp when data was created
    QVariantMap m_metadata;    // Additional metadata
};

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::DisplayData)
Q_DECLARE_METATYPE(DeepLux::PointCloudData)
Q_DECLARE_METATYPE(DeepLux::MeshData)