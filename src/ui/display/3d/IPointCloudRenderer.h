#pragma once

#include <QColor>
#include <QMatrix4x4>
#include "PointCloudGPUBuffer.h"

namespace DeepLux {

/**
 * @brief 点云渲染器接口
 *
 * 定义点云渲染器的统一接口，支持：
 * - GPU 缓冲区渲染
 * - 可配置的渲染参数
 * - 颜色模式选择
 */
class IPointCloudRenderer {
public:
    virtual ~IPointCloudRenderer() = default;

    /**
     * @brief 设置点云数据
     * @param buffer GPU 缓冲区
     */
    virtual void setPointCloud(const PointCloudGPUBuffer& buffer) = 0;

    /**
     * @brief 设置点云数据（支持 LOD）
     * @param buffer GPU 缓冲区
     * @param enableLOD 是否启用 LOD
     */
    virtual void setPointCloud(const PointCloudGPUBuffer& buffer, bool enableLOD) = 0;

    /**
     * @brief 清空渲染
     */
    virtual void clear() = 0;

    /**
     * @brief 设置背景颜色
     */
    virtual void setBackgroundColor(const QColor& color) = 0;

    /**
     * @brief 标记需要重绘（避免与 QWidget::update() 冲突）
     */
    virtual void scheduleRedraw() = 0;

    /**
     * @brief 设置点大小
     * @param size 点大小（像素）
     */
    virtual void setPointSize(float size) = 0;

    /**
     * @brief 设置颜色模式
     * @param mode 0=统一颜色, 1=RGB颜色, 2=标签颜色
     */
    virtual void setColorMode(int mode) = 0;

    /**
     * @brief 设置统一颜色
     * @param color 统一颜色
     */
    virtual void setUniformColor(const QColor& color) = 0;

    /**
     * @brief 渲染
     * @param viewMatrix 视图矩阵
     * @param projectionMatrix 投影矩阵
     */
    virtual void render(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix) = 0;

    /**
     * @brief 是否有效
     */
    virtual bool isValid() const = 0;

    // LOD 支持

    /**
     * @brief 设置 LOD 是否启用
     */
    virtual void setLODEnabled(bool enabled) = 0;

    /**
     * @brief LOD 是否启用
     */
    virtual bool isLODEnabled() const = 0;

    /**
     * @brief 根据相机距离更新 LOD 级别
     * @param distance 相机到目标中心的距离
     */
    virtual void updateLODForDistance(float distance) = 0;

    /**
     * @brief 获取当前 LOD 级别
     */
    virtual int currentLODLevel() const = 0;
};

// 颜色模式枚举
enum class PointCloudColorMode {
    Uniform = 0,   // 统一颜色
    RGB = 1,        // RGB 颜色
    Labels = 2      // 标签颜色
};

} // namespace DeepLux
