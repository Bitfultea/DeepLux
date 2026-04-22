#include "PointCloudGPUBuffer.h"
#include "core/display/DisplayData.h"

namespace DeepLux {

void PointCloudGPUBuffer::fromPointCloudData(const PointCloudData& data) {
    clear();

    if (data.isEmpty()) {
        return;
    }

    // 转换位置坐标 (double → float)
    positions.reserve(data.points.size() * 3);
    for (const auto& p : data.points) {
        positions.push_back(static_cast<float>(p.x()));
        positions.push_back(static_cast<float>(p.y()));
        positions.push_back(static_cast<float>(p.z()));
    }

    // 转换颜色 (double → float, 0-1 范围)
    if (data.hasColors()) {
        colors.reserve(data.colors.size() * 3);
        for (const auto& c : data.colors) {
            // 确保颜色在合理范围内
            colors.push_back(static_cast<float>(std::min(1.0, std::max(0.0, c.x()))));
            colors.push_back(static_cast<float>(std::min(1.0, std::max(0.0, c.y()))));
            colors.push_back(static_cast<float>(std::min(1.0, std::max(0.0, c.z()))));
        }
    }

    // 转换法向量 (double → float)
    if (data.hasNormals()) {
        normals.reserve(data.normals.size() * 3);
        for (const auto& n : data.normals) {
            normals.push_back(static_cast<float>(n.x()));
            normals.push_back(static_cast<float>(n.y()));
            normals.push_back(static_cast<float>(n.z()));
        }
    }

    // 复制标签
    if (data.hasLabels()) {
        labels = data.labels;
    }

    dirty = true;
}

void PointCloudGPUBuffer::fromPointCloudDataWithIndices(const PointCloudData& data,
                                                        const std::vector<size_t>& indices) {
    clear();

    if (data.isEmpty() || indices.empty()) {
        return;
    }

    // 转换位置坐标 (double → float)
    positions.reserve(indices.size() * 3);
    for (size_t idx : indices) {
        if (idx >= data.points.size()) continue;
        const auto& p = data.points[idx];
        positions.push_back(static_cast<float>(p.x()));
        positions.push_back(static_cast<float>(p.y()));
        positions.push_back(static_cast<float>(p.z()));
    }

    // 转换颜色 (double → float, 0-1 范围)
    if (data.hasColors() && data.colors.size() == data.points.size()) {
        colors.reserve(indices.size() * 3);
        for (size_t idx : indices) {
            if (idx >= data.colors.size()) continue;
            const auto& c = data.colors[idx];
            colors.push_back(static_cast<float>(std::min(1.0, std::max(0.0, c.x()))));
            colors.push_back(static_cast<float>(std::min(1.0, std::max(0.0, c.y()))));
            colors.push_back(static_cast<float>(std::min(1.0, std::max(0.0, c.z()))));
        }
    }

    // 转换法向量 (double → float)
    if (data.hasNormals() && data.normals.size() == data.points.size()) {
        normals.reserve(indices.size() * 3);
        for (size_t idx : indices) {
            if (idx >= data.normals.size()) continue;
            const auto& n = data.normals[idx];
            normals.push_back(static_cast<float>(n.x()));
            normals.push_back(static_cast<float>(n.y()));
            normals.push_back(static_cast<float>(n.z()));
        }
    }

    // 复制标签
    if (data.hasLabels() && data.labels.size() == data.points.size()) {
        labels.reserve(indices.size());
        for (size_t idx : indices) {
            if (idx >= data.labels.size()) continue;
            labels.push_back(data.labels[idx]);
        }
    }

    dirty = true;
}

} // namespace DeepLux
