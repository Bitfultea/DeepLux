#include "PointCloudRendererOpenGL.h"

#include "PointCloudGPUBuffer.h"

#include <QDebug>
#include <QOpenGLContext>
#include <algorithm>
#include <cmath>
#include <limits>

namespace DeepLux {

PointCloudRendererOpenGL::PointCloudRendererOpenGL()
    : m_vboPositions(0), m_vboColors(0), m_vboNormals(0), m_pointCount(0), m_pointSize(2.0f),
      m_colorMode(ColorMode::Uniform), m_buffersDirty(true), m_initialized(false) {}

PointCloudRendererOpenGL::~PointCloudRendererOpenGL() {
    cleanupBuffers();
    if (QOpenGLContext::currentContext()) {
        m_program.reset();
    } else {
        m_program.release();
    }
}

void PointCloudRendererOpenGL::initializeGL() {
    if (m_initialized)
        return;

    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context) {
        qWarning() << "PointCloudRendererOpenGL: initializeGL called without current context";
        return;
    }

    QOpenGLFunctions* f = context->functions();
    f->initializeOpenGLFunctions();

    m_program = std::make_unique<QOpenGLShaderProgram>();
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, POINT_CLOUD_VERTEX_SHADER)) {
        qWarning() << "PointCloudRendererOpenGL: vertex shader compile failed:" << m_program->log();
        m_program.reset();
        return;
    }
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, POINT_CLOUD_FRAGMENT_SHADER)) {
        qWarning() << "PointCloudRendererOpenGL: fragment shader compile failed:" << m_program->log();
        m_program.reset();
        return;
    }
    if (!m_program->link()) {
        qWarning() << "PointCloudRendererOpenGL: shader link failed:" << m_program->log();
        m_program.reset();
        return;
    }

    m_initialized = true;
}

void PointCloudRendererOpenGL::setPointCloud(const PointCloudGPUBuffer& buffer) {
    setPointCloud(buffer, false);
}

void PointCloudRendererOpenGL::setPointCloud(const PointCloudGPUBuffer& buffer, bool enableLOD) {
    m_lodEnabled = enableLOD;
    m_interactionActive = false;
    m_currentDistance = 0.0f;
    m_cacheInvalid = true;

    m_zMin = std::numeric_limits<float>::infinity();
    m_zMax = -std::numeric_limits<float>::infinity();
    for (size_t i = 2; i < buffer.positions.size(); i += 3) {
        const float z = buffer.positions[i];
        if (std::isfinite(z)) {
            m_zMin = std::min(m_zMin, z);
            m_zMax = std::max(m_zMax, z);
        }
    }
    if (!std::isfinite(m_zMin) || !std::isfinite(m_zMax)) {
        m_zMin = 0.0f;
        m_zMax = 1.0f;
    }

    // 始终将数据存储在 LOD buffer 中，保证指针有效
    m_lodBuffer.setData(buffer);
    selectLODLevel(0);
    m_buffersDirty = true;
}

void PointCloudRendererOpenGL::clear() {
    m_buffer = nullptr;
    m_pointCount = 0;
    m_zMin = 0.0f;
    m_zMax = 1.0f;
    cleanupBuffers();
}

void PointCloudRendererOpenGL::setBackgroundColor(const QColor& color) {
    m_backgroundColor = color;
}

void PointCloudRendererOpenGL::scheduleRedraw() {
    // OpenGL widget 会自动重绘
}

void PointCloudRendererOpenGL::setPointSize(float size) {
    m_pointSize = size;
}

void PointCloudRendererOpenGL::setColorMode(ColorMode mode) {
    m_colorMode = mode;
}

void PointCloudRendererOpenGL::setUniformColor(const QColor& color) {
    m_uniformColor = color;
}

void PointCloudRendererOpenGL::setLODEnabled(bool enabled) {
    if (m_lodEnabled != enabled) {
        m_lodEnabled = enabled;
        selectLODLevel(enabled ? m_lodBuffer.currentLevel() : 0);
        m_buffersDirty = true;
    }
}

void PointCloudRendererOpenGL::updateLODForDistance(float distance) {
    if (!m_lodEnabled || !m_lodBuffer.isValid()) {
        return;
    }

    if (std::abs(m_currentDistance - distance) < 0.01f && !m_interactionActive) {
        return; // 距离变化太小，跳过
    }

    m_currentDistance = distance;

    if (m_interactionActive) {
        selectLODLevel(std::min(m_interactionLODLevel, LODController::MAX_LOD_LEVELS - 1));
        return;
    }

    // 计算新的 LOD 级别（使用成员变量以支持用户配置）
    selectLODLevel(m_lodController.calculateLODLevel(distance));
}

void PointCloudRendererOpenGL::setInteractionActive(bool active) {
    if (m_interactionActive == active) {
        return;
    }

    m_interactionActive = active;
    if (!m_lodEnabled || !m_lodBuffer.isValid()) {
        return;
    }

    if (active) {
        selectLODLevel(std::min(m_interactionLODLevel, LODController::MAX_LOD_LEVELS - 1));
    } else {
        selectLODLevel(m_lodController.calculateLODLevel(m_currentDistance));
    }
}

bool PointCloudRendererOpenGL::isValid() const {
    return m_buffer != nullptr && m_buffer->isValid() && m_pointCount > 0;
}

int PointCloudRendererOpenGL::activeLODLevel() const {
    return m_lodEnabled ? m_lodBuffer.currentLevel() : 0;
}

void PointCloudRendererOpenGL::selectLODLevel(int level) {
    if (!m_lodBuffer.isValid()) {
        m_buffer = nullptr;
        m_pointCount = 0;
        return;
    }

    level = std::max(0, std::min(level, LODController::MAX_LOD_LEVELS - 1));
    if (!m_lodEnabled) {
        level = 0;
    }

    const int oldLevel = activeLODLevel();
    const PointCloudGPUBuffer* oldBuffer = m_buffer;
    m_lodBuffer.setCurrentLevel(level);
    const PointCloudGPUBuffer* levelBuffer = m_lodEnabled ? m_lodBuffer.getLevel(level) : &m_lodBuffer.originalBuffer();
    m_buffer = levelBuffer;
    m_pointCount = levelBuffer ? levelBuffer->pointCount() : 0;
    if (oldLevel != level || oldBuffer != m_buffer) {
        m_buffersDirty = true;
    }
}

void PointCloudRendererOpenGL::render(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix) {
    if (!m_initialized) {
        initializeGL();
    }
    if (!m_initialized || !m_program) {
        return;
    }

    if (!isValid()) {
        return;
    }

    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context) {
        return;
    }
    QOpenGLFunctions* f = context->functions();

    // 上传缓冲区
    if (m_buffersDirty) {
        uploadBuffers();
        m_buffersDirty = false;
    }

    // 设置 OpenGL 状态
    f->glClearColor(m_backgroundColor.redF(), m_backgroundColor.greenF(), m_backgroundColor.blueF(), 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // 启用深度测试
    f->glEnable(GL_DEPTH_TEST);
    f->glDepthFunc(GL_LESS);

    // 启用点渲染
    f->glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

    m_program->bind();

    // 设置 uniform
    m_program->setUniformValue("uViewMatrix", viewMatrix);
    m_program->setUniformValue("uProjectionMatrix", projectionMatrix);
    m_program->setUniformValue("uPointSize", m_pointSize);

    // 颜色模式 + 颜色
    m_program->setUniformValue("uColorMode", static_cast<int>(m_colorMode));
    m_program->setUniformValue("uUniformColor",
                               QVector3D(m_uniformColor.redF(), m_uniformColor.greenF(), m_uniformColor.blueF()));

    // 高度着色范围
    if (m_colorMode == ColorMode::Height) {
        m_program->setUniformValue("uZMin", m_zMin);
        m_program->setUniformValue("uZMax", m_zMax);
    } else {
        m_program->setUniformValue("uZMin", 0.0f);
        m_program->setUniformValue("uZMax", 1.0f);
    }

    // Blinn-Phong 光照参数
    QVector3D eyePos = viewMatrix.inverted().map(QVector3D(0, 0, 0));
    m_program->setUniformValue("uLightPos", eyePos + QVector3D(0, 0, 10));
    m_program->setUniformValue("uLightColor", QVector3D(1.0f, 1.0f, 1.0f));
    m_program->setUniformValue("uViewPos", eyePos);
    m_program->setUniformValue("uAmbient", 0.25f);
    m_program->setUniformValue("uDiffuse", 0.60f);
    m_program->setUniformValue("uSpecular", 0.30f);
    m_program->setUniformValue("uShininess", 32.0f);

    // 绑定 VBO 并绘制
    if (m_vboPositions) {
        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboPositions);
        m_program->enableAttributeArray("aPosition");
        m_program->setAttributeBuffer("aPosition", GL_FLOAT, 0, 3, 0);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // 颜色 VBO
    if (m_vboColors) {
        m_program->enableAttributeArray("aColor");
        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboColors);
        m_program->setAttributeBuffer("aColor", GL_FLOAT, 0, 3, 0);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        m_program->setAttributeValue("aColor",
                                     QVector3D(m_uniformColor.redF(), m_uniformColor.greenF(), m_uniformColor.blueF()));
    }

    // 强度 VBO
    if (m_colorMode == ColorMode::Intensity && m_vboIntensities) {
        m_program->enableAttributeArray("aIntensity");
        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboIntensities);
        m_program->setAttributeBuffer("aIntensity", GL_FLOAT, 0, 1, 0);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        m_program->setAttributeValue("aIntensity", 0.5f);
    }

    // 法向量
    if (m_vboNormals) {
        m_program->enableAttributeArray("aNormal");
        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboNormals);
        m_program->setAttributeBuffer("aNormal", GL_FLOAT, 0, 3, 0);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        m_program->setAttributeValue("aNormal", QVector3D(0, 0, 1));
    }

    // 绘制点云
    f->glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_pointCount));

    m_program->release();
}

void PointCloudRendererOpenGL::uploadBuffers() {
    if (!m_buffer || !m_initialized)
        return;

    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context) {
        return;
    }
    QOpenGLFunctions* f = context->functions();

    if (m_cacheInvalid) {
        cleanupBuffers();
    }

    const int level = activeLODLevel();
    VboSet& vbo = m_vboCache[level];
    if (!vbo.uploaded) {
        auto upload = [f](unsigned int& id, const std::vector<float>& data) {
            if (data.empty()) {
                return;
            }
            f->glGenBuffers(1, &id);
            f->glBindBuffer(GL_ARRAY_BUFFER, id);
            f->glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
            f->glBindBuffer(GL_ARRAY_BUFFER, 0);
        };

        upload(vbo.positions, m_buffer->positions);
        upload(vbo.colors, m_buffer->colors);
        upload(vbo.normals, m_buffer->normals);
        upload(vbo.intensities, m_buffer->intensities);
        vbo.uploaded = true;
    }

    m_vboPositions = vbo.positions;
    m_vboColors = vbo.colors;
    m_vboNormals = vbo.normals;
    m_vboIntensities = vbo.intensities;
    m_buffersDirty = false;
}

void PointCloudRendererOpenGL::cleanupBuffers() {
    if (!m_initialized)
        return;

    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context) {
        m_cacheInvalid = true;
        return;
    }
    QOpenGLFunctions* f = context->functions();

    for (VboSet& vbo : m_vboCache) {
        if (vbo.positions)
            f->glDeleteBuffers(1, &vbo.positions);
        if (vbo.colors)
            f->glDeleteBuffers(1, &vbo.colors);
        if (vbo.normals)
            f->glDeleteBuffers(1, &vbo.normals);
        if (vbo.intensities)
            f->glDeleteBuffers(1, &vbo.intensities);
        vbo = VboSet{};
    }

    m_vboPositions = 0;
    m_vboColors = 0;
    m_vboNormals = 0;
    m_vboIntensities = 0;
    m_cacheInvalid = false;
}

} // namespace DeepLux
