#pragma once

#include <QWidget>
#include "core/display/DisplayData.h"

namespace DeepLux {

/**
 * @brief 视口内容接口
 *
 * 定义 2D/3D 视口内容的统一接口，实现：
 * - 数据无关的显示抽象
 * - 惰性加载（按需创建）
 * - 工具栏扩展
 */
class IViewportContent {
public:
    virtual ~IViewportContent() = default;

    /**
     * @brief 显示数据
     * @param data 显示数据（可能是 ImageData 或 PointCloudData）
     */
    virtual void displayData(const DisplayData& data) = 0;

    /**
     * @brief 清空显示
     */
    virtual void clearDisplay() = 0;

    /**
     * @brief 获取工具栏扩展（可选）
     * @return 工具栏 widget，如果不需要返回 nullptr
     */
    virtual QWidget* toolbarExtension() { return nullptr; }

    /**
     * @brief 获取内容 widget
     * @return 底层 QWidget
     */
    virtual QWidget* widget() = 0;
};

} // namespace DeepLux
