#include "PointCloudLODBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace DeepLux {

PointCloudLODBuffer::PointCloudLODBuffer() {
    // 初始化所有 LOD 缓冲区为干净状态
    for (auto& level : m_lodLevels) {
        level.dirty = true;
    }
}

void PointCloudLODBuffer::setData(const PointCloudGPUBuffer& original, const LODController& controller) {
    m_originalBuffer = original;
    for (auto& level : m_lodLevels)
        level.clear();
    m_currentLevel = 0;
    m_dirty = true;

    if (original.isValid()) {
        generateLODLevels(controller);
    }
}

const PointCloudGPUBuffer* PointCloudLODBuffer::getLevel(int level) const {
    if (level < 0 || level >= LODController::MAX_LOD_LEVELS) {
        return nullptr;
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
        m_lodLevels[level].dirty = true;
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

void PointCloudLODBuffer::generateLODLevels(const LODController& controller) {
    size_t originalCount = m_originalBuffer.pointCount();
    if (originalCount == 0) {
        return;
    }

    std::array<float, 3> minimum{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                                 std::numeric_limits<float>::infinity()};
    std::array<float, 3> maximum{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity()};
    for (size_t i = 0; i < m_originalBuffer.positions.size(); i += 3) {
        for (size_t axis = 0; axis < 3; ++axis) {
            const float value = m_originalBuffer.positions[i + axis];
            if (std::isfinite(value)) {
                minimum[axis] = std::min(minimum[axis], value);
                maximum[axis] = std::max(maximum[axis], value);
            }
        }
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(minimum[axis]))
            minimum[axis] = maximum[axis] = 0.0f;
    }

    const auto spreadBits = [](uint32_t value) {
        uint64_t bits = value & 0x3ffu;
        bits = (bits | (bits << 16)) & 0x030000FFu;
        bits = (bits | (bits << 8)) & 0x0300F00Fu;
        bits = (bits | (bits << 4)) & 0x030C30C3u;
        bits = (bits | (bits << 2)) & 0x09249249u;
        return bits;
    };
    std::vector<std::pair<uint64_t, size_t>> orderedPoints;
    orderedPoints.reserve(originalCount);
    for (size_t i = 0; i < originalCount; ++i) {
        uint64_t key = 0;
        for (size_t axis = 0; axis < 3; ++axis) {
            const float extent = maximum[axis] - minimum[axis];
            const float value = m_originalBuffer.positions[i * 3 + axis];
            const float normalized = std::isfinite(value) && extent > 0.0f ? (value - minimum[axis]) / extent : 0.0f;
            const uint32_t quantized = static_cast<uint32_t>(std::clamp(normalized, 0.0f, 1.0f) * 1023.0f);
            key |= spreadBits(quantized) << axis;
        }
        orderedPoints.emplace_back(key, i);
    }
    std::sort(orderedPoints.begin(), orderedPoints.end());

    // 生成各级空间均匀的降采样数据，最细层也遵守点数预算。
    for (int level = 0; level < LODController::MAX_LOD_LEVELS; ++level) {
        const size_t targetCount = controller.getTargetPointCount(originalCount, level);
        m_lodLevels[level] = downsample(m_originalBuffer, targetCount, orderedPoints);
        m_lodLevels[level].dirty = true;
    }
}

PointCloudGPUBuffer
PointCloudLODBuffer::downsample(const PointCloudGPUBuffer& input, size_t targetCount,
                                const std::vector<std::pair<uint64_t, size_t>>& spatialOrder) const {
    PointCloudGPUBuffer output;
    size_t originalCount = input.pointCount();

    if (originalCount <= targetCount) {
        return input; // 不需要降采样
    }

    output.positions.reserve(targetCount * 3);
    if (input.hasColors())
        output.colors.reserve(targetCount * 3);
    if (input.hasNormals())
        output.normals.reserve(targetCount * 3);
    if (input.hasIntensities())
        output.intensities.reserve(targetCount);
    if (input.hasLabels())
        output.labels.reserve(targetCount);
    for (size_t i = 0; i < targetCount; ++i) {
        const size_t sourcePosition =
            static_cast<size_t>((static_cast<long double>(i) + 0.5L) * originalCount / targetCount);
        const size_t idx = spatialOrder[std::min(originalCount - 1, sourcePosition)].second;
        size_t base = idx * 3;
        output.positions.push_back(input.positions[base]);
        output.positions.push_back(input.positions[base + 1]);
        output.positions.push_back(input.positions[base + 2]);
        if (input.hasColors()) {
            output.colors.push_back(input.colors[base]);
            output.colors.push_back(input.colors[base + 1]);
            output.colors.push_back(input.colors[base + 2]);
        }
        if (input.hasNormals()) {
            output.normals.push_back(input.normals[base]);
            output.normals.push_back(input.normals[base + 1]);
            output.normals.push_back(input.normals[base + 2]);
        }
        if (input.hasIntensities())
            output.intensities.push_back(input.intensities[idx]);
        if (input.hasLabels())
            output.labels.push_back(input.labels[idx]);
    }

    output.dirty = true;
    return output;
}

} // namespace DeepLux
