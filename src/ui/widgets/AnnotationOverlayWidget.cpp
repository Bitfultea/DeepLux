#include "AnnotationOverlayWidget.h"

#include <QPainter>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPainterPathStroker>

namespace DeepLux {

AnnotationOverlayWidget::AnnotationOverlayWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAutoFillBackground(false);
}

void AnnotationOverlayWidget::setCoordConverter(CoordConverter imageToWidget) {
    m_imageToWidget = imageToWidget;
    update();
}

void AnnotationOverlayWidget::setAnnotations(const QList<AnnotationObject>& objects) {
    m_annotations = objects;
    update();
}

void AnnotationOverlayWidget::setPreviewPolygon(const QList<QPointF>& polygon) {
    m_previewPolygon = polygon;
    update();
}

void AnnotationOverlayWidget::setPreviewBox(const QRectF& box) {
    m_previewBox = box;
    update();
}

void AnnotationOverlayWidget::clearPreview() {
    m_previewPolygon.clear();
    m_previewBox = QRectF();
    update();
}

void AnnotationOverlayWidget::setPromptPoints(const QList<QPointF>& positive, const QList<QPointF>& negative) {
    m_positivePoints = positive;
    m_negativePoints = negative;
    update();
}

void AnnotationOverlayWidget::clearPromptPoints() {
    m_positivePoints.clear();
    m_negativePoints.clear();
    update();
}

void AnnotationOverlayWidget::setSelectedId(const QString& id) {
    m_selectedId = id;
    update();
}

void AnnotationOverlayWidget::setDragBox(const QRectF& box) {
    m_dragBox = box;
    update();
}

void AnnotationOverlayWidget::clearDragBox() {
    m_dragBox = QRectF();
    update();
}

void AnnotationOverlayWidget::paintEvent(QPaintEvent*) {
    if (!m_imageToWidget) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 绘制已确认的标注对象
    for (const AnnotationObject& obj : m_annotations) {
        bool isSelected = (obj.id == m_selectedId);

        // 绘制 polygon 轮廓
        if (obj.polygon.size() >= 2) {
            QPainterPath path;
            QPointF first = m_imageToWidget(obj.polygon[0]);
            path.moveTo(first);
            for (int i = 1; i < obj.polygon.size(); ++i) {
                path.lineTo(m_imageToWidget(obj.polygon[i]));
            }
            path.closeSubpath();

            // 填充（半透明）
            QColor fillColor = isSelected ? QColor(6, 182, 212, 60) : QColor(6, 182, 212, 30);
            painter.fillPath(path, fillColor);

            // 轮廓线
            QPen pen(isSelected ? QColor("#06B6D4") : QColor("#64748B"), 2);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.drawPath(path);
        }
    }

    // 绘制预览 polygon
    if (m_previewPolygon.size() >= 2) {
        QPainterPath path;
        path.moveTo(m_imageToWidget(m_previewPolygon[0]));
        for (int i = 1; i < m_previewPolygon.size(); ++i) {
            path.lineTo(m_imageToWidget(m_previewPolygon[i]));
        }
        path.closeSubpath();

        painter.fillPath(path, QColor(245, 158, 11, 50));
        QPen pen(QColor("#F59E0B"), 2);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawPath(path);
    }

    // 绘制预览框
    if (m_previewBox.isValid() && !m_previewBox.isEmpty()) {
        QRectF widgetRect = QRectF(
            m_imageToWidget(m_previewBox.topLeft()),
            m_imageToWidget(m_previewBox.bottomRight())
        );
        QPen pen(QColor("#F59E0B"), 2, Qt::DashLine);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawRect(widgetRect);
    }

    // 绘制拖拽框
    if (m_dragBox.isValid() && !m_dragBox.isEmpty()) {
        QPen pen(QColor("#06B6D4"), 2, Qt::DashLine);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(QColor(6, 182, 212, 20));
        painter.drawRect(m_dragBox);
    }

    // 绘制正点（绿色）
    painter.setPen(QPen(QColor("#111827"), 2));
    painter.setBrush(QColor("#22C55E"));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(8);
    painter.setFont(font);
    for (const QPointF& p : m_positivePoints) {
        QPointF wp = m_imageToWidget(p);
        painter.drawEllipse(wp, 6, 6);
    }

    // 绘制负点（红色）
    painter.setBrush(QColor("#EF4444"));
    for (const QPointF& p : m_negativePoints) {
        QPointF wp = m_imageToWidget(p);
        painter.drawEllipse(wp, 6, 6);
    }
}

void AnnotationOverlayWidget::mousePressEvent(QMouseEvent* event) {
    if (!m_imageToWidget) return;

    // We need the inverse converter; but we only have imageToWidget.
    // The host should provide widgetToImage. For simplicity, we emit
    // widget coordinates and let the host convert.
    // Actually, let's emit in image coordinates by using widgetToImage.
    // Since we don't have widgetToImage, we emit widget coords and let
    // the host convert. But the signal says imagePoint...

    // Simplest: emit widget coords, host converts to image coords.
    // But signal signature says imagePoint. Let's change approach:
    // emit widget coords and let host handle conversion.

    QPointF widgetPoint = event->pos();
    // We'll just emit the widget point; the host can convert.
    // Actually the signal says imagePoint, so we need inverse.
    // Let's just emit widgetPoint for now and have the host do conversion.
    // For a clean implementation, we'd need a widgetToImage converter too.

    if (m_mode == Mode::Box && event->button() == Qt::LeftButton) {
        m_isDragging = true;
        m_dragStart = widgetPoint;
        emit dragStarted(widgetPoint);
    } else if ((m_mode == Mode::PositivePoint || m_mode == Mode::NegativePoint) &&
               event->button() == Qt::LeftButton) {
        emit widgetClicked(widgetPoint, Qt::LeftButton);
    } else if (m_mode == Mode::Select && event->button() == Qt::LeftButton) {
        emit widgetClicked(widgetPoint, Qt::LeftButton);
    }
}

void AnnotationOverlayWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging) {
        QRectF dragRect(m_dragStart, event->pos());
        m_dragBox = dragRect.normalized();
        emit dragMoved(event->pos());
        update();
    }
}

void AnnotationOverlayWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (m_isDragging && event->button() == Qt::LeftButton) {
        m_isDragging = false;
        QRectF dragRect(m_dragStart, event->pos());
        emit dragEnded(m_dragStart, event->pos());
        m_dragBox = QRectF();
        update();
    }
}

} // namespace DeepLux
