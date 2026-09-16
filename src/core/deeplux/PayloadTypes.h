#pragma once

#include <QList>
#include <QMetaType>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace DeepLux {

/**
 * @brief 2D 圆测量结果载荷（契约层专用载荷类型）
 *
 * 供 FindCircle/FitCircle 等测量插件以端口式输出强类型结果。
 * 置于契约层（core/deeplux），避免数据契约依赖显示层语义。
 */
struct Circle2D {
    double centerX = 0.0;
    double centerY = 0.0;
    double radius = 0.0;
    double score = 0.0; // 拟合/检测置信度（可选）

    bool isValid() const {
        return radius > 0.0;
    }
};

/**
 * @brief 单个检测目标（契约层专用载荷类型）
 */
struct Detection {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double score = 0.0;
    QString label;
};

/**
 * @brief 检测结果列表载荷（契约层专用载荷类型）
 *
 * 供 Matching/检测类插件以端口式输出强类型结果集合。
 */
struct DetectionList {
    QList<Detection> items;

    bool isEmpty() const {
        return items.isEmpty();
    }
    int size() const {
        return items.size();
    }
};

/**
 * @brief 2D 区域载荷（契约层专用载荷类型，SAM 批1）
 *
 * 供 SAM 分割等生产者把区域送入主流程：polygon 为原图坐标多边形，bbox 为
 * 外接矩形，maskRle 为掩膜 RLE 编码（像素掩膜不入载荷，避免跨插件 QImage
 * 序列化开销；需要像素掩膜的消费者从 maskRle 自行解码，生产者不缓存掩膜
 * 像素）。与 AnnotationObject 字段对齐，便于标注会话与流程互转。
 *
 * isValid() 为结构体自身的最低有效性（polygon≥3 或 bbox 有效）；端口契约
 * （DataContract isRegion2D）另要求：map 形态必须同时含 polygon 与 bbox
 * 字段，且两种形态全部数值分量有限、score∈[0,1]，非有限失败关闭。
 */
struct Region2D {
    QString label;
    QList<QPointF> polygon; // 原图坐标
    QRectF bbox;
    QString maskRle;
    double score = 0.0;

    bool isValid() const {
        return polygon.size() >= 3 || bbox.isValid();
    }
};

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::Circle2D)
Q_DECLARE_METATYPE(DeepLux::Detection)
Q_DECLARE_METATYPE(DeepLux::DetectionList)
Q_DECLARE_METATYPE(DeepLux::Region2D)
