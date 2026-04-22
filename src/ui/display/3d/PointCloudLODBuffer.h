#pragma once

#include "PointCloudGPUBuffer.h"
#include "LODController.h"
#include <array>

namespace DeepLux {

/**
 * @brief 多分辨率点云缓冲区
 *
 * 存储原始数据和多个降采样级别，
 * 支持基于距离的 LOD 切换
 */
class PointCloudLODBuffer {
public:
    PointCloudLODBuffer();

    /**
     * @brief 设置原始数据，自动生成各级 LOD
     * @param original 原始全分辨率数据
     */
    void setData(const PointCloudGPUBuffer& original);

    /**
     * @brief 获取指定 LOD 级别的缓冲区
     * @param level LOD 级别 (0 = 原始)
     * @return 缓冲区指针，nullptr 表示无效
     */
    const PointCloudGPUBuffer* getLevel(int level) const;

    /**
     * @brief 当前激活的 LOD 级别
     */
    int currentLevel() const { return m_currentLevel; }

    /**
     * @brief 切换到指定级别
     * @param level 目标级别
     */
    void setCurrentLevel(int level);

    /**
     * @brief 清空所有数据
     */
    void clear();

    /**
     * @brief 是否有效
     */
    bool isValid() const { return m_originalBuffer.isValid(); }

    /**
     * @brief 原始数据点数量
     */
    size_t originalPointCount() const { return m_originalBuffer.pointCount(); }

    /**
     * @brief 当前级别点数量
     */
    size_t currentPointCount() const;

    /**
     * @brief 原始缓冲区引用
     */
    const PointCloudGPUBuffer& originalBuffer() const { return m_originalBuffer; }

private:
    /**
     * @brief 生成降采样层级
     */
    void generateLODLevels();

    /**
     * @brief 均匀降采样
     * @param input 输入数据
     * @param targetCount 目标点数
     */
    PointCloudGPUBuffer downsample(const PointCloudGPUBuffer& input, size_t targetCount) const;

    PointCloudGPUBuffer m_originalBuffer;
    std::array<PointCloudGPUBuffer, LODController::MAX_LOD_LEVELS> m_lodLevels;
    int m_currentLevel = 0;
    bool m_dirty = true;
};

} // namespace DeepLux
