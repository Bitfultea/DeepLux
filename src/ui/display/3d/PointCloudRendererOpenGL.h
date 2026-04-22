#pragma once

#include "IPointCloudRenderer.h"
#include "PointCloudGPUBuffer.h"
#include "PointCloudLODBuffer.h"
#include "LODController.h"
#include <QOpenGLFunctions>
#include <QMatrix4x4>
#include <memory>

namespace DeepLux {

/**
 * @brief OpenGL 点云渲染器
 *
 * 使用 OpenGL VBO 实现高效点云渲染，支持 LOD
 */
class PointCloudRendererOpenGL : public IPointCloudRenderer, protected QOpenGLFunctions {
public:
    PointCloudRendererOpenGL();
    ~PointCloudRendererOpenGL() override;

    // IPointCloudRenderer
    void setPointCloud(const PointCloudGPUBuffer& buffer) override;
    void setPointCloud(const PointCloudGPUBuffer& buffer, bool enableLOD) override;
    void clear() override;
    void setBackgroundColor(const QColor& color) override;
    void scheduleRedraw() override;
    void setPointSize(float size) override;
    void setColorMode(int mode) override;
    void setUniformColor(const QColor& color) override;
    void render(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix) override;
    bool isValid() const override;

    // LOD 支持
    void setLODEnabled(bool enabled) override;
    bool isLODEnabled() const override { return m_lodEnabled; }
    void updateLODForDistance(float distance) override;
    int currentLODLevel() const override { return m_lodBuffer.currentLevel(); }

    /**
     * @brief 获取 LOD 控制器引用（用于配置距离阈值等）
     */
    LODController& lodController() { return m_lodController; }
    const LODController& lodController() const { return m_lodController; }

    /**
     * @brief 初始化 OpenGL 资源
     * 必须在有效的 OpenGL 上下文中调用
     */
    void initializeGL();

private:
    void uploadBuffers();
    void cleanupBuffers();

    // VBO IDs
    unsigned int m_vboPositions = 0;
    unsigned int m_vboColors = 0;
    unsigned int m_vboNormals = 0;

    // 当前数据
    const PointCloudGPUBuffer* m_buffer = nullptr;
    size_t m_pointCount = 0;

    // LOD 支持
    PointCloudLODBuffer m_lodBuffer;
    LODController m_lodController;  // 用户可配置的 LOD 控制器
    bool m_lodEnabled = true;
    float m_currentDistance = 0.0f;

    // 渲染参数
    float m_pointSize = 2.0f;
    int m_colorMode = 0;  // 0=uniform, 1=rgb, 2=labels
    QColor m_uniformColor = Qt::white;
    QColor m_backgroundColor = Qt::darkGray;

    // 脏标记
    bool m_buffersDirty = true;
    bool m_initialized = false;
};

// 简单顶点着色器源码
const char* const POINT_CLOUD_VERTEX_SHADER = R"(
    #version 130
    attribute vec3 aPosition;
    attribute vec3 aColor;
    attribute vec3 aNormal;

    uniform mat4 uViewMatrix;
    uniform mat4 uProjectionMatrix;
    uniform float uPointSize;

    varying vec3 vColor;
    varying vec3 vNormal;

    void main() {
        gl_Position = uProjectionMatrix * uViewMatrix * vec4(aPosition, 1.0);
        gl_PointSize = uPointSize;
        vColor = aColor;
        vNormal = aNormal;
    }
)";

// 简单片段着色器源码
const char* const POINT_CLOUD_FRAGMENT_SHADER = R"(
    #version 130
    varying vec3 vColor;
    varying vec3 vNormal;

    void main() {
        // 简单的点渲染，无深度缓冲（透明点云效果）
        gl_FragColor = vec4(vColor, 1.0);
    }
)";

} // namespace DeepLux
