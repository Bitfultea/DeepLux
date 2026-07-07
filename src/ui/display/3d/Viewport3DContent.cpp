#include "Viewport3DContent.h"

#include "PointCloudGPUBuffer.h"

#include <QDebug>
#include <QMouseEvent>
#include <QOpenGLContext>

namespace DeepLux {

Viewport3DContent::Viewport3DContent(QWidget* parent)
    : QOpenGLWidget(parent), m_renderer(nullptr), m_needsUpdate(true), m_lodEnabled(true) {
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
    if (context() && context()->isValid()) {
        makeCurrent();
        if (QOpenGLContext::currentContext()) {
            m_renderer.reset(); // 清理渲染器资源
            doneCurrent();
            return;
        }
        doneCurrent();
    }
    m_renderer.reset();
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
    if (h == 0)
        h = 1;
    glViewport(0, 0, w, h);
    updateMatrices();
    m_needsUpdate = true;
}

void Viewport3DContent::paintGL() {
    if (!m_renderer)
        return;

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
        m_lastPoints.clear();
        m_lastPoints.reserve(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i > 0) {
                mn = mn.cwiseMin(pts[i]);
                mx = mx.cwiseMax(pts[i]);
            }
            m_lastPoints.push_back(QVector3D(static_cast<float>(pts[i].x()), static_cast<float>(pts[i].y()),
                                             static_cast<float>(pts[i].z())));
        }
        m_bboxMin = QVector3D(static_cast<float>(mn.x()), static_cast<float>(mn.y()), static_cast<float>(mn.z()));
        m_bboxMax = QVector3D(static_cast<float>(mx.x()), static_cast<float>(mx.y()), static_cast<float>(mx.z()));
        m_hasBbox = true;
        m_camera.frameData(m_bboxMin, m_bboxMax);
    } else {
        m_hasBbox = false;
        m_lastPoints.clear();
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
    if (height() == 0)
        aspect = 1.0f;

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

void Viewport3DContent::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::ControlModifier) {
        if (m_lastPoints.empty())
            return;

        // Update matrices for current viewport dimensions
        updateMatrices();

        QRect viewport(0, 0, width(), height());
        QPointF clickPos = event->pos();

        float bestDist = 144.0f; // 12px squared
        int bestIdx = -1;

        // ponytail: O(n) picking is enough for interactive setup; replace with a spatial index if latency shows up.
        for (size_t i = 0; i < m_lastPoints.size(); ++i) {
            QVector3D projected = m_lastPoints[i].project(m_viewMatrix, m_projectionMatrix, viewport);
            float dx = static_cast<float>(projected.x() - clickPos.x());
            float dy = static_cast<float>(projected.y() - clickPos.y());
            float dist = dx * dx + dy * dy;
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = static_cast<int>(i);
            }
        }

        if (bestIdx >= 0) {
            emit point3DClicked(m_lastPoints[bestIdx]);
        }
    }
}

void Viewport3DContent::wheelEvent(QWheelEvent* event) {
    float steps = event->angleDelta().y() / 120.0f;
    m_camera.zoom(steps * 0.15f);
    m_needsUpdate = true;
    update();
}

} // namespace DeepLux
