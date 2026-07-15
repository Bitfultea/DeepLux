#include "AnnotationCoordinateMapping.h"

#include <algorithm>

namespace DeepLux {

AnnotationCoordinateMapping::AnnotationCoordinateMapping(const QSizeF& originalSize, const QSizeF& modelInputSize)
    : m_originalSize(originalSize), m_modelInputSize(modelInputSize) {
    if (originalSize.width() > 0 && originalSize.height() > 0 && modelInputSize.width() > 0 &&
        modelInputSize.height() > 0) {
        const double scale =
            std::min(modelInputSize.width() / originalSize.width(), modelInputSize.height() / originalSize.height());
        m_scaleX = scale;
        m_scaleY = scale;
        m_offsetX = (modelInputSize.width() - originalSize.width() * scale) / 2.0;
        m_offsetY = (modelInputSize.height() - originalSize.height() * scale) / 2.0;
    }
}

QPointF AnnotationCoordinateMapping::toModel(const QPointF& originalPoint) const {
    return QPointF(originalPoint.x() * m_scaleX + m_offsetX, originalPoint.y() * m_scaleY + m_offsetY);
}

QRectF AnnotationCoordinateMapping::toModel(const QRectF& originalRect) const {
    return QRectF(originalRect.x() * m_scaleX + m_offsetX, originalRect.y() * m_scaleY + m_offsetY,
                  originalRect.width() * m_scaleX, originalRect.height() * m_scaleY);
}

QList<QPointF> AnnotationCoordinateMapping::toModel(const QList<QPointF>& originalPolygon) const {
    QList<QPointF> result;
    result.reserve(originalPolygon.size());
    for (const QPointF& p : originalPolygon) {
        result.append(toModel(p));
    }
    return result;
}

QPointF AnnotationCoordinateMapping::toOriginal(const QPointF& modelPoint) const {
    return QPointF((modelPoint.x() - m_offsetX) / m_scaleX, (modelPoint.y() - m_offsetY) / m_scaleY);
}

QRectF AnnotationCoordinateMapping::toOriginal(const QRectF& modelRect) const {
    return QRectF((modelRect.x() - m_offsetX) / m_scaleX, (modelRect.y() - m_offsetY) / m_scaleY,
                  modelRect.width() / m_scaleX, modelRect.height() / m_scaleY);
}

QList<QPointF> AnnotationCoordinateMapping::toOriginal(const QList<QPointF>& modelPolygon) const {
    QList<QPointF> result;
    result.reserve(modelPolygon.size());
    for (const QPointF& p : modelPolygon) {
        result.append(toOriginal(p));
    }
    return result;
}

} // namespace DeepLux
