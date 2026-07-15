#pragma once

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>

namespace DeepLux {

/**
 * @brief 原图坐标与模型输入坐标的双向映射
 *
 * SAM 模型通常将最长边缩放到固定尺寸（如 1024），短边按比例缩放后填充。
 * 此类封装缩放比例和偏移，确保所有导出坐标回到原图坐标。
 */
class AnnotationCoordinateMapping {
public:
    AnnotationCoordinateMapping() = default;
    AnnotationCoordinateMapping(const QSizeF& originalSize, const QSizeF& modelInputSize);

    // 原图坐标 → 模型输入坐标
    QPointF toModel(const QPointF& originalPoint) const;
    QRectF toModel(const QRectF& originalRect) const;
    QList<QPointF> toModel(const QList<QPointF>& originalPolygon) const;

    // 模型输入坐标 → 原图坐标
    QPointF toOriginal(const QPointF& modelPoint) const;
    QRectF toOriginal(const QRectF& modelRect) const;
    QList<QPointF> toOriginal(const QList<QPointF>& modelPolygon) const;

    // 访问器
    QSizeF originalSize() const {
        return m_originalSize;
    }
    QSizeF modelInputSize() const {
        return m_modelInputSize;
    }
    double scaleX() const {
        return m_scaleX;
    }
    double scaleY() const {
        return m_scaleY;
    }
    double offsetX() const {
        return m_offsetX;
    }
    double offsetY() const {
        return m_offsetY;
    }

private:
    QSizeF m_originalSize;
    QSizeF m_modelInputSize;
    double m_scaleX = 1.0;
    double m_scaleY = 1.0;
    double m_offsetX = 0.0;
    double m_offsetY = 0.0;
};

} // namespace DeepLux
