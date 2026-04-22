#include "PointCloudLODBuffer.h"
#include <algorithm>
#include <cmath>

namespace DeepLux {

PointCloudLODBuffer::PointCloudLODBuffer() {
    // 初始化所有 LOD 缓冲区为干净状态
    for (auto& level : m_lodLevels) {
        level.dirty = true;
    }
}

void PointCloudLODBuffer::setData(const PointCloudGPUBuffer& original) {
    m_originalBuffer = original;
    m_currentLevel = 0;
    m_dirty = true;

    if (original.isValid()) {
        generateLODLevels();
    }
}

const PointCloudGPUBuffer* PointCloudLODBuffer::getLevel(int level) const {
    if (level < 0 || level >= LODController::MAX_LOD_LEVELS) {
        return nullptr;
    }

    if (level == 0) {
        return &m_originalBuffer;
    }

    return &m_lodLevels[level];
}

void PointCloudLODBuffer::setCurrentLevel(int level) {
    if (level < 0 || level >= LODController::MAX_LOD_LEVELS) {
        return;
    }

    if (m_currentLevel != level) {
        m_currentLevel = level;
        // 标记当前级别缓冲区需要上传
        if (level > 0) {
            m_lodLevels[level].dirty = true;
        }
    }
}

void PointCloudLODBuffer::clear() {
    m_originalBuffer.clear();
    for (auto& level : m_lodLevels) {
        level.clear();
    }
    m_currentLevel = 0;
    m_dirty = true;
}

size_t PointCloudLODBuffer::currentPointCount() const {
    if (!isValid()) {
        return 0;
    }

    const PointCloudGPUBuffer* level = getLevel(m_currentLevel);
    return level ? level->pointCount() : 0;
}

void PointCloudLODBuffer::generateLODLevels() {
    size_t originalCount = m_originalBuffer.pointCount();
    if (originalCount == 0) {
        return;
    }

    // 生成各级降采样数据
    for (int level = 1; level < LODController::MAX_LOD_LEVELS; ++level) {
        // 采样率: 50%, 25%, 12.5%, 6.25%
        float rate = std::pow(0.5f, level);
        size_t targetCount = static_cast<size_t>(originalCount * rate);

        // 最小保留 100 点
        targetCount = std::max(targetCount, size_t(100));

        m_lodLevels[level] = downsample(m_originalBuffer, targetCount);
        m_lodLevels[level].dirty = true;
    }
}

PointCloudGPUBuffer PointCloudLODBuffer::downsample(
    const PointCloudGPUBuffer& input,
    size_t targetCount) const
{
    PointCloudGPUBuffer output;
    size_t originalCount = input.pointCount();

    if (originalCount <= targetCount) {
        return input;  // 不需要降采样
    }

    // 计算采样步长 (等间距采样保证均匀性)
    float step = static_cast<float>(originalCount) / static_cast<float>(targetCount);

    // positions 是交错格式 (x,y,z,x,y,z,...)
    output.positions.reserve(targetCount * 3);
    for (size_t i = 0; i < targetCount; ++i) {
        // 使用四舍五入获得更均匀的分布
        size_t idx = static_cast<size_t>(std::round(i * step));
        if (idx >= originalCount) idx = originalCount - 1;  // 边界保护
        size_t base = idx * 3;
        output.positions.push_back(input.positions[base]);
        output.positions.push_back(input.positions[base + 1]);
        output.positions.push_back(input.positions[base + 2]);
    }

    // 颜色同步采样
    if (input.hasColors()) {
        output.colors.reserve(targetCount * 3);
        for (size_t i = 0; i < targetCount; ++i) {
            size_t idx = static_cast<size_t>(i * step);
            size_t base = idx * 3;
            output.colors.push_back(input.colors[base]);
            output.colors.push_back(input.colors[base + 1]);
            output.colors.push_back(input.colors[base + 2]);
        }
    }

    // 法向量同步采样
    if (input.hasNormals()) {
        output.normals.reserve(targetCount * 3);
        for (size_t i = 0; i < targetCount; ++i) {
            size_t idx = static_cast<size_t>(i * step);
            size_t base = idx * 3;
            output.normals.push_back(input.normals[base]);
            output.normals.push_back(input.normals[base + 1]);
            output.normals.push_back(input.normals[base + 2]);
        }
    }

    // 标签同步采样
    if (input.hasLabels()) {
        output.labels.reserve(targetCount);
        for (size_t i = 0; i < targetCount; ++i) {
            size_t idx = static_cast<size_t>(i * step);
            output.labels.push_back(input.labels[idx]);
        }
    }

    output.dirty = true;
    return output;
}

} // namespace DeepLux
