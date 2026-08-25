#pragma once

#include "core/model/Annotation.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>
#include <functional>

namespace DeepLux {

/**
 * @brief 标注叠加层 — 透明覆盖在图像显示控件上
 *
 * 接收原图坐标的标注对象，由宿主提供坐标转换函数。
 * 不修改 HImageWidget 状态。
 */
class AnnotationOverlayWidget : public QWidget {
    Q_OBJECT

public:
    using CoordConverter = std::function<QPointF(const QPointF&)>;

    explicit AnnotationOverlayWidget(QWidget* parent = nullptr);

    void setCoordConverter(CoordConverter imageToWidget);
    void setInverseCoordConverter(CoordConverter widgetToImage);

    // 设置已确认的标注对象列表
    void setAnnotations(const QList<AnnotationObject>& objects);

    // 设置当前预览（未确认的 mask 预览）
    void setPreviewPolygon(const QList<QPointF>& polygon);
    void setPreviewBox(const QRectF& box);
    void setPreviewMask(const QImage& maskImage);
    void clearPreview();

    // 设置已确认对象的 mask（按对象 id）
    void setObjectMask(const QString& id, const QImage& maskImage);
    void clearObjectMask(const QString& id);
    QImage objectMask(const QString& id) const;

    // 设置当前 prompt 点
    void setPromptPoints(const QList<QPointF>& positive, const QList<QPointF>& negative);
    void clearPromptPoints();

    // 设置/获取选中对象 ID
    void setSelectedId(const QString& id);
    QString selectedId() const {
        return m_selectedId;
    }

    // 交互模式
    enum class Mode { Select, PositivePoint, NegativePoint, Box };
    void setMode(Mode mode) {
        m_mode = mode;
        update();
    }
    Mode mode() const {
        return m_mode;
    }

    // 正在拖拽的框选（屏幕坐标）
    void setDragBox(const QRectF& box);
    void clearDragBox();

    // 访问当前预览 mask（供测试）
    QImage previewMask() const {
        return m_previewMask;
    }

signals:
    void widgetClicked(const QPointF& imagePoint, Qt::MouseButton button);
    void dragStarted(const QPointF& imagePoint);
    void dragMoved(const QPointF& imagePoint);
    void dragEnded(const QPointF& imageStart, const QPointF& imageEnd);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    CoordConverter m_imageToWidget;
    CoordConverter m_widgetToImage;
    QList<AnnotationObject> m_annotations;
    QList<QPointF> m_previewPolygon;
    QRectF m_previewBox;
    QImage m_previewMask;
    QHash<QString, QImage> m_objectMasks;
    QList<QPointF> m_positivePoints;
    QList<QPointF> m_negativePoints;
    QString m_selectedId;
    Mode m_mode = Mode::Select;
    QRectF m_dragBox;
    bool m_isDragging = false;
    QPointF m_dragStart;
};

} // namespace DeepLux
