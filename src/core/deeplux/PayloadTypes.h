#pragma once

#include <QList>
#include <QMetaType>
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

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::Circle2D)
Q_DECLARE_METATYPE(DeepLux::Detection)
Q_DECLARE_METATYPE(DeepLux::DetectionList)
