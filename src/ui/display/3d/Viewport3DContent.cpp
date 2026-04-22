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
    , m_mouseDown(false)
    , m_lodEnabled(true)
{
    // 允许接受焦点以便接收键盘事件
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

Viewport3DContent::~Viewport3DContent() {
    makeCurrent();
    m_renderer.reset();  // 清理渲染器资源
    doneCurrent();
}

void Viewport3DContent::initializeGL() {
    // 初始化渲染器
    m_renderer = std::make_unique<PointCloudRendererOpenGL>();
    m_renderer->initializeGL();
    m_renderer->setPointSize(3.0f);
    m_renderer->setUniformColor(Qt::white);

    // 初始化相机
    m_camera.reset();

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

    // 转换为 GPU 缓冲区
    m_gpuBuffer.fromPointCloudData(*pcData);
    m_renderer->setPointCloud(m_gpuBuffer, m_lodEnabled);
    m_renderer->setLODEnabled(m_lodEnabled);
    m_renderer->scheduleRedraw();

    m_needsUpdate = true;
    update();
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
    m_camera.reset();
    updateMatrices();
    m_needsUpdate = true;
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
    if (event->button() == Qt::LeftButton ||
        event->button() == Qt::MiddleButton ||
        event->button() == Qt::RightButton) {
        m_lastMousePos = event->pos();
        m_mouseDown = true;
    }
}

void Viewport3DContent::mouseMoveEvent(QMouseEvent* event) {
    if (!m_mouseDown) return;

    QPoint delta = event->pos() - m_lastMousePos;
    float sensitivity = 0.005f;

    if (event->buttons() & Qt::LeftButton) {
        // 左键旋转
        m_camera.orbit(delta.x() * sensitivity, -delta.y() * sensitivity);
        m_needsUpdate = true;
        update();
    } else if (event->buttons() & Qt::MiddleButton) {
        // 中键平移
        m_camera.pan(-delta.x() * 0.01f, delta.y() * 0.01f);
        m_needsUpdate = true;
        update();
    } else if (event->buttons() & Qt::RightButton) {
        // 右键缩放
        m_camera.zoom(delta.y() * 0.01f);
        m_needsUpdate = true;
        update();
    }

    m_lastMousePos = event->pos();
}

void Viewport3DContent::wheelEvent(QWheelEvent* event) {
    float delta = event->angleDelta().y() * 0.001f;
    m_camera.zoom(delta);
    m_needsUpdate = true;
    update();
}

} // namespace DeepLux
