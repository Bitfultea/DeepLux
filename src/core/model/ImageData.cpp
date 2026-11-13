#include "ImageData.h"

#include <QBuffer>
#include <QDebug>
#include <QImage>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

namespace {

bool finiteMatrix(const ImageCoordinateProvenance::Matrix3& matrix) {
    return std::all_of(matrix.begin(), matrix.end(), [](double value) { return std::isfinite(value); });
}

double determinant(const ImageCoordinateProvenance::Matrix3& matrix) {
    return matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7]) -
           matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6]) +
           matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
}

ImageCoordinateProvenance::Matrix3 multiply(const ImageCoordinateProvenance::Matrix3& lhs,
                                            const ImageCoordinateProvenance::Matrix3& rhs) {
    ImageCoordinateProvenance::Matrix3 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            for (int index = 0; index < 3; ++index)
                result[row * 3 + column] += lhs[row * 3 + index] * rhs[index * 3 + column];
        }
    }
    return result;
}

bool mapPoint(const ImageCoordinateProvenance::Matrix3& matrix, const QPointF& point, QPointF* mapped) {
    const double x = point.x();
    const double y = point.y();
    const double denominator = matrix[6] * x + matrix[7] * y + matrix[8];
    const double denominatorScale = std::abs(matrix[6] * x) + std::abs(matrix[7] * y) + std::abs(matrix[8]);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(denominator) ||
        std::abs(denominator) <= std::numeric_limits<double>::epsilon() * 16.0 * qMax(1.0, denominatorScale))
        return false;

    const double mappedX = (matrix[0] * x + matrix[1] * y + matrix[2]) / denominator;
    const double mappedY = (matrix[3] * x + matrix[4] * y + matrix[5]) / denominator;
    if (!std::isfinite(mappedX) || !std::isfinite(mappedY))
        return false;
    *mapped = QPointF(mappedX, mappedY);
    return true;
}

} // namespace

bool ImageCoordinateProvenance::isValid() const {
    if (sourceImageId.isEmpty() || sourceSize.width() <= 0 || sourceSize.height() <= 0)
        return false;
    if (!hasLocalToSourceTransform)
        return std::isfinite(localOriginInSource.x()) && std::isfinite(localOriginInSource.y());
    const Matrix3 matrix = localToSourceMatrix();
    const double det = determinant(matrix);
    return finiteMatrix(matrix) && std::isfinite(det) && det != 0.0;
}

std::optional<QPointF> ImageCoordinateProvenance::mapLocalToSource(const QPointF& point) const {
    if (!isValid())
        return std::nullopt;
    QPointF mapped;
    if (!mapPoint(localToSourceMatrix(), point, &mapped))
        return std::nullopt;
    return mapped;
}

std::optional<QRectF> ImageCoordinateProvenance::mapRectToSourceBounds(const QRectF& rect) const {
    if (!isValid())
        return std::nullopt;
    const QRectF local = rect.normalized();
    const QPointF corners[] = {local.topLeft(), local.topRight(), local.bottomRight(), local.bottomLeft()};
    const Matrix3 matrix = localToSourceMatrix();
    double denominatorSign = 0.0;
    QPointF mappedCorners[4];
    for (int i = 0; i < 4; ++i) {
        const double denominator = matrix[6] * corners[i].x() + matrix[7] * corners[i].y() + matrix[8];
        if (!std::isfinite(denominator) || denominator == 0.0 || !mapPoint(matrix, corners[i], &mappedCorners[i]))
            return std::nullopt;
        const double sign = std::copysign(1.0, denominator);
        if (denominatorSign != 0.0 && sign != denominatorSign)
            return std::nullopt;
        denominatorSign = sign;
    }

    double minX = mappedCorners[0].x();
    double maxX = minX;
    double minY = mappedCorners[0].y();
    double maxY = minY;
    for (int i = 1; i < 4; ++i) {
        minX = qMin(minX, mappedCorners[i].x());
        maxX = qMax(maxX, mappedCorners[i].x());
        minY = qMin(minY, mappedCorners[i].y());
        maxY = qMax(maxY, mappedCorners[i].y());
    }
    return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
}

bool ImageCoordinateProvenance::offsetLocalOrigin(const QPointF& offset) {
    const Matrix3 translation{{1.0, 0.0, offset.x(), 0.0, 1.0, offset.y(), 0.0, 0.0, 1.0}};
    return composeLocalToSource(translation);
}

bool ImageCoordinateProvenance::composeLocalToSource(const Matrix3& localToPrevious) {
    const double det = determinant(localToPrevious);
    if (!finiteMatrix(localToPrevious) || !std::isfinite(det) || det == 0.0)
        return false;
    const Matrix3 composed = multiply(localToSourceMatrix(), localToPrevious);
    const double composedDet = determinant(composed);
    if (!finiteMatrix(composed) || !std::isfinite(composedDet) || composedDet == 0.0)
        return false;
    localToSource = composed;
    hasLocalToSourceTransform = true;
    const std::optional<QPointF> origin = mapLocalToSource(QPointF());
    if (origin)
        localOriginInSource = *origin;
    return true;
}

ImageCoordinateProvenance::Matrix3 ImageCoordinateProvenance::localToSourceMatrix() const {
    if (hasLocalToSourceTransform)
        return localToSource;
    return Matrix3{{1.0, 0.0, localOriginInSource.x(), 0.0, 1.0, localOriginInSource.y(), 0.0, 0.0, 1.0}};
}

ImageData::ImageData() : m_impl(std::make_unique<Impl>()) {}

ImageData::ImageData(const QImage& image) : m_impl(std::make_unique<Impl>()) {
    setQImage(image);
    establishCoordinateSource();
}

#ifdef DEEPLUX_HAS_OPENCV
ImageData::ImageData(const cv::Mat& mat) : m_impl(std::make_unique<Impl>()) {
    setMat(mat);
    establishCoordinateSource();
}
#endif

ImageData::ImageData(int width, int height, int channels) : m_impl(std::make_unique<Impl>()) {
#ifdef DEEPLUX_HAS_OPENCV
    Q_UNUSED(channels);
    if (width > 0 && height > 0) {
        m_impl->mat = cv::Mat(height, width, CV_8UC1, cv::Scalar(0));
        m_impl->valid = true;
    }
#else
    Q_UNUSED(channels);
    if (width > 0 && height > 0) {
        m_impl->qImage = QImage(width, height, QImage::Format_Grayscale8);
        m_impl->qImage.fill(Qt::black);
        m_impl->valid = true;
    }
#endif
    establishCoordinateSource();
}

ImageData::ImageData(const ImageData& other) : m_impl(std::make_unique<Impl>(*other.m_impl)) {}

ImageData& ImageData::operator=(const ImageData& other) {
    if (this != &other) {
        m_impl = std::make_unique<Impl>(*other.m_impl);
    }
    return *this;
}

ImageData::ImageData(ImageData&& other) noexcept : m_impl(std::move(other.m_impl)) {}

ImageData& ImageData::operator=(ImageData&& other) noexcept {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

bool ImageData::isValid() const {
    return m_impl->valid;
}

bool ImageData::isEmpty() const {
    return !m_impl->valid;
}

int ImageData::width() const {
    if (!m_impl->valid)
        return 0;

#ifdef DEEPLUX_HAS_OPENCV
    if (!m_impl->mat.empty()) {
        return m_impl->mat.cols;
    }
#endif

    if (!m_impl->qImage.isNull()) {
        return m_impl->qImage.width();
    }

    return 0;
}

int ImageData::height() const {
    if (!m_impl->valid)
        return 0;

#ifdef DEEPLUX_HAS_OPENCV
    if (!m_impl->mat.empty()) {
        return m_impl->mat.rows;
    }
#endif

    if (!m_impl->qImage.isNull()) {
        return m_impl->qImage.height();
    }

    return 0;
}

int ImageData::channels() const {
    if (!m_impl->valid)
        return 0;

#ifdef DEEPLUX_HAS_OPENCV
    if (!m_impl->mat.empty()) {
        return m_impl->mat.channels();
    }
#endif

    if (!m_impl->qImage.isNull()) {
        switch (m_impl->qImage.format()) {
        case QImage::Format_Grayscale8:
        case QImage::Format_Indexed8:
            return 1;
        case QImage::Format_RGB888:
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied:
            return 3; // RGB888 is 3 channels, RGB32/ARGB32 are stored as 4 but displayed as 3
        default:
            return m_impl->qImage.depth() / 8;
        }
    }

    return 0;
}

QString ImageData::format() const {
    if (!m_impl->valid)
        return QString();

#ifdef DEEPLUX_HAS_OPENCV
    if (!m_impl->mat.empty()) {
        int depth = m_impl->mat.depth();
        int channels = m_impl->mat.channels();
        if (depth == CV_8U && channels == 1)
            return "byte";
        if (depth == CV_8U && channels == 3)
            return "rgb";
        return "unknown";
    }
#endif

    if (!m_impl->qImage.isNull()) {
        switch (m_impl->qImage.format()) {
        case QImage::Format_Grayscale8:
            return "byte";
        case QImage::Format_RGB32:
            return "rgb";
        default:
            return "unknown";
        }
    }

    return QString();
}

QImage ImageData::toQImage() const {
    if (!m_impl->qImage.isNull()) {
        return m_impl->qImage;
    }

    if (!m_impl->valid) {
        return QImage();
    }

#ifdef DEEPLUX_HAS_OPENCV
    if (!m_impl->mat.empty()) {
        cv::Mat rgbMat;
        int depth = m_impl->mat.depth();

        // 处理 32 位浮点灰度图像 - 先转换为 8 位
        if (m_impl->mat.channels() == 1) {
            if (depth == CV_32F || depth == CV_64F) {
                // 归一化 32 位浮点到 0-255 范围
                cv::Mat normalized;
                cv::normalize(m_impl->mat, normalized, 0, 255, cv::NORM_MINMAX, CV_8UC1);
                cvtColor(normalized, rgbMat, cv::COLOR_GRAY2RGB);
            } else {
                cvtColor(m_impl->mat, rgbMat, cv::COLOR_GRAY2RGB);
            }
        } else if (m_impl->mat.channels() == 3) {
            if (depth == CV_32F || depth == CV_64F) {
                // 归一化并转换
                cv::Mat normalized;
                cv::normalize(m_impl->mat, normalized, 0, 255, cv::NORM_MINMAX, CV_8UC3);
                cvtColor(normalized, rgbMat, cv::COLOR_BGR2RGB);
            } else {
                cvtColor(m_impl->mat, rgbMat, cv::COLOR_BGR2RGB);
            }
        } else if (m_impl->mat.channels() == 4) {
            if (depth == CV_32F || depth == CV_64F) {
                cv::Mat normalized;
                cv::normalize(m_impl->mat, normalized, 0, 255, cv::NORM_MINMAX, CV_8UC4);
                cvtColor(normalized, rgbMat, cv::COLOR_BGRA2RGB);
            } else {
                cvtColor(m_impl->mat, rgbMat, cv::COLOR_BGRA2RGB);
            }
        } else {
            return QImage();
        }

        QImage::Format format = (rgbMat.channels() == 1) ? QImage::Format_Grayscale8 : QImage::Format_RGB888;
        return QImage(rgbMat.data, rgbMat.cols, rgbMat.rows, rgbMat.step, format).copy();
    }
#endif

    return QImage();
}

#ifdef DEEPLUX_HAS_OPENCV
const cv::Mat& ImageData::toMat() const {
    if (m_impl->valid && !m_impl->mat.empty()) {
        return m_impl->mat;
    }

    if (!m_impl->qImage.isNull()) {
        // Lazily convert and cache the result
        m_impl->mat = qImageToMat(m_impl->qImage);
        return m_impl->mat;
    }

    static cv::Mat emptyMat;
    return emptyMat;
}

bool ImageData::hasMat() const {
    return m_impl->valid && !m_impl->mat.empty();
}

void ImageData::setMat(const cv::Mat& mat) {
    m_impl->mat = mat;
    m_impl->valid = !mat.empty();
    m_impl->qImage = QImage();
}
#endif

void ImageData::setQImage(const QImage& image) {
    m_impl->qImage = image;

    if (image.isNull()) {
        m_impl->valid = false;
        return;
    }

    m_impl->valid = true;

#ifdef DEEPLUX_HAS_OPENCV
    m_impl->mat = qImageToMat(image);
#else
    Q_UNUSED(image);
#endif
}

#ifdef DEEPLUX_HAS_OPENCV
cv::Mat qImageToMat(const QImage& image) {
    if (image.isNull())
        return cv::Mat();

    QImage::Format format = image.format();
    cv::Mat mat;

    switch (format) {
    case QImage::Format_Grayscale8:
    case QImage::Format_Indexed8:
        mat = cv::Mat(image.height(), image.width(), CV_8UC1, (void*) image.constBits(), image.bytesPerLine()).clone();
        break;
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32: {
        cv::Mat tmp(image.height(), image.width(), CV_8UC4, (void*) image.constBits(), image.bytesPerLine());
        cvtColor(tmp, mat, cv::COLOR_BGRA2BGR);
    } break;
    case QImage::Format_RGB888: {
        cv::Mat tmp(image.height(), image.width(), CV_8UC3, (void*) image.constBits(), image.bytesPerLine());
        cvtColor(tmp, mat, cv::COLOR_RGB2BGR);
    } break;
    default: {
        QImage conv = image.convertToFormat(QImage::Format_RGB888);
        cv::Mat tmp(conv.height(), conv.width(), CV_8UC3, (void*) conv.constBits(), conv.bytesPerLine());
        cvtColor(tmp, mat, cv::COLOR_RGB2BGR);
    } break;
    }

    return mat;
}
#endif

void ImageData::setData(const QString& key, const QVariant& value) {
    m_impl->metadata[key] = value;
}

QVariant ImageData::data(const QString& key) const {
    return m_impl->metadata.value(key);
}

bool ImageData::hasData(const QString& key) const {
    return m_impl->metadata.contains(key);
}

void ImageData::removeData(const QString& key) {
    m_impl->metadata.remove(key);
}

QMap<QString, QVariant> ImageData::allData() const {
    return m_impl->metadata;
}

void ImageData::clearData() {
    m_impl->metadata.clear();
}

void ImageData::establishCoordinateSource(const QString& sourceImageId) {
    if (!isValid() || width() <= 0 || height() <= 0) {
        invalidateCoordinateProvenance();
        return;
    }
    m_impl->coordinateProvenance.sourceImageId =
        sourceImageId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : sourceImageId;
    m_impl->coordinateProvenance.sourceSize = QSize(width(), height());
    m_impl->coordinateProvenance.localOriginInSource = QPointF(0.0, 0.0);
    m_impl->coordinateProvenance.localToSource =
        ImageCoordinateProvenance::Matrix3{{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};
    m_impl->coordinateProvenance.hasLocalToSourceTransform = false;
}

void ImageData::setCoordinateProvenance(const ImageCoordinateProvenance& provenance) {
    m_impl->coordinateProvenance = provenance.isValid() ? provenance : ImageCoordinateProvenance();
}

void ImageData::offsetCoordinateOrigin(int x, int y) {
    if (!hasCoordinateProvenance())
        return;
    if (!m_impl->coordinateProvenance.offsetLocalOrigin(QPointF(x, y)))
        invalidateCoordinateProvenance();
}

void ImageData::invalidateCoordinateProvenance() {
    m_impl->coordinateProvenance = ImageCoordinateProvenance();
}

bool ImageData::hasCoordinateProvenance() const {
    return m_impl->coordinateProvenance.isValid();
}

ImageCoordinateProvenance ImageData::coordinateProvenance() const {
    return m_impl->coordinateProvenance;
}

void ImageData::setTimestamp(qint64 timestamp) {
    m_impl->timestamp = timestamp;
}

qint64 ImageData::timestamp() const {
    return m_impl->timestamp;
}

QByteArray ImageData::toByteArray() const {
    QImage img = toQImage();
    if (img.isNull()) {
        return QByteArray();
    }
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return data;
}

ImageData ImageData::fromByteArray(const QByteArray& data) {
    QImage img;
    img.loadFromData(data);
    ImageData result(img);
    // Byte serialization stores pixels only; it cannot preserve a derived image's parent frame.
    result.invalidateCoordinateProvenance();
    return result;
}

} // namespace DeepLux
