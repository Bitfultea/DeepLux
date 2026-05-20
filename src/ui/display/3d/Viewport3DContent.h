#pragma once

#include "../IViewportContent.h"
#include "PointCloudRendererOpenGL.h"
#include "CameraController.h"
#include "IPointCloudRenderer.h"
#include <QOpenGLWidget>
#include <QMatrix4x4>

namespace DeepLux {

/**
 * @brief 3D 视口内容 Widget
 *
 * 基于 QOpenGLWidget 实现 3D 点云渲染
 * 实现 IViewportContent 接口
 */
class Viewport3DContent : public QOpenGLWidget, public IViewportContent {
    Q_OBJECT

public:
    explicit Viewport3DContent(QWidget* parent = nullptr);
    ~Viewport3DContent() override;

    // IViewportContent
    void displayData(const DisplayData& data) override;
    void clearDisplay() override;
    QWidget* toolbarExtension() override { return nullptr; }
    QWidget* widget() override { return this; }

    // 渲染模式
    ColorMode renderMode() const { return m_renderMode; }
    void setRenderMode(ColorMode mode);

public slots:
    void resetCamera();

    // LOD 控制
    void setLODEnabled(bool enabled);
    bool isLODEnabled() const;

signals:
    void pointClicked(int index, const QVector3D& point);

protected:
    // QOpenGLWidget
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    // 鼠标事件
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void updateMatrices();

    std::unique_ptr<PointCloudRendererOpenGL> m_renderer;
    PointCloudGPUBuffer m_gpuBuffer;
    CameraController m_camera;
    ColorMode m_renderMode = ColorMode::BlinnPhong;

    QMatrix4x4 m_projectionMatrix;
    QMatrix4x4 m_viewMatrix;

    bool m_needsUpdate = true;
    QPoint m_lastMousePos;
    bool m_lodEnabled = true;

    QVector3D m_bboxMin;
    QVector3D m_bboxMax;
    bool m_hasBbox = false;
};

} // namespace DeepLux
