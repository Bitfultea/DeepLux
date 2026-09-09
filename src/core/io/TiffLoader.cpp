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
    // 仅在 secondMax > secondMin 时有定义，否则按 0 处理（分离阈值退化为 1）。
    // 同时增加量级门禁：候选哨兵绝对值必须远超数据尺度（>= max(1e6, 1e4×参考
    // 尺度)），防止把合法大面积平台（如 69% 的 -1000 平台 + 0..1 细节）误判为
    // NoData 而删除有效高度。
    const double repeatedThreshold = std::max(16.0, static_cast<double>(finiteCount) * 0.05);
    const double interiorRange =
        (std::isfinite(secondMinimum) && std::isfinite(secondMaximum) && secondMaximum > secondMinimum)
            ? (secondMaximum - secondMinimum)
            : 0.0;
    const double separationThreshold = std::max(interiorRange * 100.0, 1.0);
    const auto sentinelLike = [&](double extreme, qint64 count, double nearestInterior, double oppositeExtreme) {
        if (count < repeatedThreshold) {
            return false;
        }
        if (!std::isfinite(nearestInterior)) {
            return false; // 除候选极值外无其他值，无法建立分离判据
        }
        if (std::abs(nearestInterior - extreme) <= separationThreshold) {
            return false;
        }
        const double scaleRef = std::max(std::abs(nearestInterior), std::abs(oppositeExtreme));
        return std::abs(extreme) > std::max(1e6, scaleRef * 1e4);
    };
    const bool minimumIsNoData = sentinelLike(minimum, minimumCount, secondMinimum, maximum);
    const bool maximumIsNoData = sentinelLike(maximum, maximumCount, secondMaximum, minimum);

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
