#pragma once

#include "core/display/DisplayData.h"

#include <QString>
#include <optional>

namespace cv {
class Mat;
}

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

    /**
     * @brief NoData 自动检测（阶7 批3复核：公开供 3D 插件复用同一策略）
     *
     * 重复极值判据：仅对单通道 CV_32F/CV_64F 生效——某侧极值出现次数
     * >= max(16, 5%·有限像素数)，且与最近内部值的间隙 >
     * max(100×内部值域, 1e6)（绝对间隙下限与数据高度偏置无关，大负值域
     * 真实图的哨兵可检出，合法大面积平台间隙远低于下限不被误删）时判定
     * 该侧为 NoData 哨兵；两侧独立评估，双侧同时满足（两值图互为最近
     * 内部值）时取绝对值更大侧、绝对值相等视为歧义，均不满足返回 nullopt。
     */
    static std::optional<double> detectNoDataValue(const cv::Mat& image);
};

} // namespace DeepLux
