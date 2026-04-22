#pragma once

#include "OctreeNode.h"
#include "PointCloudOctree.h"
#include <QMatrix4x4>
#include <QVector3D>
#include <QColor>
#include <QOpenGLShaderProgram>
#include <memory>
#include <vector>
#include <unordered_map>

namespace DeepLux {

class CameraController;

/**
 * @brief 八叉树感知渲染器
 *
 * 特性:
 * - 视锥裁剪 (Frustum Culling)
 * - 基于相机距离的 LOD
 * - 节点级 VBO 缓存
 * - GPU 缓冲区管理
 * - LOD 阈值设置（超过阈值才启用 LOD）
 */
class OctreeRenderer {
public:
    OctreeRenderer();
    ~OctreeRenderer();

    // 设置八叉树
    void setOctree(std::shared_ptr<PointCloudOctree> octree);
    std::shared_ptr<PointCloudOctree> octree() const { return m_octree; }

    // LOD 配置
    void setLODEnabled(bool enabled) { m_lodEnabled = enabled; }
    bool isLODEnabled() const { return m_lodEnabled; }

    void setMaxLODDepth(int depth) { m_maxLODDepth = depth; }
    int maxLODDepth() const { return m_maxLODDepth; }

    // LOD 阈值配置
    void setLODThreshold(size_t threshold) { m_lodThreshold = threshold; }
    size_t lodThreshold() const { return m_lodThreshold; }

    // 相机设置（用于视锥裁剪和 LOD 计算）
    void setCamera(const CameraController* camera);
    void setCameraMatrices(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix);

    // 渲染
    void render(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix);

    // OpenGL 上下文初始化（必须在有效 OpenGL 上下文中调用）
    void initializeGL();

    // 清理
    void clear();

    // 状态统计
    size_t renderedPointCount() const { return m_renderedPointCount; }
    size_t visibleNodeCount() const { return m_visibleNodeCount; }
    int currentLODLevel() const { return m_currentLODLevel; }
    bool isUsingLOD() const { return m_useLOD; }

    // 渲染参数
    void setPointSize(float size) { m_pointSize = size; }
    float pointSize() const { return m_pointSize; }

    void setUniformColor(const QColor& color) { m_uniformColor = color; }

private:
    struct VBOEntry {
        unsigned int vboPositions = 0;
        unsigned int vboColors = 0;
        unsigned int vboNormals = 0;
        size_t pointCount = 0;
        bool dirty = true;
    };

    void updateVisibleNodes();
    int selectLODLevel();
    void uploadDirtyBuffers();
    void renderNodes();

    // 视锥裁剪
    struct Plane {
        QVector3D normal;
        float d;  // plane equation: normal.dot(p) + d = 0
    };
    void extractFrustumPlanes(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix);
    bool isInFrustum(const QVector3D& center, float size) const;
    bool isSphereInFrustum(const QVector3D& center, float radius) const;

    std::shared_ptr<PointCloudOctree> m_octree;

    // 相机视锥（6 个平面）
    std::vector<Plane> m_frustumPlanes;
    QVector3D m_cameraPosition;
    QMatrix4x4 m_viewMatrix;
    QMatrix4x4 m_projectionMatrix;

    // 可见节点
    std::vector<const OctreeLeafNode*> m_visibleLeaves;
    int m_currentLODLevel = 0;
    size_t m_renderedPointCount = 0;
    size_t m_visibleNodeCount = 0;
    bool m_useLOD = false;

    // VBO 缓存
    std::unordered_map<const OctreeLeafNode*, VBOEntry> m_vboCache;

    // 渲染参数
    float m_pointSize = 2.0f;
    QColor m_uniformColor = Qt::white;
    QColor m_backgroundColor = Qt::darkGray;

    // LOD 控制
    bool m_lodEnabled = true;
    int m_maxLODDepth = 8;
    size_t m_lodThreshold = 50000;  // 超过 5 万点启用 LOD

    // OpenGL 状态
    bool m_initialized = false;
    QOpenGLShaderProgram* m_shaderProgram = nullptr;
};

} // namespace DeepLux
