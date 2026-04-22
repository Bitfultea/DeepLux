#pragma once

#include <QObject>
#include <QImage>
#include <QVariant>
#include <QMap>
#include <memory>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

/**
 * @brief 图像数据类
 *
 * 封装图像数据，支持 OpenCV cv::Mat 和 QImage 互转
 * 支持附加元数据
 *
 * 当 DEEPLUX_HAS_OPENCV 未定义时，仅支持 QImage 模式
 */
class ImageData
{
public:
    ImageData();
    ImageData(const QImage& image);
#ifdef DEEPLUX_HAS_OPENCV
    ImageData(const cv::Mat& mat);
#endif
    ImageData(int width, int height, int channels = 1);

    ~ImageData() = default;

    // 拷贝和移动
    ImageData(const ImageData& other);
    ImageData& operator=(const ImageData& other);
    ImageData(ImageData&& other) noexcept;
    ImageData& operator=(ImageData&& other) noexcept;

    // ========== 图像信息 ==========

    bool isValid() const;
    bool isEmpty() const;
    int width() const;
    int height() const;
    int channels() const;
    QString format() const;

    // ========== 图像获取 ==========

    QImage toQImage() const;
#ifdef DEEPLUX_HAS_OPENCV
    const cv::Mat& toMat() const;
    bool hasMat() const;

    void setMat(const cv::Mat& mat);
#endif

    void setQImage(const QImage& image);

    // ========== 元数据 ==========

    void setData(const QString& key, const QVariant& value);
    QVariant data(const QString& key) const;
    bool hasData(const QString& key) const;
    void removeData(const QString& key);
    QMap<QString, QVariant> allData() const;
    void clearData();

    // ========== 时间戳 ==========

    void setTimestamp(qint64 timestamp);
    qint64 timestamp() const;

    // ========== 序列化 ==========

    QByteArray toByteArray() const;
    static ImageData fromByteArray(const QByteArray& data);

private:
    struct Impl {
        QImage qImage;
        QMap<QString, QVariant> metadata;
        qint64 timestamp = 0;
        bool valid = false;
#ifdef DEEPLUX_HAS_OPENCV
        cv::Mat mat;
#endif
    };
    std::unique_ptr<Impl> m_impl;
};

} // namespace DeepLux

#ifdef DEEPLUX_HAS_OPENCV
// QImage to cv::Mat conversion helper - defined in ImageData.cpp
namespace DeepLux {
cv::Mat qImageToMat(const QImage& image);
}
#endif

Q_DECLARE_METATYPE(DeepLux::ImageData)
