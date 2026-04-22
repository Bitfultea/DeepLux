#pragma once

#include <memory>
#include <array>
#include <vector>
#include <Eigen/Dense>
#include <QVector3D>
#include <QColor>

namespace DeepLux {

class PointCloudGPUBuffer;

/**
 * @brief 八叉树节点信息（在遍历时计算，不存储）
 */
struct OctreeNodeInfo {
    QVector3D origin;      // 节点原点 (最小角)
    float size;           // 节点大小
    size_t depth;         // 深度 (root = 0)
    size_t child_index;   // 在父节点中的索引 (0-7)

    // 中心点
    QVector3D center() const {
        return QVector3D(origin.x() + size * 0.5f,
                         origin.y() + size * 0.5f,
                         origin.z() + size * 0.5f);
    }

    // 最大点
    QVector3D maxBound() const {
        return QVector3D(origin.x() + size,
                         origin.y() + size,
                         origin.z() + size);
    }
};

/**
 * @brief 八叉树节点基类
 */
class OctreeNode {
public:
    virtual ~OctreeNode() = default;

    // 节点类型
    virtual bool isLeaf() const = 0;

    // 获取中心点
    virtual QVector3D center() const = 0;

    // 获取大小
    virtual float size() const = 0;
};

/**
 * @brief 八叉树内部节点
 */
class OctreeInternalNode : public OctreeNode {
public:
    OctreeInternalNode();
    ~OctreeInternalNode() override = default;

    bool isLeaf() const override { return false; }
    QVector3D center() const override { return m_center; }
    float size() const override { return m_size; }

    void setInfo(const OctreeNodeInfo& info) {
        m_center = info.center();
        m_size = info.size;
    }

    // 8 个子节点
    std::array<std::shared_ptr<OctreeNode>, 8> children;

    // 所有子节点的点索引汇总（用于快速聚合查询）
    std::vector<size_t> indices;

private:
    QVector3D m_center;
    float m_size = 0;
};

/**
 * @brief 八叉树叶子节点
 */
class OctreeLeafNode : public OctreeNode {
public:
    OctreeLeafNode();
    ~OctreeLeafNode() override = default;

    bool isLeaf() const override { return true; }
    QVector3D center() const override { return m_center; }
    float size() const override { return m_size; }

    void setInfo(const OctreeNodeInfo& info) {
        m_center = info.center();
        m_size = info.size;
    }

    // 包含的点的索引
    std::vector<size_t> indices;

    // 预计算的 GPU 缓冲区
    std::unique_ptr<PointCloudGPUBuffer> gpuBuffer;

    // 平均颜色
    QVector3D color = QVector3D(1.0f, 1.0f, 1.0f);

    // 脏标记（需要重新上传 VBO）
    mutable bool dirty = true;

private:
    QVector3D m_center;
    float m_size = 0;
};

// 8 个子节点的索引排列 (x + y*2 + z*4)
// 0: [0,0,0]  1: [1,0,0]
// 2: [0,1,0]  3: [1,1,0]
// 4: [0,0,1]  5: [1,0,1]
// 6: [0,1,1]  7: [1,1,1]

/**
 * @brief 根据点在父节点中的位置计算子节点索引
 */
inline int computeChildIndex(const QVector3D& point, const OctreeNodeInfo& parentInfo) {
    float midX = parentInfo.origin.x() + parentInfo.size * 0.5f;
    float midY = parentInfo.origin.y() + parentInfo.size * 0.5f;
    float midZ = parentInfo.origin.z() + parentInfo.size * 0.5f;

    int x = point.x() >= midX ? 1 : 0;
    int y = point.y() >= midY ? 1 : 0;
    int z = point.z() >= midZ ? 1 : 0;

    return x + y * 2 + z * 4;
}

} // namespace DeepLux
