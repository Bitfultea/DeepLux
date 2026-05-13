#include "TiffLoader.h"

#include <QString>
#include <QDebug>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

bool TiffLoader::load(const QString& filePath, PointCloudData& outData, QString& errorMsg,
                       const Config& config) {
#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat img = cv::imread(filePath.toStdString(), cv::IMREAD_UNCHANGED);
    if (img.empty()) {
        errorMsg = QString("Cannot open TIFF file: %1").arg(filePath);
        return false;
    }

    int channels = img.channels();
    int depth = img.depth(); // CV_8U=0, CV_16U=2, CV_32F=5
    int step = qMax(1, config.step);

    outData.clear();
    int totalPixels = (img.rows / step) * (img.cols / step);
    outData.points.reserve(totalPixels);

    if (channels >= 3) {
        outData.colors.reserve(totalPixels);
    }

    for (int y = 0; y < img.rows; y += step) {
        for (int x = 0; x < img.cols; x += step) {
            double z = 0;
            Eigen::Vector3d color(0.5, 0.5, 0.5);

            if (channels == 1) {
                switch (depth) {
                case CV_8U:  z = img.at<uchar>(y, x); break;
                case CV_16U: z = img.at<ushort>(y, x); break;
                case CV_32F: z = img.at<float>(y, x); break;
                default:     z = 0;
                }
            } else if (channels >= 3) {
                cv::Vec3b bgr = img.at<cv::Vec3b>(y, x);
                // Greyscale approximation from RGB
                z = 0.299 * bgr[2] + 0.587 * bgr[1] + 0.114 * bgr[0];
                color = Eigen::Vector3d(bgr[2] / 255.0, bgr[1] / 255.0, bgr[0] / 255.0);
            }

            outData.points.push_back(Eigen::Vector3d(
                x * config.scaleX,
                y * config.scaleY,
                z * config.scaleZ
            ));

            if (channels >= 3) {
                outData.colors.push_back(color);
            }
        }
    }

    qDebug() << "TiffLoader: loaded" << outData.points.size() << "points from" << filePath;
    return !outData.points.empty();
#else
    Q_UNUSED(filePath); Q_UNUSED(outData); Q_UNUSED(config);
    errorMsg = "OpenCV not available (DEEPLUX_HAS_OPENCV not defined)";
    return false;
#endif
}

} // namespace DeepLux
