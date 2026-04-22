#include "LODController.h"
#include <algorithm>
#include <cmath>

namespace DeepLux {

LODController::LODController() = default;

int LODController::calculateLODLevel(float cameraDistance) const {
    if (!m_enabled) {
        return 0;  // LOD 禁用时使用最高细节
    }

    // 从远到近查找第一个小于阈值的级别
    for (int i = MAX_LOD_LEVELS - 1; i >= 0; --i) {
        if (cameraDistance >= m_distanceThresholds[i]) {
            return i;
        }
    }

    // 距离最近，使用最高细节 (LOD 0)
    return 0;
}

void LODController::setDistanceThresholds(const std::array<float, MAX_LOD_LEVELS>& thresholds) {
    // 验证阈值递增
    for (int i = 1; i < MAX_LOD_LEVELS; ++i) {
        if (thresholds[i] <= thresholds[i - 1]) {
            return;  // 无效阈值，不设置
        }
    }
    m_distanceThresholds = thresholds;
}

size_t LODController::getTargetPointCount(size_t originalCount, int level) const {
    if (level < 0 || level >= MAX_LOD_LEVELS) {
        return originalCount;
    }

    // 采样率递减：level 0 = 100%, level 1 = 50%, ...
    float rate = std::pow(0.5f, level);
    size_t target = static_cast<size_t>(originalCount * rate);

    // 确保不超过预算
    return std::min(target, m_pointBudget);
}

} // namespace DeepLux
