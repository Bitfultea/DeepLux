#include "AnnotationOverlayWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QCoreApplication>

namespace DeepLux {

AnnotationOverlayWidget::AnnotationOverlayWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAutoFillBackground(false);
}

void AnnotationOverlayWidget::setCoordConverter(CoordConverter imageToWidget) {
    m_imageToWidget = imageToWidget;
    update();
}

void AnnotationOverlayWidget::setInverseCoordConverter(CoordConverter widgetToImage) {
    m_widgetToImage = widgetToImage;
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

void AnnotationOverlayWidget::setPreviewMask(const QImage& maskImage) {
    m_previewMask = maskImage;
    update();
}

void AnnotationOverlayWidget::clearPreview() {
    m_previewPolygon.clear();
    m_previewBox = QRectF();
    m_previewMask = QImage();
    update();
}

void AnnotationOverlayWidget::setObjectMask(const QString& id, const QImage& maskImage) {
    m_objectMasks.insert(id, maskImage);
    update();
}

void AnnotationOverlayWidget::clearObjectMask(const QString& id) {
    m_objectMasks.remove(id);
    update();
}

QImage AnnotationOverlayWidget::objectMask(const QString& id) const {
    return m_objectMasks.value(id);
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
    if (!m_imageToWidget)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 绘制已确认的标注对象
    for (const AnnotationObject& obj : m_annotations) {
        bool isSelected = (obj.id == m_selectedId);

        // 绘制对象 mask（如果有）— mask 是整图大小的 RGBA 图
        const QImage objMask = m_objectMasks.value(obj.id);
        if (!objMask.isNull() && m_imageToWidget) {
            // Fix P1-5: mask 是整图坐标空间，从 (0,0) 到 (maskW, maskH) 映射到 widget
            QPointF tl = m_imageToWidget(QPointF(0, 0));
            QPointF br = m_imageToWidget(QPointF(objMask.width(), objMask.height()));
            QRectF widgetTarget(tl, br);
            painter.drawImage(widgetTarget, objMask, QRectF(0, 0, objMask.width(), objMask.height()));
        }

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

    // 绘制预览 mask（如果有）— 整图大小的 mask
    if (!m_previewMask.isNull() && m_imageToWidget) {
        // Fix P1-5: mask 是整图坐标空间
        QPointF tl = m_imageToWidget(QPointF(0, 0));
        QPointF br = m_imageToWidget(QPointF(m_previewMask.width(), m_previewMask.height()));
        QRectF widgetTarget(tl, br);
        painter.drawImage(widgetTarget, m_previewMask, QRectF(0, 0, m_previewMask.width(), m_previewMask.height()));
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
        QRectF widgetRect =
            QRectF(m_imageToWidget(m_previewBox.topLeft()), m_imageToWidget(m_previewBox.bottomRight()));
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
    // 中键和 Ctrl+左键转发给父 HImageWidget（平移/缩放）
    if (event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && event->modifiers() & Qt::ControlModifier)) {
        if (auto* p = parentWidget())
            QCoreApplication::sendEvent(p, event);
        return;
    }

    if (!m_widgetToImage)
        return;

    const QPointF widgetPoint = event->pos();
    const QPointF imagePoint = m_widgetToImage(widgetPoint);

    if (m_mode == Mode::Box && event->button() == Qt::LeftButton) {
        m_isDragging = true;
        m_dragStart = widgetPoint;
        emit dragStarted(imagePoint);
    } else if ((m_mode == Mode::PositivePoint || m_mode == Mode::NegativePoint) &&
               (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
        emit widgetClicked(imagePoint, event->button());
    } else if (m_mode == Mode::Select && event->button() == Qt::LeftButton) {
        emit widgetClicked(imagePoint, event->button());
    }
}

void AnnotationOverlayWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging) {
        QRectF dragRect(m_dragStart, event->pos());
        m_dragBox = dragRect.normalized();
        if (m_widgetToImage)
            emit dragMoved(m_widgetToImage(event->pos()));
        update();
    } else if (event->buttons() & Qt::MiddleButton) {
        // Fix P1-7: 中键移动转发给父 HImageWidget（平移）
        if (auto* p = parentWidget())
            QCoreApplication::sendEvent(p, event);
    }
}

void AnnotationOverlayWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        if (auto* p = parentWidget())
            QCoreApplication::sendEvent(p, event);
        return;
    }
    if (m_isDragging && event->button() == Qt::LeftButton) {
        m_isDragging = false;
        emit dragEnded(m_widgetToImage ? m_widgetToImage(m_dragStart) : m_dragStart,
                       m_widgetToImage ? m_widgetToImage(event->pos()) : QPointF(event->pos()));
        m_dragBox = QRectF();
        update();
    }
}

void AnnotationOverlayWidget::wheelEvent(QWheelEvent* event) {
    // 滚轮缩放转发给父 HImageWidget
    if (auto* p = parentWidget())
        QCoreApplication::sendEvent(p, event);
}

} // namespace DeepLux
