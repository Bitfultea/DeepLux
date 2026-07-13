#include "Viewport3DContent.h"

#include "PointCloudGPUBuffer.h"

#include <QDebug>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <algorithm>
#include <cmath>

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

    m_interactionTimer.setSingleShot(true);
    m_interactionTimer.setInterval(16);
    connect(&m_interactionTimer, &QTimer::timeout, this, &Viewport3DContent::flushPendingInteraction);
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
    drawMeasurementOverlay();
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
    clearMeasurementOverlay();
    m_needsUpdate = true;
    update();
}

void Viewport3DContent::setMeasurementOverlay(const QList<MeasurementOverlayPoint3D>& points,
                                              const QList<MeasurementOverlayLine3D>& lines) {
    m_measurementPoints = points;
    m_measurementLines = lines;
    update();
}

void Viewport3DContent::clearMeasurementOverlay() {
    if (m_measurementPoints.isEmpty() && m_measurementLines.isEmpty()) {
        return;
    }
    m_measurementPoints.clear();
    m_measurementLines.clear();
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

bool Viewport3DContent::projectToScreen(const QVector3D& worldPos, QPointF* screenPos) const {
    if (!screenPos || width() <= 0 || height() <= 0) {
        return false;
    }

    QRect viewport(0, 0, width(), height());
    const QVector3D projected = worldPos.project(m_viewMatrix, m_projectionMatrix, viewport);
    if (!std::isfinite(projected.x()) || !std::isfinite(projected.y()) || !std::isfinite(projected.z()) ||
        projected.z() < 0.0f || projected.z() > 1.0f) {
        return false;
    }

    // QVector3D::project 返回 OpenGL 视口坐标，Qt 鼠标/绘制坐标需要翻转 Y。
    *screenPos = QPointF(projected.x(), height() - projected.y());
    return true;
}

void Viewport3DContent::drawMeasurementOverlay() {
    if (m_measurementPoints.isEmpty() && m_measurementLines.isEmpty()) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPen linePen(QColor("#06B6D4"), 2.0);
    linePen.setCapStyle(Qt::RoundCap);
    linePen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(linePen);

    for (const MeasurementOverlayLine3D& line : m_measurementLines) {
        QPointF p1;
        QPointF p2;
        if (!projectToScreen(line.p1, &p1) || !projectToScreen(line.p2, &p2)) {
            continue;
        }
        painter.drawLine(p1, p2);

        if (!line.label.isEmpty()) {
            const QPointF mid = (p1 + p2) * 0.5;
            const QRect textRect = painter.fontMetrics()
                                       .boundingRect(line.label)
                                       .adjusted(-4, -2, 4, 2)
                                       .translated(mid.toPoint() + QPoint(8, -8));
            painter.fillRect(textRect, QColor(17, 24, 39, 190));
            painter.setPen(QColor("#F8FAFC"));
            painter.drawText(textRect, Qt::AlignCenter, line.label);
            painter.setPen(linePen);
        }
    }

    QPen pointPen(QColor("#FFF7ED"), 1.5);
    QBrush pointBrush(QColor("#F59E0B"));
    QFont font = painter.font();
    font.setPointSize(10);
    font.setBold(true);
    painter.setFont(font);

    for (const MeasurementOverlayPoint3D& point : m_measurementPoints) {
        QPointF screen;
        if (!projectToScreen(point.pos, &screen)) {
            continue;
        }

        painter.setPen(pointPen);
        painter.setBrush(pointBrush);
        painter.drawEllipse(screen, 5.0, 5.0);

        if (!point.label.isEmpty()) {
            const QRect textRect = painter.fontMetrics()
                                       .boundingRect(point.label)
                                       .adjusted(-4, -2, 4, 2)
                                       .translated(screen.toPoint() + QPoint(8, -14));
            painter.fillRect(textRect, QColor(17, 24, 39, 190));
            painter.setPen(QColor("#F8FAFC"));
            painter.drawText(textRect, Qt::AlignCenter, point.label);
        }
    }
}

void Viewport3DContent::beginInteraction() {
    if (m_isInteracting) {
        return;
    }
    m_isInteracting = true;
    if (m_renderer) {
        m_renderer->setInteractionActive(true);
    }
}

void Viewport3DContent::endInteraction() {
    if (!m_isInteracting) {
        return;
    }
    flushPendingInteraction();
    m_isInteracting = false;
    if (m_renderer) {
        m_renderer->setInteractionActive(false);
        m_renderer->updateLODForDistance(m_camera.distance());
    }
    m_needsUpdate = true;
    update();
}

void Viewport3DContent::flushPendingInteraction() {
    const QPoint delta = m_pendingMouseDelta;
    const Qt::MouseButtons buttons = m_pendingMouseButtons;
    m_pendingMouseDelta = QPoint();
    m_pendingMouseButtons = Qt::NoButton;

    if (delta.isNull()) {
        return;
    }

    float orbitSens = 0.005f;
    float d = std::max(m_camera.distance(), 1.0f);
    float moveScale = d * 0.002f;

    if (buttons & Qt::LeftButton) {
        m_camera.orbit(delta.x() * orbitSens, -delta.y() * orbitSens);
    } else if (buttons & Qt::RightButton) {
        m_camera.pan(-delta.x() * moveScale, delta.y() * moveScale);
    } else if (buttons & Qt::MiddleButton) {
        m_camera.zoom(delta.y() * moveScale);
    }

    m_needsUpdate = true;
    update();
}

void Viewport3DContent::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    if (event->button() == Qt::LeftButton) {
        m_pickPressPos = event->pos();
    }
    if (event->buttons() & (Qt::LeftButton | Qt::RightButton | Qt::MiddleButton)) {
        beginInteraction();
    }
}

void Viewport3DContent::mouseMoveEvent(QMouseEvent* event) {
    const Qt::MouseButtons buttons = event->buttons() & (Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    const QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    if (!buttons || delta.isNull()) {
        return;
    }

    beginInteraction();
    m_pendingMouseDelta += delta;
    m_pendingMouseButtons = buttons;
    if (!m_interactionTimer.isActive()) {
        m_interactionTimer.start();
    }
}

void Viewport3DContent::mouseReleaseEvent(QMouseEvent* event) {
    if (!(event->buttons() & (Qt::LeftButton | Qt::RightButton | Qt::MiddleButton))) {
        endInteraction();
    }

    // 拾取模式：左键短点击（非拖拽）拾取最近的 3D 点
    // 非拾取模式：需要 Ctrl+左键
    const bool isPickClick = event->button() == Qt::LeftButton &&
                             (m_pickMode || event->modifiers() == Qt::ControlModifier);

    if (isPickClick) {
        // 判断是否为点击（移动距离小于阈值）而非拖拽
        if (m_pickMode) {
            const QPoint delta = event->pos() - m_pickPressPos;
            const int distSq = delta.x() * delta.x() + delta.y() * delta.y();
            if (distSq > 25) { // 5px threshold squared
                return; // 是拖拽，不拾取
            }
        }

        if (m_lastPoints.empty())
            return;

        // Update matrices for current viewport dimensions
        updateMatrices();

        QPointF clickPos = event->pos();

        float bestDist = 144.0f; // 12px squared
        int bestIdx = -1;

        // ponytail: O(n) picking is enough for interactive setup; replace with a spatial index if latency shows up.
        for (size_t i = 0; i < m_lastPoints.size(); ++i) {
            QPointF projected;
            if (!projectToScreen(m_lastPoints[i], &projected)) {
                continue;
            }
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
