#pragma once

#include "core/display/DisplayData.h"

#include <QString>
#include <optional>

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
        Config() : scaleX(1.0f), scaleY(1.0f), scaleZ(1.0f), offsetZ(0.0f), step(1), autoDetectNoData(true) {}

        float scaleX;
        float scaleY;
        float scaleZ;
        float offsetZ;
        int step;
        bool autoDetectNoData;
        std::optional<double> invalidValue;
        std::optional<double> validMin;
        std::optional<double> validMax;
    };

    static bool load(const QString& filePath, PointCloudData& outData, QString& errorMsg,
                     const Config& config = Config());
};

} // namespace DeepLux
