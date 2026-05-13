#pragma once

#include "core/display/DisplayData.h"
#include <QString>

namespace DeepLux {

/**
 * @brief TIFF 高度图加载器
 *
 * 使用 OpenCV 读取 TIFF，每像素转换为 (x, y, depth) 点云。
 * 支持 8-bit 和 16-bit 灰度图，彩色 TIFF 同时提供颜色。
 */
class TiffLoader {
public:
    struct Config {
        float scaleX; float scaleY; float scaleZ; int step;
        Config() : scaleX(1.0f), scaleY(1.0f), scaleZ(1.0f), step(1) {}
    };

    static bool load(const QString& filePath, PointCloudData& outData, QString& errorMsg,
                     const Config& config = Config());
};

} // namespace DeepLux
