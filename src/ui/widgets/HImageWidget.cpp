#include "HImageWidget.h"

#include <QDebug>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QWheelEvent>
#include <cmath>

namespace DeepLux {

HImageWidget::HImageWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // Auto-fill background so stylesheet color is applied before paintEvent
    setAutoFillBackground(true);
    setAttribute(Qt::WA_StyledBackground, true);
}

HImageWidget::~HImageWidget() {}

void HImageWidget::setImage(const QImage& image) {
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

void HImageWidget::clearImage() {
    m_image = QImage();
    m_hasImage = false;
    m_imageWidth = 0;
    m_imageHeight = 0;
    m_zoom = 1.0;
    m_offset = QPointF();
    m_measurementPoints.clear();
    m_measurementLines.clear();
    updateTransform();
    update();
}

void HImageWidget::setZoom(double factor) {
    factor = qBound(0.1, factor, 20.0);
    if (!qFuzzyCompare(m_zoom, factor)) {
        m_zoom = factor;
        updateTransform();
        update();
        emit zoomChanged(factor);
    }
}

void HImageWidget::fitToWindow() {
    if (!m_hasImage)
        return;

    double scaleX = (double) width() / m_imageWidth;
    double scaleY = (double) height() / m_imageHeight;
    m_zoom = qMin(scaleX, scaleY) * 0.95;

    // 居中
    double imgW = m_imageWidth * m_zoom;
    double imgH = m_imageHeight * m_zoom;
    m_offset = QPointF((width() - imgW) / 2.0, (height() - imgH) / 2.0);

    updateTransform();
    update();
    emit zoomChanged(m_zoom);
}

void HImageWidget::actualSize() {
    m_zoom = 1.0;
    m_offset = QPointF((width() - m_imageWidth) / 2.0, (height() - m_imageHeight) / 2.0);
    updateTransform();
    update();
    emit zoomChanged(m_zoom);
}

void HImageWidget::setRoiMode(RoiType type) {
    m_roiMode = type;
    m_isDrawing = false;
}

void HImageWidget::clearRois() {
    m_rois.clear();
    update();
}

void HImageWidget::setMeasurementOverlay(const QList<MeasurementOverlayPoint>& points,
                                         const QList<MeasurementOverlayLine>& lines) {
    m_measurementPoints = points;
    m_measurementLines = lines;
    update();
}

void HImageWidget::clearMeasurementOverlay() {
    m_measurementPoints.clear();
    m_measurementLines.clear();
    update();
}

void HImageWidget::updateTransform() {
    m_transform = QTransform();
    m_transform.translate(m_offset.x(), m_offset.y());
    m_transform.scale(m_zoom, m_zoom);
    m_inverseTransform = m_transform.inverted();
}

QPointF HImageWidget::widgetToImage(const QPointF& widgetPoint) const {
    return m_inverseTransform.map(widgetPoint);
}

QPointF HImageWidget::imageToWidget(const QPointF& imagePoint) const {
    return m_transform.map(imagePoint);
}

void HImageWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::TextAntialiasing);

    painter.fillRect(rect(), palette().color(QPalette::Window));

    QStyleOption option;
    option.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);

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

        if (!m_measurementLines.isEmpty() || !m_measurementPoints.isEmpty()) {
            QPen linePen(QColor("#06B6D4"), 2.0);
            linePen.setCosmetic(true);
            painter.setPen(linePen);
            painter.setBrush(Qt::NoBrush);
            for (const MeasurementOverlayLine& line : m_measurementLines) {
                const QPointF p1 = imageToWidget(line.p1);
                const QPointF p2 = imageToWidget(line.p2);
                painter.drawLine(p1, p2);
                if (!line.label.isEmpty()) {
                    const QPointF mid = (p1 + p2) / 2.0;
                    QPointF dir = p2 - p1;
                    qreal len = sqrt(dir.x() * dir.x() + dir.y() * dir.y());
                    QPointF normal(-dir.y(), dir.x());
                    if (len > 1.0) {
                        normal /= len;
                    } else {
                        normal = QPointF(0, -1);
                    }
                    const QPointF labelPos = mid + normal * 14;

                    QFont font = painter.font();
                    font.setBold(true);
                    font.setPointSize(9);
                    painter.setFont(font);
                    const QRectF textRect = painter.boundingRect(QRectF(), Qt::AlignCenter, line.label);
                    const QRectF bgRect = textRect.translated(labelPos - textRect.center()).adjusted(-5, -3, 5, 3);

                    painter.setBrush(QColor(15, 23, 42, 200));
                    painter.setPen(QPen(QColor("#06B6D4"), 1));
                    painter.drawRoundedRect(bgRect, 4, 4);

                    painter.setPen(QColor("#E0F2FE"));
                    painter.drawText(bgRect, Qt::AlignCenter, line.label);
                    painter.setPen(linePen);
                    painter.setBrush(Qt::NoBrush);
                }
            }

            QFont labelFont = painter.font();
            labelFont.setBold(true);
            labelFont.setPointSize(9);
            painter.setFont(labelFont);
            for (const MeasurementOverlayPoint& point : m_measurementPoints) {
                const QPointF widgetPoint = imageToWidget(point.pos);
                painter.setPen(QPen(QColor("#111827"), 2));
                painter.setBrush(QColor("#F59E0B"));
                painter.drawEllipse(widgetPoint, 5.5, 5.5);
                if (!point.label.isEmpty()) {
                    const QPointF textPos = widgetPoint + QPointF(9, -6);
                    painter.setPen(QColor(0, 0, 0, 200));
                    painter.drawText(textPos + QPointF(1, 1), point.label);
                    painter.setPen(QColor("#FEF3C7"));
                    painter.drawText(textPos, point.label);
                }
            }
        }

        return;
    }

    const QColor background = palette().color(QPalette::Window);
    const bool isLight = background.lightness() > 128;
    const QColor primary = isLight ? QColor("#4b5563") : QColor("#d1d5db");
    const QColor secondary = isLight ? QColor("#6b7280") : QColor("#9ca3af");

    QFont titleFont = painter.font();
    titleFont.setPointSize(rect().width() < 360 ? qMax(9, titleFont.pointSize()) : qMax(10, titleFont.pointSize() + 1));
    titleFont.setBold(true);

    QRect titleRect = rect().adjusted(24, 0, -24, 0);
    titleRect.setHeight(rect().width() < 360 ? 56 : 34);
    titleRect.moveCenter(QPoint(rect().center().x(), rect().center().y() - 14));

    painter.setPen(primary);
    painter.setFont(titleFont);
    painter.drawText(titleRect, Qt::AlignCenter | Qt::TextWordWrap, tr("拖入图像或点云，或运行流程后显示结果"));

    QFont hintFont = painter.font();
    hintFont.setBold(false);
    hintFont.setPointSize(qMax(9, hintFont.pointSize() - 1));
    painter.setFont(hintFont);
    painter.setPen(secondary);
    QRect hintRect = titleRect.translated(0, rect().width() < 360 ? 54 : 30);
    hintRect.setHeight(48);
    painter.drawText(hintRect, Qt::AlignCenter | Qt::TextWordWrap, tr("支持图片、PLY/TIFF 点云和流程模块输出"));
}

void HImageWidget::resizeEvent(QResizeEvent* event) {
    Q_UNUSED(event)
    if (m_hasImage) {
        fitToWindow();
    }
}

void HImageWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_clickPressPos = event->pos();
        m_clickIsLeftButton = true;
    }

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

void HImageWidget::mouseMoveEvent(QMouseEvent* event) {
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

void HImageWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (m_clickIsLeftButton) {
            QPointF delta = event->pos() - m_clickPressPos;
            double dist = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
            if (dist < 5.0 && m_hasImage && m_roiMode == RoiType::None) {
                QPointF imgPos = widgetToImage(event->pos());
                emit imageClicked(imgPos);
            }
        }
        m_clickIsLeftButton = false;
    }

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

void HImageWidget::wheelEvent(QWheelEvent* event) {
    if (!m_hasImage)
        return;

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
