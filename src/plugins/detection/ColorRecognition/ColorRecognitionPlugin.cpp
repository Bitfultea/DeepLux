#include "ColorRecognitionPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

ColorRecognitionPlugin::ColorRecognitionPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"targetColor", "红色"}
    };
    m_params = m_defaultParams;
}

ColorRecognitionPlugin::~ColorRecognitionPlugin()
{
}

bool ColorRecognitionPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "ColorRecognitionPlugin initialized";
    return true;
}

void ColorRecognitionPlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_resultMat.release();
    m_mask.release();
#endif
    ModuleBase::shutdown();
}

bool ColorRecognitionPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

#ifdef DEEPLUX_HAS_OPENCV
    // 获取图像
    cv::Mat image;
    if (input.hasMat()) {
        image = input.toMat();
    } else {
        image = qImageToMat(input.toQImage());
    }

    if (image.empty()) {
        emit errorOccurred(tr("输入图像无效"));
        return false;
    }

    m_targetColor = m_params["targetColor"].toString();

    // 定义颜色范围
    std::vector<ColorRange> colorRanges = {
        {"红色", cv::Scalar(0, 100, 100), cv::Scalar(10, 255, 255)},
        {"绿色", cv::Scalar(35, 100, 100), cv::Scalar(85, 255, 255)},
        {"蓝色", cv::Scalar(100, 100, 100), cv::Scalar(130, 255, 255)},
        {"黄色", cv::Scalar(15, 100, 100), cv::Scalar(35, 255, 255)},
        {"橙色", cv::Scalar(10, 100, 100), cv::Scalar(25, 255, 255)},
        {"紫色", cv::Scalar(130, 100, 100), cv::Scalar(170, 255, 255)}
    };

    // 转换为HSV颜色空间
    cv::Mat hsv;
    cvtColor(image, hsv, cv::COLOR_BGR2HSV);

    // 找到匹配的颜色
    ColorRange selectedRange;
    bool found = false;

    for (const auto& range : colorRanges) {
        if (range.name == m_targetColor) {
            selectedRange = range;
            found = true;
            break;
        }
    }

    if (!found) {
        selectedRange = colorRanges[0];  // 默认红色
    }

    // 检测颜色
    double area = 0;
    detectColor(hsv, selectedRange, m_mask, area);

    if (area < 100) {
        emit errorOccurred(tr("未检测到目标颜色"));
        return false;
    }

    // 在原图上绘制结果
    m_resultMat = image.clone();
    cv::bitwise_and(image, image, m_resultMat, m_mask);

    // 找轮廓
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(m_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    // 绘制最大轮廓
    if (!contours.empty()) {
        double maxArea = 0;
        std::vector<cv::Point> maxContour;

        for (const auto& contour : contours) {
            double a = cv::contourArea(contour);
            if (a > maxArea) {
                maxArea = a;
                maxContour = contour;
            }
        }

        if (!maxContour.empty()) {
            cv::Rect boundingRect = cv::boundingRect(maxContour);
            cv::rectangle(m_resultMat, boundingRect, cv::Scalar(0, 255, 0), 2);

            // 计算中心点
            cv::Moments moments = cv::moments(maxContour);
            int centerX = static_cast<int>(moments.m10 / moments.m00);
            int centerY = static_cast<int>(moments.m01 / moments.m00);

            output.setData("color_center_x", centerX);
            output.setData("color_center_y", centerY);
        }
    }

    m_colorCount = static_cast<int>(contours.size());

    output.setMat(m_resultMat);
    output.setData("color_name", m_targetColor);
    output.setData("color_area", area);
    output.setData("color_count", m_colorCount);

    Logger::instance().debug(QString("颜色识别: %1, 面积=%2, 数量=%3")
                                .arg(m_targetColor).arg(area).arg(m_colorCount), "ColorRecognition");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool ColorRecognitionPlugin::detectColor(const cv::Mat& hsv, const ColorRange& range,
                                         cv::Mat& mask, double& area)
{
#ifdef DEEPLUX_HAS_OPENCV
    cv::inRange(hsv, range.lower, range.upper, mask);

    // 形态学操作去噪
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

    // 计算面积
    area = cv::countNonZero(mask);

    return area > 0;
#else
    return false;
#endif
}

bool ColorRecognitionPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* ColorRecognitionPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("目标颜色:")));
    QComboBox* colorCombo = new QComboBox();
    colorCombo->addItem("红色", "红色");
    colorCombo->addItem("绿色", "绿色");
    colorCombo->addItem("蓝色", "蓝色");
    colorCombo->addItem("黄色", "黄色");
    colorCombo->addItem("橙色", "橙色");
    colorCombo->addItem("紫色", "紫色");

    int idx = colorCombo->findData(m_params["targetColor"].toString());
    if (idx >= 0) colorCombo->setCurrentIndex(idx);
    layout->addWidget(colorCombo);

    layout->addStretch();

    connect(colorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, colorCombo](int) {
        m_params["targetColor"] = colorCombo->currentData().toString();
    });

    return widget;
}

IModule* ColorRecognitionPlugin::cloneImpl() const
{
    ColorRecognitionPlugin* clone = new ColorRecognitionPlugin();
    return clone;
}

} // namespace DeepLux
