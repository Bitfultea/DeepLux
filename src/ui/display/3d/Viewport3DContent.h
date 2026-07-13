#pragma once

#include "../IViewportContent.h"
#include "CameraController.h"
#include "IPointCloudRenderer.h"
#include "PointCloudRendererOpenGL.h"

#include <QList>
#include <QMatrix4x4>
#include <QOpenGLWidget>
#include <QString>
#include <QTimer>
#include <QVector3D>
#include <vector>

namespace DeepLux {

struct MeasurementOverlayPoint3D {
    QVector3D pos;
    QString label;
};

struct MeasurementOverlayLine3D {
    QVector3D p1;
    QVector3D p2;
    QString label;
};

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
    QWidget* toolbarExtension() override {
        return nullptr;
    }
    QWidget* widget() override {
        return this;
    }

    // 渲染模式
    ColorMode renderMode() const {
        return m_renderMode;
    }
    void setRenderMode(ColorMode mode);
    void setMeasurementOverlay(const QList<MeasurementOverlayPoint3D>& points,
                               const QList<MeasurementOverlayLine3D>& lines);
    void clearMeasurementOverlay();
    void setPickMode(bool enabled) { m_pickMode = enabled; }

public slots:
    void resetCamera();

    // LOD 控制
    void setLODEnabled(bool enabled);
    bool isLODEnabled() const;
    void applyTheme(bool isDark);

signals:
    void pointClicked(int index, const QVector3D& point);
    void point3DClicked(const QVector3D& worldPos);

protected:
    // QOpenGLWidget
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    // 鼠标事件
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void updateMatrices();
    void beginInteraction();
    void endInteraction();
    void flushPendingInteraction();
    bool projectToScreen(const QVector3D& worldPos, QPointF* screenPos) const;
    void drawMeasurementOverlay();

    std::unique_ptr<PointCloudRendererOpenGL> m_renderer;
    PointCloudGPUBuffer m_gpuBuffer;
    CameraController m_camera;
    ColorMode m_renderMode = ColorMode::BlinnPhong;

    QMatrix4x4 m_projectionMatrix;
    QMatrix4x4 m_viewMatrix;

    bool m_needsUpdate = true;
    QPoint m_lastMousePos;
    QPoint m_pendingMouseDelta;
    Qt::MouseButtons m_pendingMouseButtons = Qt::NoButton;
    QTimer m_interactionTimer;
    bool m_isInteracting = false;
    bool m_lodEnabled = true;

    QVector3D m_bboxMin;
    QVector3D m_bboxMax;
    bool m_hasBbox = false;

    // 最近显示的3D点（用于 Ctrl+Click 坐标拾取）
    std::vector<QVector3D> m_lastPoints;
    QList<MeasurementOverlayPoint3D> m_measurementPoints;
    QList<MeasurementOverlayLine3D> m_measurementLines;
    bool m_pickMode = false;
    QPoint m_pickPressPos;
};

} // namespace DeepLux
