#pragma once

#include <vector>
#include <cstddef>

namespace DeepLux {

class PointCloudData;

/**
 * @brief GPU 友好的点云缓冲区
 *
 * 负责将算法层使用的 PointCloudData (double 精度)
 * 转换为 GPU 渲染友好的格式 (float 精度)
 *
 * 使用交错格式 (interleaved) 便于 VBO 上传
 */
struct PointCloudGPUBuffer {
    std::vector<float> positions;  // x,y,z 交错，float 精度
    std::vector<float> colors;     // r,g,b 交错 (可选)
    std::vector<float> normals;     // nx,ny,nz 交错 (可选)
    std::vector<int> labels;       // 标签 (可选，用于分类显示)

    bool dirty = true;             // 标记是否需要重新上传 VBO

    /**
     * @brief 从 PointCloudData 转换
     * @param data 源数据（double 精度）
     */
    void fromPointCloudData(const PointCloudData& data);

    /**
     * @brief 从 PointCloudData 的指定索引子集转换
     * @param data 源数据（double 精度）
     * @param indices 要包含的点的索引
     */
    void fromPointCloudDataWithIndices(const PointCloudData& data,
                                       const std::vector<size_t>& indices);

    /**
     * @brief 点数量
     */
    size_t pointCount() const {
        return positions.size() / 3;
    }

    /**
     * @brief 是否有效
     */
    bool isValid() const {
        return !positions.empty();
    }

    /**
     * @brief 清空缓冲区
     */
    void clear() {
        positions.clear();
        colors.clear();
        normals.clear();
        labels.clear();
        dirty = true;
    }

    /**
     * @brief 是否包含颜色
     */
    bool hasColors() const { return colors.size() == positions.size(); }

    /**
     * @brief 是否包含法向量
     */
    bool hasNormals() const { return normals.size() == positions.size(); }

    /**
     * @brief 是否包含标签
     */
    bool hasLabels() const { return !labels.empty(); }
};

} // namespace DeepLux
