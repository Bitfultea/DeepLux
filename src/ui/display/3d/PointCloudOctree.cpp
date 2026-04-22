#include "PointCloudOctree.h"
#include "PointCloudGPUBuffer.h"
#include "core/display/DisplayData.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <future>

namespace DeepLux {

PointCloudOctree::PointCloudOctree(const Config& config)
    : m_config(config)
{
}

PointCloudOctree::~PointCloudOctree() {
    cancelBuild();
}

void PointCloudOctree::build(const PointCloudData& pointCloud) {
    m_building.store(true);
    m_built.store(false);

    // 保存原始点云引用
    m_pointCloud = &pointCloud;

    computeBounds(pointCloud);
    m_pointCount = pointCloud.points.size();

    // 分配点索引
    std::vector<size_t> allIndices(m_pointCount);
    std::iota(allIndices.begin(), allIndices.end(), 0);

    // 如果点数量小于阈值，不构建八叉树，直接创建单叶子节点
    if (!shouldUseLOD()) {
        OctreeNodeInfo rootInfo;
        rootInfo.origin = m_origin;
        rootInfo.size = m_size;
        rootInfo.depth = 0;
        rootInfo.child_index = 0;

        auto leaf = std::make_shared<OctreeLeafNode>();
        leaf->setInfo(rootInfo);
        leaf->indices = allIndices;
        leaf->color = computeAverageColor(pointCloud, allIndices);

        if (m_config.buildGpuBuffers && !allIndices.empty()) {
            leaf->gpuBuffer = std::make_unique<PointCloudGPUBuffer>();
            leaf->gpuBuffer->fromPointCloudDataWithIndices(pointCloud, allIndices);
        }

        leaf->dirty = true;
        m_root = leaf;
        m_nodeCount.store(1);
        m_leafCount.store(1);
    } else {
        // 初始化根节点
        OctreeNodeInfo rootInfo;
        rootInfo.origin = m_origin;
        rootInfo.size = m_size;
        rootInfo.depth = 0;
        rootInfo.child_index = 0;

        m_root = std::make_shared<OctreeInternalNode>();

        // 递归构建
        m_nodeCount.store(0);
        m_leafCount.store(0);
        buildNode(m_root, rootInfo, pointCloud, allIndices);
    }

    m_built.store(true);
    m_building.store(false);
}

void PointCloudOctree::buildAsync(const PointCloudData& pointCloud) {
    if (m_building.load()) {
        return;  // 已经在构建中
    }

    std::thread builder([this, pointCloud]() {
        build(pointCloud);
    });
    builder.detach();
}

void PointCloudOctree::cancelBuild() {
    m_cancelled.store(true);
}

int PointCloudOctree::recommendedLODLevel() const {
    // 基于点数量建议 LOD 级别
    // 点数越少，深度可以越深
    if (m_pointCount < m_config.lodThreshold) {
        return 0;  // 不需要 LOD，全部显示
    }

    // 计算推荐的深度级别
    // 每个级别的点数约为 pointsPerNode * 8^level
    float log8 = std::log(static_cast<float>(m_pointCount) / static_cast<float>(m_config.pointsPerNode))
                   / std::log(8.0f);
    int level = static_cast<int>(std::ceil(log8));
    return std::min(level, static_cast<int>(m_config.maxDepth));
}

void PointCloudOctree::buildNode(std::shared_ptr<OctreeNode>& node,
                                 const OctreeNodeInfo& info,
                                 const PointCloudData& pointCloud,
                                 const std::vector<size_t>& pointIndices) {
    if (m_cancelled.load()) {
        return;
    }

    m_nodeCount.fetch_add(1);

    // 叶子判断：点数太少或达到最大深度
    if (pointIndices.size() <= m_config.pointsPerNode ||
        info.depth >= m_config.maxDepth) {

        auto leaf = std::make_shared<OctreeLeafNode>();
        leaf->setInfo(info);
        leaf->indices = pointIndices;
        leaf->color = computeAverageColor(pointCloud, leaf->indices);

        if (m_config.buildGpuBuffers && !pointIndices.empty()) {
            leaf->gpuBuffer = std::make_unique<PointCloudGPUBuffer>();
            leaf->gpuBuffer->fromPointCloudDataWithIndices(pointCloud, leaf->indices);
        }

        leaf->dirty = true;
        node = leaf;
        m_leafCount.fetch_add(1);
        return;
    }

    // 内部节点：划分点集到 8 个子节点
    auto internal = std::make_shared<OctreeInternalNode>();
    internal->setInfo(info);

    std::array<std::vector<size_t>, 8> childIndices;

    float midX = info.origin.x() + info.size * 0.5f;
    float midY = info.origin.y() + info.size * 0.5f;
    float midZ = info.origin.z() + info.size * 0.5f;

    for (size_t idx : pointIndices) {
        const auto& p = pointCloud.points[idx];
        int x = p.x() >= midX ? 1 : 0;
        int y = p.y() >= midY ? 1 : 0;
        int z = p.z() >= midZ ? 1 : 0;
        int child = x + y * 2 + z * 4;
        childIndices[child].push_back(idx);
    }

    // 创建子节点
    float childSize = info.size * 0.5f;

    for (int i = 0; i < 8; ++i) {
        if (childIndices[i].empty()) {
            continue;
        }

        OctreeNodeInfo childInfo;
        childInfo.depth = info.depth + 1;
        childInfo.child_index = i;
        childInfo.size = childSize;
        childInfo.origin = QVector3D(
            info.origin.x() + ((i & 1) ? childSize : 0),
            info.origin.y() + ((i & 2) ? childSize : 0),
            info.origin.z() + ((i & 4) ? childSize : 0)
        );

        buildNode(internal->children[i], childInfo, pointCloud, childIndices[i]);
    }

    // 汇总子节点索引（用于父节点的聚合查询）
    for (auto& child : internal->children) {
        if (child && child->isLeaf()) {
            auto* leaf = static_cast<OctreeLeafNode*>(child.get());
            internal->indices.insert(internal->indices.end(),
                                   leaf->indices.begin(),
                                   leaf->indices.end());
        }
    }

    node = internal;
}

QVector3D PointCloudOctree::computeAverageColor(const PointCloudData& pointCloud,
                                               const std::vector<size_t>& indices) const {
    if (indices.empty() || !pointCloud.hasColors()) {
        return QVector3D(1.0f, 1.0f, 1.0f);
    }

    float sumR = 0, sumG = 0, sumB = 0;
    for (size_t idx : indices) {
        const auto& c = pointCloud.colors[idx];
        sumR += c.x();
        sumG += c.y();
        sumB += c.z();
    }

    float n = static_cast<float>(indices.size());
    return QVector3D(sumR / n, sumG / n, sumB / n);
}

void PointCloudOctree::computeBounds(const PointCloudData& pointCloud) {
    if (pointCloud.points.empty()) {
        m_origin = QVector3D(0, 0, 0);
        m_size = 1.0f;
        return;
    }

    // 计算包围盒
    float minX = pointCloud.points[0].x();
    float minY = pointCloud.points[0].y();
    float minZ = pointCloud.points[0].z();
    float maxX = minX, maxY = minY, maxZ = minZ;

    for (const auto& p : pointCloud.points) {
        minX = std::min(minX, static_cast<float>(p.x()));
        minY = std::min(minY, static_cast<float>(p.y()));
        minZ = std::min(minZ, static_cast<float>(p.z()));
        maxX = std::max(maxX, static_cast<float>(p.x()));
        maxY = std::max(maxY, static_cast<float>(p.y()));
        maxZ = std::max(maxZ, static_cast<float>(p.z()));
    }

    // 扩展包围盒为正方体
    float maxExtent = std::max({maxX - minX, maxY - minY, maxZ - minZ});
    float margin = maxExtent * 0.1f;  // 10% margin

    m_origin = QVector3D(minX - margin, minY - margin, minZ - margin);
    m_size = maxExtent + margin * 2;

    // 确保 size 是 2 的幂次（方便八叉树划分）
    float sizePow = std::pow(2.0f, std::ceil(std::log2(m_size)));
    m_size = sizePow;
}

std::vector<size_t> PointCloudOctree::queryRadius(const QVector3D& center,
                                                    float radius) const {
    std::vector<size_t> result;

    if (!m_root) return result;

    float radiusSq = radius * radius;

    std::function<void(OctreeNode*, const OctreeNodeInfo&)> traverse;
    traverse = [&](OctreeNode* node, const OctreeNodeInfo& info) {
        if (!node) return;

        // 球体与立方体相交测试
        QVector3D closest = QVector3D(
            std::max(info.origin.x(), std::min(center.x(), info.origin.x() + info.size)),
            std::max(info.origin.y(), std::min(center.y(), info.origin.y() + info.size)),
            std::max(info.origin.z(), std::min(center.z(), info.origin.z() + info.size))
        );

        float dx = center.x() - closest.x();
        float dy = center.y() - closest.y();
        float dz = center.z() - closest.z();

        if (dx * dx + dy * dy + dz * dz > radiusSq) {
            return;  // 不相交，跳过
        }

        if (node->isLeaf()) {
            auto* leaf = static_cast<OctreeLeafNode*>(node);
            for (size_t idx : leaf->indices) {
                const auto& p = leaf->gpuBuffer ?
                    QVector3D(leaf->gpuBuffer->positions[idx * 3],
                              leaf->gpuBuffer->positions[idx * 3 + 1],
                              leaf->gpuBuffer->positions[idx * 3 + 2]) :
                    QVector3D(0, 0, 0);
                float dx = center.x() - p.x();
                float dy = center.y() - p.y();
                float dz = center.z() - p.z();
                if (dx * dx + dy * dy + dz * dz <= radiusSq) {
                    result.push_back(idx);
                }
            }
            return;
        }

        auto* internal = static_cast<OctreeInternalNode*>(node);
        float childSize = info.size * 0.5f;

        for (int i = 0; i < 8; ++i) {
            if (!internal->children[i]) continue;

            OctreeNodeInfo childInfo;
            childInfo.depth = info.depth + 1;
            childInfo.child_index = i;
            childInfo.size = childSize;
            childInfo.origin = QVector3D(
                info.origin.x() + ((i & 1) ? childSize : 0),
                info.origin.y() + ((i & 2) ? childSize : 0),
                info.origin.z() + ((i & 4) ? childSize : 0)
            );

            traverse(internal->children[i].get(), childInfo);
        }
    };

    OctreeNodeInfo rootInfo;
    rootInfo.origin = m_origin;
    rootInfo.size = m_size;
    rootInfo.depth = 0;
    traverse(m_root.get(), rootInfo);

    return result;
}

std::vector<size_t> PointCloudOctree::queryBox(const QVector3D& min,
                                               const QVector3D& max) const {
    std::vector<size_t> result;

    if (!m_root) return result;

    std::function<void(OctreeNode*, const OctreeNodeInfo&)> traverse;
    traverse = [&](OctreeNode* node, const OctreeNodeInfo& info) {
        if (!node) return;

        // AABB 相交测试
        bool intersects =
            info.origin.x() <= max.x() && info.origin.x() + info.size >= min.x() &&
            info.origin.y() <= max.y() && info.origin.y() + info.size >= min.y() &&
            info.origin.z() <= max.z() && info.origin.z() + info.size >= min.z();

        if (!intersects) return;

        if (node->isLeaf()) {
            auto* leaf = static_cast<OctreeLeafNode*>(node);
            for (size_t idx : leaf->indices) {
                result.push_back(idx);
            }
            return;
        }

        auto* internal = static_cast<OctreeInternalNode*>(node);
        float childSize = info.size * 0.5f;

        for (int i = 0; i < 8; ++i) {
            if (!internal->children[i]) continue;

            OctreeNodeInfo childInfo;
            childInfo.depth = info.depth + 1;
            childInfo.child_index = i;
            childInfo.size = childSize;
            childInfo.origin = QVector3D(
                info.origin.x() + ((i & 1) ? childSize : 0),
                info.origin.y() + ((i & 2) ? childSize : 0),
                info.origin.z() + ((i & 4) ? childSize : 0)
            );

            traverse(internal->children[i].get(), childInfo);
        }
    };

    OctreeNodeInfo rootInfo;
    rootInfo.origin = m_origin;
    rootInfo.size = m_size;
    rootInfo.depth = 0;
    traverse(m_root.get(), rootInfo);

    return result;
}

} // namespace DeepLux
