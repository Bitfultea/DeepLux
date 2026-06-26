#include "Viewport3DContent.h"
#include "PointCloudGPUBuffer.h"
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QDebug>

namespace DeepLux {

Viewport3DContent::Viewport3DContent(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_renderer(nullptr)
    , m_needsUpdate(true)
    , m_lodEnabled(true)
{
    // 显式设置 format — 有些系统需要 widget-level format
    QSurfaceFormat fmt;
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    setFormat(fmt);

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setContextMenuPolicy(Qt::NoContextMenu);
}

Viewport3DContent::~Viewport3DContent() {
    makeCurrent();
    m_renderer.reset();  // 清理渲染器资源
    doneCurrent();
}

void Viewport3DContent::initializeGL() {
    if (!m_renderer) {
        m_renderer = std::make_unique<PointCloudRendererOpenGL>();
    }
    m_renderer->setPointSize(5.0f);
    m_renderer->setUniformColor(Qt::white);
    m_renderer->initializeGL();

    if (m_hasBbox) {
        m_camera.frameData(m_bboxMin, m_bboxMax);
    } else {
        m_camera.reset();
    }
    updateMatrices();
}

void Viewport3DContent::resizeGL(int w, int h) {
    if (h == 0) h = 1;
    glViewport(0, 0, w, h);
    updateMatrices();
    m_needsUpdate = true;
}

void Viewport3DContent::paintGL() {
    if (!m_renderer) return;

    // 更新矩阵
    if (m_needsUpdate) {
        updateMatrices();
        m_needsUpdate = false;
    }

    // 更新 LOD（基于相机距离）
    if (m_lodEnabled) {
        float distance = m_camera.distance();
        m_renderer->updateLODForDistance(distance);
    }

    // 渲染
    m_renderer->render(m_viewMatrix, m_projectionMatrix);
}

void Viewport3DContent::displayData(const DisplayData& data) {
    const auto* pcData = data.pointCloudData();
    if (!pcData || pcData->isEmpty()) {
        clearDisplay();
        return;
    }

    // initializeGL 可能尚未运行（QOpenGLWidget 首次 show 时触发）
    if (!m_renderer) {
        m_renderer = std::make_unique<PointCloudRendererOpenGL>();
    }

    // 转换为 GPU 缓冲区
    m_gpuBuffer.fromPointCloudData(*pcData);

    if (!pcData->points.empty()) {
        auto& pts = pcData->points;
        Eigen::Vector3d mn = pts[0], mx = pts[0];
        for (size_t i = 1; i < pts.size(); ++i) {
            mn = mn.cwiseMin(pts[i]);
            mx = mx.cwiseMax(pts[i]);
        }
        m_bboxMin = QVector3D(static_cast<float>(mn.x()), static_cast<float>(mn.y()), static_cast<float>(mn.z()));
        m_bboxMax = QVector3D(static_cast<float>(mx.x()), static_cast<float>(mx.y()), static_cast<float>(mx.z()));
        m_hasBbox = true;
        m_camera.frameData(m_bboxMin, m_bboxMax);
    } else {
        m_hasBbox = false;
    }

    if (m_renderer) {
        m_renderer->setPointCloud(m_gpuBuffer, m_lodEnabled);
        m_renderer->setLODEnabled(m_lodEnabled);
        m_renderer->setColorMode(m_renderMode);
    }

    m_needsUpdate = true;
    update();
}

void Viewport3DContent::setRenderMode(ColorMode mode) {
    if (m_renderMode != mode) {
        m_renderMode = mode;
        if (m_renderer) {
            m_renderer->setColorMode(mode);
            m_needsUpdate = true;
            update();
        }
    }
}

void Viewport3DContent::clearDisplay() {
    m_gpuBuffer.clear();
    if (m_renderer) {
        m_renderer->clear();
    }
    m_needsUpdate = true;
    update();
}

void Viewport3DContent::resetCamera() {
    if (m_hasBbox) {
        m_camera.frameData(m_bboxMin, m_bboxMax);
    } else {
        m_camera.reset();
    }
    updateMatrices();
    m_needsUpdate = true;
    update();
}

void Viewport3DContent::applyTheme(bool isDark) {
    if (m_renderer) {
        m_renderer->setBackgroundColor(isDark ? QColor("#1a1a1a") : QColor("#f0f0f0"));
    }
    update();
}

void Viewport3DContent::setLODEnabled(bool enabled) {
    if (m_lodEnabled != enabled) {
        m_lodEnabled = enabled;
        if (m_renderer) {
            m_renderer->setLODEnabled(enabled);
        }
    }
}

bool Viewport3DContent::isLODEnabled() const {
    return m_lodEnabled;
}

void Viewport3DContent::updateMatrices() {
    float aspect = static_cast<float>(width()) / static_cast<float>(height());
    if (height() == 0) aspect = 1.0f;

    m_projectionMatrix = m_camera.projectionMatrix(aspect);
    m_viewMatrix = m_camera.viewMatrix();
}

void Viewport3DContent::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
}

void Viewport3DContent::mouseMoveEvent(QMouseEvent* event) {
    QPoint delta = event->pos() - m_lastMousePos;
    float orbitSens = 0.005f;
    float d = std::max(m_camera.distance(), 1.0f);
    float moveScale = d * 0.002f;

    if (event->buttons() & Qt::LeftButton) {
        m_camera.orbit(delta.x() * orbitSens, -delta.y() * orbitSens);
        m_needsUpdate = true;
        update();
    } else if (event->buttons() & Qt::RightButton) {
        m_camera.pan(-delta.x() * moveScale, delta.y() * moveScale);
        m_needsUpdate = true;
        update();
    } else if (event->buttons() & Qt::MiddleButton) {
        m_camera.zoom(delta.y() * moveScale);
        m_needsUpdate = true;
        update();
    }

    m_lastMousePos = event->pos();
}

void Viewport3DContent::wheelEvent(QWheelEvent* event) {
    float steps = event->angleDelta().y() / 120.0f;
    m_camera.zoom(steps * 0.15f);
    m_needsUpdate = true;
    update();
}

} // namespace DeepLux
