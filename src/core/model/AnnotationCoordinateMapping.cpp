#include "AnnotationCoordinateMapping.h"

namespace DeepLux {

AnnotationCoordinateMapping::AnnotationCoordinateMapping(const QSizeF& originalSize, const QSizeF& modelInputSize)
    : m_originalSize(originalSize), m_modelInputSize(modelInputSize)
{
    if (originalSize.width() > 0 && originalSize.height() > 0 &&
        modelInputSize.width() > 0 && modelInputSize.height() > 0) {
        m_scaleX = modelInputSize.width() / originalSize.width();
        m_scaleY = modelInputSize.height() / originalSize.height();
    }
}

QPointF AnnotationCoordinateMapping::toModel(const QPointF& originalPoint) const {
    return QPointF(originalPoint.x() * m_scaleX, originalPoint.y() * m_scaleY);
}

QRectF AnnotationCoordinateMapping::toModel(const QRectF& originalRect) const {
    return QRectF(originalRect.x() * m_scaleX, originalRect.y() * m_scaleY,
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
    return QPointF(modelPoint.x() / m_scaleX, modelPoint.y() / m_scaleY);
}

QRectF AnnotationCoordinateMapping::toOriginal(const QRectF& modelRect) const {
    return QRectF(modelRect.x() / m_scaleX, modelRect.y() / m_scaleY,
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
