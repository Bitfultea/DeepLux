#pragma once

#include <array>
#include <cstddef>

namespace DeepLux {

/**
 * @brief LOD 控制器
 *
 * 根据相机距离计算合适的 LOD 级别
 */
class LODController {
public:
    // LOD 级别数量
    static constexpr int MAX_LOD_LEVELS = 4;

    // 默认距离阈值 [近, 中, 远, 超远] (单位：米)
    static constexpr std::array<float, MAX_LOD_LEVELS> DEFAULT_DISTANCES = {1.0f, 5.0f, 20.0f, 100.0f};

    // 默认采样率
    static constexpr std::array<float, MAX_LOD_LEVELS> DEFAULT_SAMPLE_RATES = {1.0f, 0.5f, 0.2f, 0.1f};

    LODController();

    /**
     * @brief 根据相机距离计算 LOD 级别
     * @param cameraDistance 相机到目标中心的距离
     * @return LOD 级别 (0 = 最高细节)
     */
    int calculateLODLevel(float cameraDistance) const;

    /**
     * @brief 设置距离阈值（必须按升序排列）
     */
    void setDistanceThresholds(const std::array<float, MAX_LOD_LEVELS>& thresholds);

    /**
     * @brief 获取距离阈值
     */
    const std::array<float, MAX_LOD_LEVELS>& distanceThresholds() const { return m_distanceThresholds; }

    /**
     * @brief 设置/获取 LOD 是否启用
     */
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    /**
     * @brief 点数预算（用于采样计算）
     */
    void setPointBudget(size_t budget) { m_pointBudget = budget; }
    size_t pointBudget() const { return m_pointBudget; }

    /**
     * @brief 获取指定 LOD 级别的目标点数
     * @param originalCount 原始点数
     * @param level LOD 级别
     */
    size_t getTargetPointCount(size_t originalCount, int level) const;

private:
    std::array<float, MAX_LOD_LEVELS> m_distanceThresholds = DEFAULT_DISTANCES;
    bool m_enabled = true;
    size_t m_pointBudget = 100000;  // 默认 10 万点
};

} // namespace DeepLux
