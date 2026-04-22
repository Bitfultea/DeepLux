#include "HImageWidget.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDebug>

namespace DeepLux {

HImageWidget::HImageWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // Auto-fill background so stylesheet color is applied before paintEvent
    setAutoFillBackground(true);
}

HImageWidget::~HImageWidget()
{
}

void HImageWidget::setImage(const QImage& image)
{
    m_image = image;
    m_hasImage = !image.isNull();
    
    if (m_hasImage) {
        m_imageWidth = image.width();
        m_imageHeight = image.height();
        fitToWindow();
        emit imageLoaded(image);
    } else {
        m_imageWidth = 0;
        m_imageHeight = 0;
    }
    
    update();
}

void HImageWidget::clearImage()
{
    m_image = QImage();
    m_hasImage = false;
    m_imageWidth = 0;
    m_imageHeight = 0;
    m_zoom = 1.0;
    m_offset = QPointF();
    updateTransform();
    update();
}

void HImageWidget::setZoom(double factor)
{
    factor = qBound(0.1, factor, 20.0);
    if (!qFuzzyCompare(m_zoom, factor)) {
        m_zoom = factor;
        updateTransform();
        update();
        emit zoomChanged(factor);
    }
}

void HImageWidget::fitToWindow()
{
    if (!m_hasImage) return;
    
    double scaleX = (double)width() / m_imageWidth;
    double scaleY = (double)height() / m_imageHeight;
    m_zoom = qMin(scaleX, scaleY) * 0.95;
    
    // 居中
    double imgW = m_imageWidth * m_zoom;
    double imgH = m_imageHeight * m_zoom;
    m_offset = QPointF((width() - imgW) / 2.0, (height() - imgH) / 2.0);
    
    updateTransform();
    update();
    emit zoomChanged(m_zoom);
}

void HImageWidget::actualSize()
{
    m_zoom = 1.0;
    m_offset = QPointF((width() - m_imageWidth) / 2.0, (height() - m_imageHeight) / 2.0);
    updateTransform();
    update();
    emit zoomChanged(m_zoom);
}

void HImageWidget::setRoiMode(RoiType type)
{
    m_roiMode = type;
    m_isDrawing = false;
}

void HImageWidget::clearRois()
{
    m_rois.clear();
    update();
}

void HImageWidget::updateTransform()
{
    m_transform = QTransform();
    m_transform.translate(m_offset.x(), m_offset.y());
    m_transform.scale(m_zoom, m_zoom);
    m_inverseTransform = m_transform.inverted();
}

QPointF HImageWidget::widgetToImage(const QPointF& widgetPoint) const
{
    return m_inverseTransform.map(widgetPoint);
}

QPointF HImageWidget::imageToWidget(const QPointF& imagePoint) const
{
    return m_transform.map(imagePoint);
}

void HImageWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    if (m_hasImage) {
        // 绘制图像
        QRectF targetRect = QRectF(m_offset, QSizeF(m_imageWidth * m_zoom, m_imageHeight * m_zoom));
        painter.drawImage(targetRect, m_image, QRectF(0, 0, m_imageWidth, m_imageHeight));
        
        // 绘制 ROI
        painter.setPen(QPen(Qt::green, 1));
        for (const RoiData& roi : m_rois) {
            QPointF p1 = imageToWidget(QPointF(roi.x1, roi.y1));
            QPointF p2 = imageToWidget(QPointF(roi.x2, roi.y2));
            painter.drawRect(QRectF(p1, p2).normalized());
        }
        
        // 绘制中的 ROI
        if (m_isDrawing) {
            painter.setPen(QPen(Qt::yellow, 2, Qt::DashLine));
            QPointF p1 = imageToWidget(m_drawStart);
            QPointF p2 = imageToWidget(m_drawEnd);
            painter.drawRect(QRectF(p1, p2).normalized());
        }
    }
    
    // 绘制信息
    painter.setPen(Qt::white);
    painter.drawText(10, 20, QString("Zoom: %1%").arg(m_zoom * 100, 0, 'f', 1));
    if (m_hasImage) {
        painter.drawText(10, 40, QString("Size: %1 x %2").arg(m_imageWidth).arg(m_imageHeight));
    }
}

void HImageWidget::resizeEvent(QResizeEvent* event)
{
    Q_UNUSED(event)
    if (m_hasImage) {
        fitToWindow();
    }
}

void HImageWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_roiMode != RoiType::None && m_hasImage) {
        m_isDrawing = true;
        m_drawStart = widgetToImage(event->pos());
        m_drawEnd = m_drawStart;
    } else if (event->button() == Qt::MiddleButton) {
        m_isPanning = true;
        m_panStart = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
}

void HImageWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_hasImage) {
        QPointF imgPos = widgetToImage(event->pos());
        emit mouseMoved(imgPos);
    }
    
    if (m_isDrawing) {
        m_drawEnd = widgetToImage(event->pos());
        update();
    } else if (m_isPanning) {
        QPointF delta = event->pos() - m_panStart;
        m_offset += delta;
        m_panStart = event->pos();
        updateTransform();
        update();
    }
}

void HImageWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_isDrawing) {
        m_isDrawing = false;
        
        RoiData roi;
        roi.type = m_roiMode;
        roi.x1 = m_drawStart.x();
        roi.y1 = m_drawStart.y();
        roi.x2 = m_drawEnd.x();
        roi.y2 = m_drawEnd.y();
        
        m_rois.append(roi);
        emit roiCreated(roi);
        update();
    } else if (event->button() == Qt::MiddleButton) {
        m_isPanning = false;
        setCursor(Qt::ArrowCursor);
    }
}

void HImageWidget::wheelEvent(QWheelEvent* event)
{
    if (!m_hasImage) return;
    
    double delta = event->angleDelta().y() / 120.0;
    double factor = 1.0 + delta * 0.1;
    
    QPointF mousePos = event->position();
    QPointF imgPos = widgetToImage(mousePos);
    
    setZoom(m_zoom * factor);
    
    // 保持鼠标位置不变
    QPointF newWidgetPos = imageToWidget(imgPos);
    m_offset += mousePos - newWidgetPos;
    updateTransform();
    update();
}

} // namespace DeepLux
