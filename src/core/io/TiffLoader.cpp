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

TiffLoader::NoDataResult detectRepeatedExtremeNoData(const cv::Mat& image, double minGapFloor) {
    TiffLoader::NoDataResult result;
    if (image.channels() != 1 || (image.depth() != CV_32F && image.depth() != CV_64F)) {
        return result;
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
        return result;
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
    // 阶7 批3复核四轮（P1-1）：哨兵性 = 重复率 + 与最近内部值的绝对间隙，间隙
    // 下限与数据高度偏置无关（大负值域真实图 -4455.5..-2740 + NoData=-21474836
    // 可检出；合法大平台 -1000 + 0..1 细节间隙仅 1000 不被误删）。
    // 阶7 批3复核五轮（P2-2）：本判据是原始像素单位的启发式——检测发生在
    // scaleZ/zScale 等任何缩放之前，间隙下限 minGapFloor 是声明格式（大幅值
    // 存储哨兵）的默认值而非物理量；小幅值哨兵（如 -32768）需调低下限或使用
    // 显式 invalidValue/上游契约。
    const double repeatedThreshold = std::max(16.0, static_cast<double>(finiteCount) * 0.05);
    const double interiorRange =
        (std::isfinite(secondMinimum) && std::isfinite(secondMaximum) && secondMaximum > secondMinimum)
            ? (secondMaximum - secondMinimum)
            : 0.0;
    const double gapThreshold = std::max(interiorRange * 100.0, minGapFloor);
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
        // 阶7 批3复核五轮（P1-1）：双侧同时满足判据（两值图互为最近内部值）——
        // 哨兵语义无法从像素分布推导：{0=NoData, 2e6=合法定深度} 与
        // {0=合法平台, 2e6=NoData} 的直方图完全相同，按绝对值/占比猜测可能
        // 反向删除全部合法数据。上报歧义，由调用方失败关闭并要求显式哨兵
        // 配置或上游契约。
        result.status = TiffLoader::NoDataStatus::Ambiguous;
        return result;
    }
    if (minimumIsNoData == maximumIsNoData) {
        return result; // 均不满足
    }
    result.status = TiffLoader::NoDataStatus::Found;
    result.value = minimumIsNoData ? minimum : maximum;
    return result;
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
    // 阶7 批3复核五轮（P2-2）：间隙下限为启发式可调参数，必须为非负有限数
    if (!std::isfinite(config.noDataMinGap) || config.noDataMinGap < 0.0) {
        errorMsg = "TIFF noDataMinGap must be a finite non-negative number";
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
        // 阶7 批3复核五轮（P1-1）：歧义（双侧极值同满足哨兵判据）失败关闭——
        // 要求显式 Config.invalidValue，不按分布猜测
        const NoDataResult detected = detectRepeatedExtremeNoData(img, config.noDataMinGap);
        if (detected.status == NoDataStatus::Ambiguous) {
            errorMsg = "Ambiguous NoData: both extremes satisfy the sentinel criteria; "
                       "set Config.invalidValue explicitly";
            return false;
        }
        if (detected.status == NoDataStatus::Found) {
            invalidValue = detected.value;
        }
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

TiffLoader::NoDataResult TiffLoader::detectNoData(const cv::Mat& image, double minGapFloor) {
#ifdef DEEPLUX_HAS_OPENCV
    // 阶7 批3复核（P1-1）：公开既有重复极值 NoData 判据，供 3D 插件复用同一策略，
    // 避免各插件复制检测逻辑；五轮改为三态（歧义显式上报，不按分布猜测）
    if (!std::isfinite(minGapFloor) || minGapFloor < 0.0) {
        return NoDataResult{}; // 非法下限视为未检出（调用方参数校验已拦截）
    }
    return detectRepeatedExtremeNoData(image, minGapFloor);
#else
    Q_UNUSED(image);
    Q_UNUSED(minGapFloor);
    return NoDataResult{};
#endif
}

} // namespace DeepLux
