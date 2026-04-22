#pragma once

#include "core/display/DisplayData.h"

// Forward declare HymsonVision3D types to avoid include dependencies
#ifdef DEEPLUX_HAS_HYMSON3D
namespace hymson3d {
namespace geometry {
class PointCloud;
}
}
#endif

namespace DeepLux {

/**
 * @brief 点云数据转换工具类
 *
 * 用于 DeepLux PointCloudData 与 HymsonVision3D PointCloud 之间的双向转换
 *
 * 注意: 当 DEEPLUX_HAS_HYMSON3D 未定义时,转换函数返回空数据
 *       完整的 HymsonVision3D 集成需要在构建时正确配置头文件路径
 */
class PointCloudConverter {
public:
    /**
     * @brief 将 DeepLux PointCloudData 转换为 HymsonVision3D PointCloud
     * @param data DeepLux点云数据
     * @return HymsonVision3D 点云指针 (nullptr if HymsonVision3D not available)
     */
#ifdef DEEPLUX_HAS_HYMSON3D
    static std::shared_ptr<hymson3d::geometry::PointCloud> toHymsonCloud(const PointCloudData& data);
#else
    static void* toHymsonCloud(const PointCloudData& data) {
        Q_UNUSED(data);
        return nullptr;
    }
#endif

    /**
     * @brief 将 HymsonVision3D PointCloud 转换为 DeepLux PointCloudData
     * @param cloud HymsonVision3D点云指针
     * @return DeepLux点云数据
     */
#ifdef DEEPLUX_HAS_HYMSON3D
    static PointCloudData fromHymsonCloud(const std::shared_ptr<hymson3d::geometry::PointCloud>& cloud);
#else
    static PointCloudData fromHymsonCloud(void* cloud) {
        Q_UNUSED(cloud);
        return PointCloudData();
    }
#endif
};

} // namespace DeepLux

// Implementation when HymsonVision3D is available
#ifdef DEEPLUX_HAS_HYMSON3D
#include <geometry/3D/PointCloud.h>

namespace DeepLux {

inline std::shared_ptr<hymson3d::geometry::PointCloud> PointCloudConverter::toHymsonCloud(const PointCloudData& data) {
    auto cloud = std::make_shared<hymson3d::geometry::PointCloud>();
    cloud->points_ = data.points;
    if (data.hasNormals()) {
        cloud->normals_ = data.normals;
    }
    if (data.hasColors()) {
        cloud->colors_ = data.colors;
    }
    if (data.hasLabels()) {
        cloud->labels_ = data.labels;
    }
    if (!data.intensities.empty()) {
        cloud->intensities_ = data.intensities;
    }
    return cloud;
}

inline PointCloudData PointCloudConverter::fromHymsonCloud(const std::shared_ptr<hymson3d::geometry::PointCloud>& cloud) {
    PointCloudData data;
    if (!cloud) return data;
    data.points = cloud->points_;
    data.normals = cloud->normals_;
    data.colors = cloud->colors_;
    data.labels = cloud->labels_;
    data.intensities = cloud->intensities_;
    return data;
}

} // namespace DeepLux
#endif
