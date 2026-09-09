#include "TiffLoader.h"

#include <QDebug>
#include <QString>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

#ifdef DEEPLUX_HAS_OPENCV
namespace {

// 阶7 批3复核四轮（P1-1）：NoData 判定间隙下限——重复极值与最近内部值的间隙
// 必须超过 max(100×内部值域, 1e6) 才视为哨兵（mm 级高度图上 >1km 的孤悬重复
// 极值即哨兵语义；合法大平台的间隙远低于该下限）
constexpr double kNoDataGapFloor = 1e6;

std::optional<double> singleChannelValue(const cv::Mat& image, int y, int x) {
    switch (image.depth()) {
    case CV_8U:
        return image.at<uchar>(y, x);
    case CV_8S:
        return image.at<signed char>(y, x);
    case CV_16U:
        return image.at<ushort>(y, x);
    case CV_16S:
        return image.at<short>(y, x);
    case CV_32S:
        return image.at<int>(y, x);
    case CV_32F:
        return image.at<float>(y, x);
    case CV_64F:
        return image.at<double>(y, x);
    default:
        return std::nullopt;
    }
}

std::optional<double> detectRepeatedExtremeNoData(const cv::Mat& image) {
    if (image.channels() != 1 || (image.depth() != CV_32F && image.depth() != CV_64F)) {
        return std::nullopt;
    }

    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    qint64 finiteCount = 0;
    qint64 minimumCount = 0;
    qint64 maximumCount = 0;

    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            const double value = *singleChannelValue(image, y, x);
            if (!std::isfinite(value)) {
                continue;
            }
            ++finiteCount;
            if (value < minimum) {
                minimum = value;
                minimumCount = 1;
            } else if (value == minimum) {
                ++minimumCount;
            }
            if (value > maximum) {
                maximum = value;
                maximumCount = 1;
            } else if (value == maximum) {
                ++maximumCount;
            }
        }
    }

    if (finiteCount < 3 || minimum == maximum) {
        return std::nullopt;
    }

    double secondMinimum = std::numeric_limits<double>::infinity();
    double secondMaximum = -std::numeric_limits<double>::infinity();
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            const double value = *singleChannelValue(image, y, x);
            if (!std::isfinite(value)) {
                continue;
            }
            if (value > minimum && value < secondMinimum) {
                secondMinimum = value;
            }
            if (value < maximum && value > secondMaximum) {
                secondMaximum = value;
            }
        }
    }

    // 阶7 批3复核三轮（P1-1）：两侧独立评估——旧版 secondMaximum <= secondMinimum
    // 全局防护在"哨兵 + 恒定有效值"（仅两个不同值）时漏检真实 NoData；内部值域
    // 仅在 secondMax > secondMin 时有定义，否则按 0 处理。
    // 阶7 批3复核四轮（P1-1）：哨兵性 = 重复率 + 与最近内部值的绝对间隙。三轮的
    // 量级门禁以 |绝对高度| 为数据尺度，对有效高度为大负值的真实图漏检
    // （-4455.5..-2740 + NoData=-21474836：21.5M < 1e4×4455.5=44.6M）。改为
    // 间隙下限：重复极值与最近内部值的间隙须 > max(100×内部值域, 1e6)——
    // mm 级高度图上与全部真实数据相隔 >1e6（>1km）的重复极值即哨兵语义；
    // 合法大平台（-1000 平台 + 0..1 细节，间隙 1000）仍不被误删。
    const double repeatedThreshold = std::max(16.0, static_cast<double>(finiteCount) * 0.05);
    const double interiorRange =
        (std::isfinite(secondMinimum) && std::isfinite(secondMaximum) && secondMaximum > secondMinimum)
            ? (secondMaximum - secondMinimum)
            : 0.0;
    const double gapThreshold = std::max(interiorRange * 100.0, kNoDataGapFloor);
    const auto sentinelLike = [&](double extreme, qint64 count, double nearestInterior) {
        if (count < repeatedThreshold) {
            return false;
        }
        if (!std::isfinite(nearestInterior)) {
            return false; // 除候选极值外无其他值，无法建立分离判据
        }
        return std::abs(nearestInterior - extreme) > gapThreshold;
    };
    const bool minimumIsNoData = sentinelLike(minimum, minimumCount, secondMinimum);
    const bool maximumIsNoData = sentinelLike(maximum, maximumCount, secondMaximum);

    if (minimumIsNoData && maximumIsNoData) {
        // 阶7 批3复核四轮（P1-1）：两侧同时满足重复率与间隙判据（如"哨兵+恒定
        // 有效值"的两值图，互为最近内部值）——取绝对值更大侧为哨兵：两侧均已
        // 与数据相隔 >1e6，距 0 更远者才符合存储哨兵语义（-21474836/-3.4e38/
        // 1e30 类）；绝对值相等视为歧义，不删除任何数据
        if (std::abs(minimum) == std::abs(maximum)) {
            return std::nullopt;
        }
        return std::abs(minimum) > std::abs(maximum) ? minimum : maximum;
    }
    if (minimumIsNoData == maximumIsNoData) {
        return std::nullopt;
    }
    return minimumIsNoData ? minimum : maximum;
}

} // namespace
#endif

bool TiffLoader::load(const QString& filePath, PointCloudData& outData, QString& errorMsg, const Config& config) {
#ifdef DEEPLUX_HAS_OPENCV
    errorMsg.clear();
    if (!std::isfinite(config.scaleX) || !std::isfinite(config.scaleY) || !std::isfinite(config.scaleZ) ||
        !std::isfinite(config.offsetZ)) {
        errorMsg = "TIFF scale and offset values must be finite";
        return false;
    }
    if (config.validMin && config.validMax && *config.validMin > *config.validMax) {
        errorMsg = "TIFF valid minimum must not exceed valid maximum";
        return false;
    }
    // 阶7 批3复核四轮（P2-3）：显式哨兵/有效范围必须有限——invalidValue=NaN 会
    // 关闭自动检测且后续所有比较恒假，统一在读取图像前失败关闭
    if (config.invalidValue && !std::isfinite(*config.invalidValue)) {
        errorMsg = "TIFF explicit invalid value must be finite";
        return false;
    }
    if (config.validMin && !std::isfinite(*config.validMin)) {
        errorMsg = "TIFF valid minimum must be finite";
        return false;
    }
    if (config.validMax && !std::isfinite(*config.validMax)) {
        errorMsg = "TIFF valid maximum must be finite";
        return false;
    }

    cv::Mat img = cv::imread(filePath.toStdString(), cv::IMREAD_UNCHANGED);
    if (img.empty()) {
        errorMsg = QString("Cannot open TIFF file: %1").arg(filePath);
        return false;
    }

    const int channels = img.channels();
    const int depth = img.depth();
    const int step = qMax(1, config.step);

    if (channels != 1 && channels < 3) {
        errorMsg = QString("Unsupported TIFF channel count: %1").arg(channels);
        return false;
    }
    if (channels == 1 && !singleChannelValue(img, 0, 0)) {
        errorMsg = QString("Unsupported TIFF pixel depth: %1").arg(depth);
        return false;
    }

    std::optional<double> invalidValue = config.invalidValue;
    // 阶7 批3复核三轮（P2-5）：显式哨兵按源图存储精度量化——非整数哨兵写入
    // CV_32F 后（如 -21474.8359 → -21474.8359375），double 精确比较必然失配
    if (invalidValue) {
        if (depth == CV_32F) {
            invalidValue = static_cast<double>(static_cast<float>(*invalidValue));
        } else if (depth != CV_64F) {
            invalidValue = std::nearbyint(*invalidValue);
        }
    }
    if (!invalidValue && !config.validMin && !config.validMax && config.autoDetectNoData) {
        invalidValue = detectRepeatedExtremeNoData(img);
    }

    outData.clear();
    const int totalPixels = ((img.rows + step - 1) / step) * ((img.cols + step - 1) / step);
    outData.points.reserve(totalPixels);

    if (channels >= 3) {
        outData.colors.reserve(totalPixels);
    }

    qint64 rejectedCount = 0;
    for (int y = 0; y < img.rows; y += step) {
        for (int x = 0; x < img.cols; x += step) {
            double z = 0;
            Eigen::Vector3d color(0.5, 0.5, 0.5);

            if (channels == 1) {
                z = *singleChannelValue(img, y, x);
            } else if (channels >= 3) {
                const int offset = x * channels;
                double b = 0;
                double g = 0;
                double r = 0;
                double colorMax = 1.0;

                switch (depth) {
                case CV_8U: {
                    const uchar* row = img.ptr<uchar>(y);
                    b = row[offset];
                    g = row[offset + 1];
                    r = row[offset + 2];
                    colorMax = 255.0;
                    break;
                }
                case CV_16U: {
                    const ushort* row = img.ptr<ushort>(y);
                    b = row[offset];
                    g = row[offset + 1];
                    r = row[offset + 2];
                    colorMax = 65535.0;
                    break;
                }
                case CV_32F: {
                    const float* row = img.ptr<float>(y);
                    b = row[offset];
                    g = row[offset + 1];
                    r = row[offset + 2];
                    break;
                }
                case CV_64F: {
                    const double* row = img.ptr<double>(y);
                    b = row[offset];
                    g = row[offset + 1];
                    r = row[offset + 2];
                    break;
                }
                default:
                    errorMsg = QString("Unsupported TIFF pixel depth: %1").arg(depth);
                    outData.clear();
                    return false;
                }

                // Greyscale approximation from RGB
                z = 0.299 * r + 0.587 * g + 0.114 * b;
                color = Eigen::Vector3d(std::clamp(r / colorMax, 0.0, 1.0), std::clamp(g / colorMax, 0.0, 1.0),
                                        std::clamp(b / colorMax, 0.0, 1.0));
            }

            if (!std::isfinite(z) || (invalidValue && z == *invalidValue) ||
                (config.validMin && z < *config.validMin) || (config.validMax && z > *config.validMax)) {
                ++rejectedCount;
                continue;
            }

            outData.points.push_back(
                Eigen::Vector3d(x * config.scaleX, y * config.scaleY, z * config.scaleZ + config.offsetZ));

            if (channels >= 3) {
                outData.colors.push_back(color);
            }
        }
    }

    qDebug() << "TiffLoader: loaded" << outData.points.size() << "points from" << filePath << "rejected"
             << rejectedCount << "auto/explicit no-data"
             << (invalidValue ? QString::number(*invalidValue, 'g', 12) : QString("none"));

    if (outData.points.empty()) {
        errorMsg = "TIFF contains no valid height samples";
        return false;
    }

    return true;
#else
    Q_UNUSED(filePath);
    Q_UNUSED(outData);
    Q_UNUSED(config);
    errorMsg = "OpenCV not available (DEEPLUX_HAS_OPENCV not defined)";
    return false;
#endif
}

std::optional<double> TiffLoader::detectNoDataValue(const cv::Mat& image) {
#ifdef DEEPLUX_HAS_OPENCV
    // 阶7 批3复核（P1-1）：公开既有重复极值 NoData 判据，供 3D 插件复用同一策略，
    // 避免各插件复制检测逻辑
    return detectRepeatedExtremeNoData(image);
#else
    Q_UNUSED(image);
    return std::nullopt;
#endif
}

} // namespace DeepLux
