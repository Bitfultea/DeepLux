#pragma once

#include "IPointCloudRenderer.h"
#include "LODController.h"
#include "PointCloudGPUBuffer.h"
#include "PointCloudLODBuffer.h"

#include <QMatrix4x4>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <array>
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
    void setColorMode(ColorMode mode) override;
    ColorMode colorMode() const override {
        return m_colorMode;
    }
    void setUniformColor(const QColor& color) override;
    void render(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix) override;
    bool isValid() const override;

    // LOD 支持
    void setLODEnabled(bool enabled) override;
    bool isLODEnabled() const override {
        return m_lodEnabled;
    }
    void updateLODForDistance(float distance) override;
    int currentLODLevel() const override {
        return m_lodBuffer.currentLevel();
    }

    void setInteractionActive(bool active);
    bool isInteractionActive() const {
        return m_interactionActive;
    }
    float heightMinimum() const {
        return m_zMin;
    }
    float heightMaximum() const {
        return m_zMax;
    }

    /**
     * @brief 获取 LOD 控制器引用（用于配置距离阈值等）
     */
    LODController& lodController() {
        return m_lodController;
    }
    const LODController& lodController() const {
        return m_lodController;
    }

    /**
     * @brief 初始化 OpenGL 资源
     * 必须在有效的 OpenGL 上下文中调用
     */
    void initializeGL();

private:
    struct VboSet {
        unsigned int positions = 0;
        unsigned int colors = 0;
        unsigned int normals = 0;
        unsigned int intensities = 0;
        bool uploaded = false;
    };

    void uploadBuffers();
    void cleanupBuffers();
    void selectLODLevel(int level);
    int activeLODLevel() const;

    // VBO IDs
    unsigned int m_vboPositions = 0;
    unsigned int m_vboColors = 0;
    unsigned int m_vboNormals = 0;
    unsigned int m_vboIntensities = 0;
    std::unique_ptr<QOpenGLShaderProgram> m_program;

    // 当前数据
    const PointCloudGPUBuffer* m_buffer = nullptr;
    size_t m_pointCount = 0;
    float m_zMin = 0;
    float m_zMax = 1;

    // LOD 支持
    PointCloudLODBuffer m_lodBuffer;
    LODController m_lodController; // 用户可配置的 LOD 控制器
    bool m_lodEnabled = true;
    float m_currentDistance = 0.0f;
    bool m_interactionActive = false;
    int m_interactionLODLevel = 2;

    // 渲染参数
    float m_pointSize = 2.0f;
    ColorMode m_colorMode = ColorMode::Uniform;
    QColor m_uniformColor = Qt::white;
    QColor m_backgroundColor = Qt::darkGray;

    std::array<VboSet, LODController::MAX_LOD_LEVELS> m_vboCache;

    // 脏标记
    bool m_buffersDirty = true;
    bool m_cacheInvalid = false;
    bool m_initialized = false;
};

// 顶点着色器（Blinn-Phong 光照）
const char* const POINT_CLOUD_VERTEX_SHADER = R"(
    #version 130
    attribute vec3 aPosition;
    attribute vec3 aColor;
    attribute vec3 aNormal;
    attribute float aIntensity;

    uniform mat4 uViewMatrix;
    uniform mat4 uProjectionMatrix;
    uniform float uPointSize;
    uniform int uColorMode;
    uniform vec3 uUniformColor;
    uniform float uZMin;
    uniform float uZMax;

    varying vec3 vColor;
    varying vec3 vNormal;
    varying vec3 vWorldPos;

    vec3 heightColor(float z) {
        float range = max(uZMax - uZMin, max(abs(uZMin), abs(uZMax)) * 0.000001);
        float t = clamp((z - uZMin) / max(range, 0.000001), 0.0, 1.0);
        return vec3(
            smoothstep(0.5, 1.0, t),
            sin(t * 3.14159),
            1.0 - t
        );
    }

    void main() {
        vec4 worldPos = uViewMatrix * vec4(aPosition, 1.0);
        gl_Position = uProjectionMatrix * worldPos;
        gl_PointSize = uPointSize;
        vNormal = mat3(uViewMatrix) * aNormal;
        vWorldPos = worldPos.xyz;

        if (uColorMode == 0) {
            vColor = uUniformColor;
        } else if (uColorMode == 1) {
            vColor = aColor;
        } else if (uColorMode == 2) {
            vColor = heightColor(aPosition.z);
        } else if (uColorMode == 3) {
            float t = clamp(aIntensity, 0.0, 1.0);
            vColor = vec3(t);
        } else if (uColorMode == 4) {
            vec3 n = normalize(vNormal);
            vColor = n * 0.5 + 0.5;
        } else {
            vColor = aColor;
        }
    }
)";

const char* const POINT_CLOUD_FRAGMENT_SHADER = R"(
    #version 130
    varying vec3 vColor;
    varying vec3 vNormal;
    varying vec3 vWorldPos;

    uniform int uColorMode;
    uniform vec3 uLightPos;
    uniform vec3 uLightColor;
    uniform vec3 uViewPos;
    uniform float uAmbient;
    uniform float uDiffuse;
    uniform float uSpecular;
    uniform float uShininess;

    void main() {
        if (uColorMode != 5) {
            gl_FragColor = vec4(vColor, 1.0);
            return;
        }

        vec3 normal = normalize(vNormal);
        if (length(vNormal) < 0.01) { gl_FragColor = vec4(vColor, 1.0); return; }

        vec3 lightDir = normalize(uLightPos - vWorldPos);
        vec3 viewDir = normalize(uViewPos - vWorldPos);

        vec3 ambient = uAmbient * uLightColor;

        float diff = max(dot(normal, lightDir), 0.0);
        vec3 diffuse = uDiffuse * diff * uLightColor;

        vec3 halfwayDir = normalize(lightDir + viewDir);
        float spec = pow(max(dot(normal, halfwayDir), 0.0), uShininess);
        vec3 specular = uSpecular * spec * uLightColor;

        vec3 result = (ambient + diffuse + specular) * vColor;
        gl_FragColor = vec4(result, 1.0);
    }
)";

} // namespace DeepLux
