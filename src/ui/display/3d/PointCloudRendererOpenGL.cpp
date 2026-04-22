#include "PointCloudRendererOpenGL.h"
#include "PointCloudGPUBuffer.h"
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QDebug>

namespace DeepLux {

PointCloudRendererOpenGL::PointCloudRendererOpenGL()
    : m_vboPositions(0)
    , m_vboColors(0)
    , m_vboNormals(0)
    , m_pointCount(0)
    , m_pointSize(2.0f)
    , m_colorMode(0)
    , m_buffersDirty(true)
    , m_initialized(false)
{
}

PointCloudRendererOpenGL::~PointCloudRendererOpenGL() {
    cleanupBuffers();
}

void PointCloudRendererOpenGL::initializeGL() {
    if (m_initialized) return;

    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    f->initializeOpenGLFunctions();

    m_initialized = true;
}

void PointCloudRendererOpenGL::setPointCloud(const PointCloudGPUBuffer& buffer) {
    setPointCloud(buffer, false);
}

void PointCloudRendererOpenGL::setPointCloud(const PointCloudGPUBuffer& buffer, bool enableLOD) {
    m_lodEnabled = enableLOD;

    // 始终将数据存储在 LOD buffer 中，保证指针有效
    m_lodBuffer.setData(buffer);

    if (enableLOD) {
        const PointCloudGPUBuffer* levelBuffer = m_lodBuffer.getLevel(m_lodBuffer.currentLevel());
        m_buffer = levelBuffer;
        m_pointCount = levelBuffer ? levelBuffer->pointCount() : 0;
    } else {
        m_buffer = &m_lodBuffer.originalBuffer();
        m_pointCount = m_lodBuffer.originalPointCount();
    }
    m_buffersDirty = true;
}

void PointCloudRendererOpenGL::clear() {
    m_buffer = nullptr;
    m_pointCount = 0;
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

void PointCloudRendererOpenGL::setColorMode(int mode) {
    m_colorMode = mode;
}

void PointCloudRendererOpenGL::setUniformColor(const QColor& color) {
    m_uniformColor = color;
}

void PointCloudRendererOpenGL::setLODEnabled(bool enabled) {
    if (m_lodEnabled != enabled) {
        m_lodEnabled = enabled;
        if (m_buffer) {
            // 重新设置数据以切换模式
            if (enabled) {
                const PointCloudGPUBuffer* levelBuffer = m_lodBuffer.getLevel(m_lodBuffer.currentLevel());
                m_buffer = levelBuffer;
                m_pointCount = levelBuffer ? levelBuffer->pointCount() : 0;
            } else {
                m_buffer = &m_lodBuffer.originalBuffer();
                m_pointCount = m_lodBuffer.originalPointCount();
            }
            m_buffersDirty = true;
        }
    }
}

void PointCloudRendererOpenGL::updateLODForDistance(float distance) {
    if (!m_lodEnabled || !m_lodBuffer.isValid()) {
        return;
    }

    if (std::abs(m_currentDistance - distance) < 0.01f) {
        return;  // 距离变化太小，跳过
    }

    m_currentDistance = distance;

    // 计算新的 LOD 级别（使用成员变量以支持用户配置）
    int newLevel = m_lodController.calculateLODLevel(distance);
    int currentLevel = m_lodBuffer.currentLevel();

    if (newLevel != currentLevel) {
        m_lodBuffer.setCurrentLevel(newLevel);
        const PointCloudGPUBuffer* levelBuffer = m_lodBuffer.getLevel(newLevel);
        if (levelBuffer) {
            m_buffer = levelBuffer;
            m_pointCount = levelBuffer->pointCount();
            m_buffersDirty = true;  // 需要重新上传缓冲区
        }
    }
}

bool PointCloudRendererOpenGL::isValid() const {
    return m_buffer != nullptr && m_buffer->isValid() && m_pointCount > 0;
}

void PointCloudRendererOpenGL::render(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix) {
    if (!m_initialized) {
        initializeGL();
    }

    if (!isValid()) {
        return;
    }

    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();

    // 上传缓冲区
    if (m_buffersDirty) {
        uploadBuffers();
        m_buffersDirty = false;
    }

    // 设置 OpenGL 状态
    f->glClearColor(
        m_backgroundColor.redF(),
        m_backgroundColor.greenF(),
        m_backgroundColor.blueF(),
        1.0f
    );
    f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // 启用深度测试
    f->glEnable(GL_DEPTH_TEST);
    f->glDepthFunc(GL_LESS);

    // 启用点渲染
    f->glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

    // 使用简单着色器
    static QOpenGLShaderProgram* program = nullptr;
    if (!program) {
        program = new QOpenGLShaderProgram;
        program->addShaderFromSourceCode(QOpenGLShader::Vertex, POINT_CLOUD_VERTEX_SHADER);
        program->addShaderFromSourceCode(QOpenGLShader::Fragment, POINT_CLOUD_FRAGMENT_SHADER);
        program->link();
    }

    program->bind();

    // 设置 uniform
    program->setUniformValue("uViewMatrix", viewMatrix);
    program->setUniformValue("uProjectionMatrix", projectionMatrix);
    program->setUniformValue("uPointSize", m_pointSize);

    // 绑定 VBO 并绘制
    if (m_vboPositions) {
        program->enableAttributeArray("aPosition");
        program->setAttributeBuffer("aPosition", GL_FLOAT, 0, 3, 0);

        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboPositions);
        program->setAttributeBuffer("aPosition", GL_FLOAT, 0, 3, 0);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // 颜色
    if (m_colorMode == 1 && m_vboColors) {
        program->enableAttributeArray("aColor");
        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboColors);
        program->setAttributeBuffer("aColor", GL_FLOAT, 0, 3, 0);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        // 使用统一颜色
        program->setAttributeValue("aColor",
            QVector3D(m_uniformColor.redF(), m_uniformColor.greenF(), m_uniformColor.blueF()));
    }

    // 法向量（如果可用）
    if (m_vboNormals) {
        program->enableAttributeArray("aNormal");
        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboNormals);
        program->setAttributeBuffer("aNormal", GL_FLOAT, 0, 3, 0);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        program->setAttributeValue("aNormal", QVector3D(0, 1, 0));
    }

    // 绘制点云
    f->glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_pointCount));

    program->release();
}

void PointCloudRendererOpenGL::uploadBuffers() {
    if (!m_buffer || !m_initialized) return;

    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();

    cleanupBuffers();

    // 创建位置 VBO
    if (!m_buffer->positions.empty()) {
        f->glGenBuffers(1, &m_vboPositions);
        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboPositions);
        f->glBufferData(GL_ARRAY_BUFFER,
                        m_buffer->positions.size() * sizeof(float),
                        m_buffer->positions.data(),
                        GL_STATIC_DRAW);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // 创建颜色 VBO
    if (m_colorMode == 1 && !m_buffer->colors.empty()) {
        f->glGenBuffers(1, &m_vboColors);
        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboColors);
        f->glBufferData(GL_ARRAY_BUFFER,
                        m_buffer->colors.size() * sizeof(float),
                        m_buffer->colors.data(),
                        GL_STATIC_DRAW);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // 创建法向量 VBO
    if (!m_buffer->normals.empty()) {
        f->glGenBuffers(1, &m_vboNormals);
        f->glBindBuffer(GL_ARRAY_BUFFER, m_vboNormals);
        f->glBufferData(GL_ARRAY_BUFFER,
                        m_buffer->normals.size() * sizeof(float),
                        m_buffer->normals.data(),
                        GL_STATIC_DRAW);
        f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    m_buffersDirty = false;
}

void PointCloudRendererOpenGL::cleanupBuffers() {
    if (!m_initialized) return;

    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();

    if (m_vboPositions) {
        f->glDeleteBuffers(1, &m_vboPositions);
        m_vboPositions = 0;
    }
    if (m_vboColors) {
        f->glDeleteBuffers(1, &m_vboColors);
        m_vboColors = 0;
    }
    if (m_vboNormals) {
        f->glDeleteBuffers(1, &m_vboNormals);
        m_vboNormals = 0;
    }
}

} // namespace DeepLux
