#pragma once

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace DeepLux {

/// LabelMe shape（导入器校验后保留的字段）
struct LabelMeShape {
    QString label;
    QString shapeType;     // "rectangle"/"polygon"/...（缺省时按点数推导：2→rectangle，≥3→polygon）
    QList<QPointF> points; // 有限数值坐标
};

/// rectangle → box prompt 候选（保留 label 供标注对象使用）
struct LabelMeBox {
    QString label;
    QRectF box; // 归一化（x,y,w,h，w/h>0）
};

/**
 * @brief LabelMe JSON 导入器（SAM 批4）——严格校验失败关闭
 *
 * 用于"LabelMe 矩形框 → SAM box prompt 批量分割"链路：
 * - loadShapes：JSON 对象 + shapes 数组逐元素校验（对象/label 非空字符串/
 *   points ≥2 个 [x,y] 有限数值对/shape_type 字符串或缺省推导），任一不符
 *   整体失败并指明下标——不静默丢弃畸形 shape
 * - rectangleBoxes：提取 rectangle 的归一化 box（两点取 min/max，w/h>0）；
 *   非矩形与退化矩形跳过并计数（调用方显式报告，不静默）
 */
class LabelMeImporter {
public:
    static bool loadShapes(const QString& filePath, QList<LabelMeShape>& outShapes, QString* error = nullptr);
    static QList<LabelMeBox> rectangleBoxes(const QList<LabelMeShape>& shapes, int* skippedCount = nullptr);
};

} // namespace DeepLux
