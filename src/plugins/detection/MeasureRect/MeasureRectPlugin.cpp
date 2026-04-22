#include "MeasureRectPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

MeasureRectPlugin::MeasureRectPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"minArea", 100.0},
        {"maxArea", 100000.0},
        {"threshold1", 50.0},
        {"threshold2", 150.0}
    };
    m_params = m_defaultParams;
}

MeasureRectPlugin::~MeasureRectPlugin()
{
}

bool MeasureRectPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "MeasureRectPlugin initialized";
    return true;
}

void MeasureRectPlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_gray.release();
    m_edges.release();
#endif
    ModuleBase::shutdown();
}

bool MeasureRectPlugin::process(const ImageData& input, ImageData& output)
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

    // 使用Canny边缘检测
    cv::Canny(m_gray, m_edges, m_threshold1, m_threshold2, 3);

    // 查找轮廓
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(m_edges, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    if (contours.empty()) {
        emit errorOccurred(tr("未检测到轮廓"));
        return false;
    }

    // 找到最接近矩形的轮廓
    std::vector<cv::Point> bestContour;
    double bestRectArea = 0;
    cv::RotatedRect bestRect;

    for (const auto& contour : contours) {
        double area = cv::contourArea(contour);
        if (area < m_minArea || area > m_maxArea) continue;

        // 使用最小外接矩形
        cv::RotatedRect rect = cv::minAreaRect(contour);

        // 检查是否为近似矩形 (通过角度判断)
        cv::Point2f corners[4];
        rect.points(corners);

        // 计算矩形度 (面积 / 最小外接矩形面积)
        double rectArea = rect.size.width * rect.size.height;
        double rectangularity = area / rectArea;

        if (rectangularity > 0.8 && area > bestRectArea) {
            bestRectArea = area;
            bestContour = contour;
            bestRect = rect;
        }
    }

    if (bestContour.empty()) {
        emit errorOccurred(tr("未检测到矩形"));
        return false;
    }

    // 获取矩形的四个角点
    cv::Point2f corners[4];
    bestRect.points(corners);

    // 按位置排序角点 (左上, 右上, 右下, 左下)
    std::sort(corners, corners + 4, [](const cv::Point2f& a, const cv::Point2f& b) {
        return (a.x + a.y * 10) < (b.x + b.y * 10);
    });

    // 计算宽高
    double width = cv::norm(corners[1] - corners[0]);
    double height = cv::norm(corners[3] - corners[0]);

    m_resultRow1 = corners[0].y;
    m_resultCol1 = corners[0].x;
    m_resultRow2 = corners[2].y;
    m_resultCol2 = corners[2].x;
    m_resultWidth = width;
    m_resultHeight = height;
    m_resultAngle = bestRect.angle;
    m_resultArea = bestRectArea;

    // 设置输出数据
    output.setData("rect_row1", m_resultRow1);
    output.setData("rect_col1", m_resultCol1);
    output.setData("rect_row2", m_resultRow2);
    output.setData("rect_col2", m_resultCol2);
    output.setData("rect_width", m_resultWidth);
    output.setData("rect_height", m_resultHeight);
    output.setData("rect_angle", m_resultAngle);
    output.setData("rect_area", m_resultArea);

    QString result = QString("矩形: 宽=%1, 高=%2, 角度=%3°, 面积=%4")
                        .arg(m_resultWidth, 0, 'f', 1)
                        .arg(m_resultHeight, 0, 'f', 1)
                        .arg(m_resultAngle, 0, 'f', 1)
                        .arg(m_resultArea, 0, 'f', 1);
    Logger::instance().debug(result, "MeasureRect");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool MeasureRectPlugin::doValidateParams(const QJsonObject& params, QString& error) const
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

QWidget* MeasureRectPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("最小面积 (像素²):")));
    QDoubleSpinBox* minAreaSpin = new QDoubleSpinBox();
    minAreaSpin->setRange(1.0, 1000000.0);
    minAreaSpin->setValue(m_params["minArea"].toDouble());
    minAreaSpin->setSingleStep(100);
    layout->addWidget(minAreaSpin);

    layout->addWidget(new QLabel(tr("最大面积 (像素²):")));
    QDoubleSpinBox* maxAreaSpin = new QDoubleSpinBox();
    maxAreaSpin->setRange(1.0, 10000000.0);
    maxAreaSpin->setValue(m_params["maxArea"].toDouble());
    maxAreaSpin->setSingleStep(100);
    layout->addWidget(maxAreaSpin);

    layout->addWidget(new QLabel(tr("Canny阈值1:")));
    QDoubleSpinBox* thresh1Spin = new QDoubleSpinBox();
    thresh1Spin->setRange(1.0, 200.0);
    thresh1Spin->setValue(m_params["threshold1"].toDouble());
    thresh1Spin->setSingleStep(5);
    layout->addWidget(thresh1Spin);

    layout->addWidget(new QLabel(tr("Canny阈值2:")));
    QDoubleSpinBox* thresh2Spin = new QDoubleSpinBox();
    thresh2Spin->setRange(1.0, 300.0);
    thresh2Spin->setValue(m_params["threshold2"].toDouble());
    thresh2Spin->setSingleStep(5);
    layout->addWidget(thresh2Spin);

    layout->addStretch();

    connect(minAreaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["minArea"] = value; });

    connect(maxAreaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["maxArea"] = value; });

    connect(thresh1Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["threshold1"] = value; });

    connect(thresh2Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["threshold2"] = value; });

    return widget;
}

IModule* MeasureRectPlugin::cloneImpl() const
{
    MeasureRectPlugin* clone = new MeasureRectPlugin();
    return clone;
}

} // namespace DeepLux
