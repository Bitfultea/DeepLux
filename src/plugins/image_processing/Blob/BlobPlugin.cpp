#include "BlobPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

BlobPlugin::BlobPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"minArea", 10.0},
        {"maxArea", 10000.0},
        {"minCircularity", 0.5},
        {"thresholdType", "Otsu"},
        {"fixedThreshold", 127},
        {"adaptiveBlockSize", 11},
        {"adaptiveC", 2}
    };
    m_params = m_defaultParams;
}

BlobPlugin::~BlobPlugin()
{
}

bool BlobPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "BlobPlugin initialized";
    return true;
}

void BlobPlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_gray.release();
    m_mask.release();
#endif
    ModuleBase::shutdown();
}

bool BlobPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

#ifdef DEEPLUX_HAS_OPENCV
    // 获取图像
    cv::Mat mat;
    if (input.hasMat()) {
        mat = input.toMat();
    } else {
        mat = qImageToMat(input.toQImage());
    }

    if (mat.empty()) {
        emit errorOccurred(tr("输入图像无效"));
        return false;
    }

    // 转换为灰度图
    if (mat.channels() == 3) {
        cvtColor(mat, m_gray, cv::COLOR_BGR2GRAY);
    } else {
        m_gray = mat;
    }

    // 二值化
    applyThreshold(m_gray, m_mask);

    // 检测Blob
    std::vector<BlobResult> blobs = detectBlobs(m_gray, m_mask);

    if (blobs.empty()) {
        emit errorOccurred(tr("未检测到Blob"));
        return false;
    }

    m_blobCount = static_cast<int>(blobs.size());

    // 设置输出数据
    output.setData("blob_count", m_blobCount);

    // 返回最大Blob的信息
    BlobResult maxBlob = blobs[0];
    for (const auto& blob : blobs) {
        if (blob.area > maxBlob.area) {
            maxBlob = blob;
        }
    }

    output.setData("blob_center_x", maxBlob.centerX);
    output.setData("blob_center_y", maxBlob.centerY);
    output.setData("blob_area", maxBlob.area);
    output.setData("blob_perimeter", maxBlob.perimeter);
    output.setData("blob_circularity", maxBlob.circularity);

    QString result = QString("Blob: 数量=%1, 最大: 中心(%2,%3), 面积=%4, 圆度=%5")
                        .arg(m_blobCount)
                        .arg(maxBlob.centerX, 0, 'f', 1)
                        .arg(maxBlob.centerY, 0, 'f', 1)
                        .arg(maxBlob.area, 0, 'f', 1)
                        .arg(maxBlob.circularity, 0, 'f', 2);
    Logger::instance().debug(result, "Blob");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

std::vector<BlobPlugin::BlobResult> BlobPlugin::detectBlobs(const cv::Mat& gray, const cv::Mat& mask)
{
    std::vector<BlobResult> results;

#ifdef DEEPLUX_HAS_OPENCV
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    for (const auto& contour : contours) {
        double area = cv::contourArea(contour);

        if (area < m_minArea || area > m_maxArea) continue;

        // 计算周长
        double perimeter = cv::arcLength(contour, true);

        // 计算圆度 (4 * PI * area / perimeter^2)
        double circularity = 0;
        if (perimeter > 0) {
            circularity = 4 * CV_PI * area / (perimeter * perimeter);
        }

        if (circularity < m_minCircularity) continue;

        // 计算中心
        cv::Moments moments = cv::moments(contour);
        double centerX = moments.m10 / moments.m00;
        double centerY = moments.m01 / moments.m00;

        BlobResult blob;
        blob.centerX = centerX;
        blob.centerY = centerY;
        blob.area = area;
        blob.perimeter = perimeter;
        blob.circularity = circularity;

        results.push_back(blob);
    }
#endif

    return results;
}

void BlobPlugin::applyThreshold(const cv::Mat& gray, cv::Mat& mask)
{
#ifdef DEEPLUX_HAS_OPENCV
    // 获取阈值参数
    QString typeStr = m_params["thresholdType"].toString("Otsu");
    m_fixedThreshold = m_params["fixedThreshold"].toInt(127);
    m_adaptiveBlockSize = m_params["adaptiveBlockSize"].toInt(11);
    m_adaptiveC = m_params["adaptiveC"].toInt(2);

    // 确保block size是奇数
    if (m_adaptiveBlockSize % 2 == 0) m_adaptiveBlockSize++;

    if (typeStr == "Fixed") {
        cv::threshold(gray, mask, m_fixedThreshold, 255, cv::THRESH_BINARY);
    } else if (typeStr == "Adaptive") {
        cv::adaptiveThreshold(gray, mask, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                            cv::THRESH_BINARY, m_adaptiveBlockSize, m_adaptiveC);
    } else { // Otsu
        cv::threshold(gray, mask, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    }
#else
    Q_UNUSED(gray);
    Q_UNUSED(mask);
#endif
}

bool BlobPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    double minArea = params["minArea"].toDouble();
    double maxArea = params["maxArea"].toDouble();

    if (minArea <= 0) {
        error = tr("最小面积必须大于0");
        return false;
    }

    if (maxArea <= minArea) {
        error = tr("最大面积必须大于最小面积");
        return false;
    }

    return true;
}

QWidget* BlobPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("阈值类型:")));
    QComboBox* threshTypeCombo = new QComboBox();
    threshTypeCombo->addItem("Otsu自动", "Otsu");
    threshTypeCombo->addItem("固定阈值", "Fixed");
    threshTypeCombo->addItem("自适应阈值", "Adaptive");
    QString currentType = m_params["thresholdType"].toString("Otsu");
    int idx = threshTypeCombo->findData(currentType);
    if (idx >= 0) threshTypeCombo->setCurrentIndex(idx);
    layout->addWidget(threshTypeCombo);

    layout->addWidget(new QLabel(tr("固定阈值:")));
    QSpinBox* fixedThreshSpin = new QSpinBox();
    fixedThreshSpin->setRange(0, 255);
    fixedThreshSpin->setValue(m_params["fixedThreshold"].toInt(127));
    layout->addWidget(fixedThreshSpin);

    layout->addWidget(new QLabel(tr("自适应块大小 (奇数):")));
    QSpinBox* blockSizeSpin = new QSpinBox();
    blockSizeSpin->setRange(3, 51);
    blockSizeSpin->setSingleStep(2);
    blockSizeSpin->setValue(m_params["adaptiveBlockSize"].toInt(11));
    layout->addWidget(blockSizeSpin);

    layout->addWidget(new QLabel(tr("自适应C常数:")));
    QSpinBox* adaptiveCSpin = new QSpinBox();
    adaptiveCSpin->setRange(-50, 50);
    adaptiveCSpin->setValue(m_params["adaptiveC"].toInt(2));
    layout->addWidget(adaptiveCSpin);

    layout->addWidget(new QLabel(tr("最小面积 (像素²):")));
    QDoubleSpinBox* minAreaSpin = new QDoubleSpinBox();
    minAreaSpin->setRange(1.0, 100000.0);
    minAreaSpin->setValue(m_params["minArea"].toDouble());
    minAreaSpin->setSingleStep(10);
    layout->addWidget(minAreaSpin);

    layout->addWidget(new QLabel(tr("最大面积 (像素²):")));
    QDoubleSpinBox* maxAreaSpin = new QDoubleSpinBox();
    maxAreaSpin->setRange(1.0, 1000000.0);
    maxAreaSpin->setValue(m_params["maxArea"].toDouble());
    maxAreaSpin->setSingleStep(100);
    layout->addWidget(maxAreaSpin);

    layout->addWidget(new QLabel(tr("最小圆度:")));
    QDoubleSpinBox* circSpin = new QDoubleSpinBox();
    circSpin->setRange(0.0, 1.0);
    circSpin->setValue(m_params["minCircularity"].toDouble());
    circSpin->setSingleStep(0.05);
    layout->addWidget(circSpin);

    layout->addStretch();

    connect(threshTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, threshTypeCombo](int) { m_params["thresholdType"] = threshTypeCombo->currentData().toString(); });

    connect(fixedThreshSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int value) { m_params["fixedThreshold"] = value; });

    connect(blockSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int value) { m_params["adaptiveBlockSize"] = value; });

    connect(adaptiveCSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int value) { m_params["adaptiveC"] = value; });

    connect(minAreaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["minArea"] = value; });

    connect(maxAreaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["maxArea"] = value; });

    connect(circSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["minCircularity"] = value; });

    return widget;
}

IModule* BlobPlugin::cloneImpl() const
{
    BlobPlugin* clone = new BlobPlugin();
    return clone;
}

} // namespace DeepLux
