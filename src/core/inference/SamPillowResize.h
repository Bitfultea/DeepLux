#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <opencv2/core.hpp>
#include <vector>

namespace DeepLux::SamNativeDetail {

struct BilinearTap {
    int first = 0;
    std::vector<int> weights;
};

inline std::vector<BilinearTap> bilinearTaps(int sourceSize, int outputSize) {
    constexpr int precision = 1 << 22;
    const double scale = double(sourceSize) / outputSize;
    const double filterScale = std::max(1.0, scale);
    std::vector<BilinearTap> taps(outputSize);
    for (int i = 0; i < outputSize; ++i) {
        const double center = (i + 0.5) * scale;
        const int first = std::max(0, int(center - filterScale + 0.5));
        const int last = std::min(sourceSize, int(center + filterScale + 0.5));
        BilinearTap& tap = taps[i];
        tap.first = first;
        std::vector<double> coefficients;
        double total = 0;
        for (int j = first; j < last; ++j) {
            const double distance = std::abs((j - center + 0.5) / filterScale);
            const double weight = std::max(0.0, 1.0 - distance);
            coefficients.push_back(weight);
            total += weight;
        }
        for (double weight : coefficients)
            tap.weights.push_back(int(weight / total * precision + 0.5));
    }
    return taps;
}

inline cv::Mat resizePillowBilinear(const cv::Mat& image, const cv::Size& outputSize) {
    // Match the installed Pillow 12.3 RGB8 path: horizontal then vertical, with uint8 rounding after each axis.
    // https://github.com/python-pillow/Pillow/blob/12.3.0/src/libImaging/Resample.c
    constexpr int precisionBits = 22;
    auto resizeAxis = [](const cv::Mat& source, int outputLength, bool horizontal) {
        const auto taps = bilinearTaps(horizontal ? source.cols : source.rows, outputLength);
        cv::Mat output(horizontal ? source.rows : outputLength, horizontal ? outputLength : source.cols, CV_8UC3);
        for (int y = 0; y < output.rows; ++y) {
            auto* dst = output.ptr<cv::Vec3b>(y);
            for (int x = 0; x < output.cols; ++x) {
                const auto& tap = taps[horizontal ? x : y];
                int64_t sum[3] = {1LL << (precisionBits - 1), 1LL << (precisionBits - 1), 1LL << (precisionBits - 1)};
                for (size_t k = 0; k < tap.weights.size(); ++k) {
                    const cv::Vec3b& pixel =
                        source.ptr<cv::Vec3b>(horizontal ? y : tap.first + int(k))[horizontal ? tap.first + int(k) : x];
                    for (int c = 0; c < 3; ++c)
                        sum[c] += int64_t(pixel[c]) * tap.weights[k];
                }
                for (int c = 0; c < 3; ++c)
                    dst[x][c] = static_cast<uchar>(std::clamp(sum[c] >> precisionBits, int64_t(0), int64_t(255)));
            }
        }
        return output;
    };

    cv::Mat result = image;
    if (result.cols != outputSize.width)
        result = resizeAxis(result, outputSize.width, true);
    if (result.rows != outputSize.height)
        result = resizeAxis(result, outputSize.height, false);
    return result;
}

} // namespace DeepLux::SamNativeDetail
