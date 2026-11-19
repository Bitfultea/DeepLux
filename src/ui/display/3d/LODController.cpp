#include "LODController.h"

#include <algorithm>

namespace DeepLux {

LODController::LODController() = default;

int LODController::calculateLODLevel(float cameraDistance) const {
    if (!m_enabled) {
        return 0; // LOD 禁用时使用最高细节
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
            return; // 无效阈值，不设置
        }
    }
    m_distanceThresholds = thresholds;
}

size_t LODController::getTargetPointCount(size_t originalCount, int level) const {
    if (level < 0 || level >= MAX_LOD_LEVELS) {
        return originalCount;
    }

    if (originalCount == 0 || m_pointBudget == 0) {
        return 0;
    }

    const size_t maxDetailCount = std::min(originalCount, m_pointBudget);
    const size_t target = static_cast<size_t>(maxDetailCount * DEFAULT_SAMPLE_RATES[level]);
    return std::max(size_t(1), target);
}

} // namespace DeepLux
