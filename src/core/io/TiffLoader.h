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
        Config()
            : scaleX(1.0f), scaleY(1.0f), scaleZ(1.0f), offsetZ(0.0f), step(1), autoDetectNoData(true),
              noDataMinGap(kDefaultNoDataGapFloor) {}

        float scaleX;
        float scaleY;
        float scaleZ;
        float offsetZ;
        int step;
        bool autoDetectNoData;
        // 阶7 批3复核五轮（P2-2）：自动检测是原始像素单位的启发式（发生在任何
        // 缩放之前），间隙下限可按数据格式调整；小幅值哨兵（如 -32768）调低
        // 下限或改用显式 invalidValue
        double noDataMinGap;
        std::optional<double> invalidValue;
        std::optional<double> validMin;
        std::optional<double> validMax;
    };

    // 声明格式（3D 传感器高度图，哨兵为 -21474836/-3.4e38 类大幅值）的默认间隙下限
    static constexpr double kDefaultNoDataGapFloor = 1e6;

    static bool load(const QString& filePath, PointCloudData& outData, QString& errorMsg,
                     const Config& config = Config());

    /// NoData 检测结果三态（阶7 批3复核五轮 P1-1：歧义必须显式上报，不得猜测）
    enum class NoDataStatus {
        None,      ///< 未检出哨兵
        Found,     ///< 单侧极值满足哨兵判据
        Ambiguous, ///< 两侧极值同时满足判据——分布无法判定哪侧是哨兵
    };
    struct NoDataResult {
        NoDataStatus status = NoDataStatus::None;
        double value = 0.0; // 仅 Found 有效
    };

    /**
     * @brief NoData 自动检测（阶7 批3复核：公开供 3D 插件复用同一策略）
     *
     * 重复极值判据（原始像素单位的启发式，检测发生在 scaleZ 等缩放之前）：
     * 仅对单通道 CV_32F/CV_64F 生效——某侧极值出现次数 >= max(16, 5%·有限
     * 像素数)，且与最近内部值的间隙 > max(100×内部值域, minGapFloor) 时判定
     * 该侧为 NoData 哨兵（Found）。两侧独立评估；双侧同时满足（如两值图互为
     * 最近内部值，0=NoData+大正值平台 与 大正值=NoData+0 平台 不可区分）时
     * 返回 Ambiguous——哨兵语义无法从像素分布推导，调用方必须失败关闭并要求
     * 显式哨兵配置或上游契约，不得按绝对值/占比猜测（否则可能反向删除全部
     * 合法数据）。minGapFloor 为声明格式的默认值 1e6，可经参数调整。
     */
    static NoDataResult detectNoData(const cv::Mat& image, double minGapFloor = kDefaultNoDataGapFloor);
};

} // namespace DeepLux
