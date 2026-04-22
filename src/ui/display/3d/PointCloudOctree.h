#pragma once

#include "OctreeNode.h"
#include "PointCloudGPUBuffer.h"
#include <QVector3D>
#include <QMatrix4x4>
#include <memory>
#include <vector>
#include <atomic>

namespace DeepLux {

class PointCloudData;

/**
 * @brief 点云八叉树
 *
 * 提供空间索引和 LOD 支持：
 * - 按节点存储点索引，支持 O(log n) 查询
 * - 叶子节点预计算 GPU 缓冲区
 * - 支持异步构建
 * - 支持 LOD 阈值设置（超过阈值才启用 LOD）
 */
class PointCloudOctree {
public:
    // 配置
    struct Config {
        size_t maxDepth = 8;              // 最大深度
        size_t pointsPerNode = 1000;      // 每个叶子节点的最大点数
        bool buildGpuBuffers = true;      // 是否预构建 GPU 缓冲区

        // LOD 阈值设置
        size_t lodThreshold = 50000;      // 超过此点数才启用 LOD（默认 5 万）
        bool forceLod = false;            // 强制启用 LOD（不考虑阈值）
    };

    PointCloudOctree(const Config& config);
    ~PointCloudOctree();

    // 构建
    void build(const PointCloudData& pointCloud);
    void buildAsync(const PointCloudData& pointCloud);
    void cancelBuild();
    bool isBuilt() const { return m_built.load(); }
    bool isBuilding() const { return m_building.load(); }

    // 是否应该使用 LOD（基于阈值判断）
    bool shouldUseLOD() const {
        return m_config.forceLod || m_pointCount > m_config.lodThreshold;
    }

    // 查询
    std::vector<size_t> queryRadius(const QVector3D& center, float radius) const;
    std::vector<size_t> queryBox(const QVector3D& min, const QVector3D& max) const;

    // 空间信息
    QVector3D origin() const { return m_origin; }
    float size() const { return m_size; }
    QVector3D center() const { return QVector3D(m_origin.x() + m_size * 0.5f,
                                                 m_origin.y() + m_size * 0.5f,
                                                 m_origin.z() + m_size * 0.5f); }

    // 节点访问
    std::shared_ptr<OctreeNode> root() const { return m_root; }

    // 统计
    size_t nodeCount() const { return m_nodeCount.load(); }
    size_t leafCount() const { return m_leafCount.load(); }
    size_t pointCount() const { return m_pointCount; }

    // 配置访问
    const Config& config() const { return m_config; }

    // 获取建议的 LOD 级别（基于点数量）
    int recommendedLODLevel() const;

private:
    void buildSync(const PointCloudData& pointCloud);
    void buildNode(std::shared_ptr<OctreeNode>& node,
                   const OctreeNodeInfo& info,
                   const PointCloudData& pointCloud,
                   const std::vector<size_t>& pointIndices);

    QVector3D computeAverageColor(const PointCloudData& pointCloud,
                                  const std::vector<size_t>& indices) const;

    void computeBounds(const PointCloudData& pointCloud);

    Config m_config;

    std::atomic<bool> m_built{false};
    std::atomic<bool> m_building{false};
    std::atomic<bool> m_cancelled{false};
    std::atomic<size_t> m_nodeCount{0};
    std::atomic<size_t> m_leafCount{0};

    QVector3D m_origin;
    float m_size = 0;
    size_t m_pointCount = 0;

    std::shared_ptr<OctreeNode> m_root;

    // 原始点云数据引用（用于查询）
    const PointCloudData* m_pointCloud = nullptr;
};

} // namespace DeepLux
