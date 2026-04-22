#include "FindCirclePlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

FindCirclePlugin::FindCirclePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"minRadius", 10.0},
        {"maxRadius", 500.0},
        {"param1", 50.0},
        {"param2", 30.0}
    };
    m_params = m_defaultParams;
}

FindCirclePlugin::~FindCirclePlugin()
{
}

bool FindCirclePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FindCirclePlugin initialized";
    return true;
}

void FindCirclePlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_gray.release();
#endif
    ModuleBase::shutdown();
}

bool FindCirclePlugin::process(const ImageData& input, ImageData& output)
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

    // 应用高斯模糊减少噪声
    GaussianBlur(m_gray, m_gray, cv::Size(9, 9), 2, 2);

    double centerX, centerY, radius;
    bool success = findCircleHough(m_gray, centerX, centerY, radius);

    if (!success) {
        emit errorOccurred(tr("未检测到圆"));
        return false;
    }

    m_resultCenterX = centerX;
    m_resultCenterY = centerY;
    m_resultRadius = radius;
    m_resultScore = 1.0; // Hough变换返回的是最佳圆的得分

    // 设置输出数据
    output.setData("circle_center_x", m_resultCenterX);
    output.setData("circle_center_y", m_resultCenterY);
    output.setData("circle_radius", m_resultRadius);
    output.setData("circle_score", m_resultScore);

    QString result = QString("圆: 中心(%1, %2), 半径=%3")
                        .arg(centerX, 0, 'f', 1)
                        .arg(centerY, 0, 'f', 1)
                        .arg(radius, 0, 'f', 1);
    Logger::instance().debug(result, "FindCircle");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool FindCirclePlugin::findCircleHough(const cv::Mat& gray, double& centerX, double& centerY, double& radius)
{
#ifdef DEEPLUX_HAS_OPENCV
    std::vector<cv::Vec3f> circles;

    // Hough圆变换
    cv::HoughCircles(gray, circles, cv::HOUGH_GRADIENT, 1,
                      gray.rows / 8,  // 圆心之间的最小距离
                      m_param1,       // Canny高阈值
                      m_param2,       // 累加器阈值
                      static_cast<int>(m_minRadius),  // 最小半径
                      static_cast<int>(m_maxRadius)); // 最大半径

    if (circles.empty()) {
        return false;
    }

    // 选择最佳的圆
    const cv::Vec3f& bestCircle = circles[0];
    centerX = bestCircle[0];
    centerY = bestCircle[1];
    radius = bestCircle[2];

    return true;
#else
    Q_UNUSED(gray);
    Q_UNUSED(centerX);
    Q_UNUSED(centerY);
    Q_UNUSED(radius);
    return false;
#endif
}

bool FindCirclePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    double minR = params["minRadius"].toDouble();
    double maxR = params["maxRadius"].toDouble();

    if (minR <= 0) {
        error = tr("最小半径必须大于0");
        return false;
    }

    if (maxR <= minR) {
        error = tr("最大半径必须大于最小半径");
        return false;
    }

    return true;
}

QWidget* FindCirclePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("最小半径 (像素):")));
    QDoubleSpinBox* minRadiusSpin = new QDoubleSpinBox();
    minRadiusSpin->setRange(1.0, 1000.0);
    minRadiusSpin->setValue(m_params["minRadius"].toDouble());
    minRadiusSpin->setSingleStep(1.0);
    layout->addWidget(minRadiusSpin);

    layout->addWidget(new QLabel(tr("最大半径 (像素):")));
    QDoubleSpinBox* maxRadiusSpin = new QDoubleSpinBox();
    maxRadiusSpin->setRange(1.0, 2000.0);
    maxRadiusSpin->setValue(m_params["maxRadius"].toDouble());
    maxRadiusSpin->setSingleStep(1.0);
    layout->addWidget(maxRadiusSpin);

    layout->addWidget(new QLabel(tr("边缘阈值 (Canny):")));
    QDoubleSpinBox* param1Spin = new QDoubleSpinBox();
    param1Spin->setRange(1.0, 200.0);
    param1Spin->setValue(m_params["param1"].toDouble());
    param1Spin->setSingleStep(5.0);
    layout->addWidget(param1Spin);

    layout->addWidget(new QLabel(tr("累加器阈值:")));
    QDoubleSpinBox* param2Spin = new QDoubleSpinBox();
    param2Spin->setRange(1.0, 200.0);
    param2Spin->setValue(m_params["param2"].toDouble());
    param2Spin->setSingleStep(1.0);
    layout->addWidget(param2Spin);

    layout->addStretch();

    connect(minRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["minRadius"] = value; });

    connect(maxRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["maxRadius"] = value; });

    connect(param1Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["param1"] = value; });

    connect(param2Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["param2"] = value; });

    return widget;
}

IModule* FindCirclePlugin::cloneImpl() const
{
    FindCirclePlugin* clone = new FindCirclePlugin();
    return clone;
}

} // namespace DeepLux
