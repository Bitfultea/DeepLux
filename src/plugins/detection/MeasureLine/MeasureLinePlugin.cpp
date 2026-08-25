#include "MeasureLinePlugin.h"

#include "common/Logger.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QVBoxLayout>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

MeasureLinePlugin::MeasureLinePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{
        {"minLength", 20.0}, {"maxLength", 1000.0}, {"threshold", 50.0}, {"minAngle", 0.0}, {"maxAngle", 180.0}};
    m_params = m_defaultParams;
}

MeasureLinePlugin::~MeasureLinePlugin() {}

bool MeasureLinePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "MeasureLinePlugin initialized";
    return true;
}

void MeasureLinePlugin::shutdown() {
#ifdef DEEPLUX_HAS_OPENCV
    m_gray.release();
#endif
    ModuleBase::shutdown();
}

bool MeasureLinePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

#ifdef DEEPLUX_HAS_OPENCV
    // 阶段 3: 从 currentParams() 读取参数到局部变量
    QJsonObject params = currentParams();
    const double minLength = params["minLength"].toDouble(20.0);
    const double maxLength = params["maxLength"].toDouble(1000.0);
    const double threshold = params["threshold"].toDouble(50.0);
    const double minAngle = params["minAngle"].toDouble(0.0);
    const double maxAngle = params["maxAngle"].toDouble(180.0);

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

    // 使用Canny边缘检测（threshold 参数作为低阈值，2*threshold 作为高阈值）
    cv::Mat edges;
    cv::Canny(m_gray, edges, threshold, threshold * 3.0, 3);

    // 使用Hough变换检测直线
    std::vector<cv::Vec4i> lines;
    detectLines(edges, lines, minLength, maxLength, threshold, minAngle, maxAngle);

    if (lines.empty()) {
        emit errorOccurred(tr("未检测到线条"));
        return false;
    }

    // 选择最长的线
    double bestLength = 0;
    cv::Vec4i bestLine = lines[0];

    for (const cv::Vec4i& l : lines) {
        double length = sqrt(pow(l[2] - l[0], 2) + pow(l[3] - l[1], 2));
        if (length > bestLength) {
            bestLength = length;
            bestLine = l;
        }
    }

    m_resultRow1 = bestLine[1];
    m_resultCol1 = bestLine[0];
    m_resultRow2 = bestLine[3];
    m_resultCol2 = bestLine[2];
    m_resultLength = bestLength;

    // 计算角度 (0-180度)
    double dx = m_resultCol2 - m_resultCol1;
    double dy = m_resultRow2 - m_resultRow1;
    m_resultAngle = atan2(dy, dx) * 180.0 / CV_PI;
    if (m_resultAngle < 0)
        m_resultAngle += 180.0;

    m_resultLineCount = static_cast<int>(lines.size());

    // 设置输出数据
    output.setData("line_row1", m_resultRow1);
    output.setData("line_col1", m_resultCol1);
    output.setData("line_row2", m_resultRow2);
    output.setData("line_col2", m_resultCol2);
    output.setData("line_length", m_resultLength);
    output.setData("line_angle", m_resultAngle);
    output.setData("line_count", m_resultLineCount);

    QString result = QString("线条: 长度=%1, 角度=%2°, 端点1(%3,%4), 端点2(%5,%6)")
                         .arg(m_resultLength, 0, 'f', 1)
                         .arg(m_resultAngle, 0, 'f', 1)
                         .arg(m_resultRow1, 0, 'f', 1)
                         .arg(m_resultCol1, 0, 'f', 1)
                         .arg(m_resultRow2, 0, 'f', 1)
                         .arg(m_resultCol2, 0, 'f', 1);
    Logger::instance().debug(result, "MeasureLine");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool MeasureLinePlugin::detectLines(const cv::Mat& gray, std::vector<cv::Vec4i>& lines, double minLength,
                                    double maxLength, double threshold, double minAngle, double maxAngle) {
#ifdef DEEPLUX_HAS_OPENCV
    std::vector<cv::Vec4i> allLines;
    // 阶段 3: 使用局部参数，maxLength 不再被误当作 Hough maxLineGap
    // HoughLinesP 的 maxLineGap 使用固定值 10
    cv::HoughLinesP(gray, allLines, 1, CV_PI / 180, static_cast<int>(threshold), minLength, 10);

    // 过滤角度和长度
    for (const cv::Vec4i& l : allLines) {
        double dx = static_cast<double>(l[2] - l[0]);
        double dy = static_cast<double>(l[3] - l[1]);
        double length = sqrt(dx * dx + dy * dy);
        if (length > maxLength)
            continue;

        double angle = atan2(dy, dx) * 180.0 / CV_PI;
        if (angle < 0)
            angle += 180.0;

        if (angle >= minAngle && angle <= maxAngle) {
            lines.push_back(l);
        }
    }

    return !lines.empty();
#else
    Q_UNUSED(gray);
    Q_UNUSED(lines);
    Q_UNUSED(minLength);
    Q_UNUSED(maxLength);
    Q_UNUSED(threshold);
    Q_UNUSED(minAngle);
    Q_UNUSED(maxAngle);
    return false;
#endif
}

bool MeasureLinePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    const double threshold = params["threshold"].toDouble();
    double minLength = params["minLength"].toDouble();
    double maxLength = params["maxLength"].toDouble();
    const double minAngle = params["minAngle"].toDouble();
    const double maxAngle = params["maxAngle"].toDouble();

    if (threshold <= 0) {
        error = tr("阈值必须大于0");
        return false;
    }

    if (minLength <= 0) {
        error = tr("最小长度必须大于0");
        return false;
    }

    if (maxLength <= minLength) {
        error = tr("最大长度必须大于最小长度");
        return false;
    }

    if (minAngle < 0.0 || maxAngle > 180.0) {
        error = tr("角度范围必须在0到180度之间");
        return false;
    }

    if (maxAngle < minAngle) {
        error = tr("最大角度必须大于或等于最小角度");
        return false;
    }

    return true;
}

QWidget* MeasureLinePlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("最小长度 (像素):")));
    QDoubleSpinBox* minLengthSpin = new QDoubleSpinBox();
    minLengthSpin->setRange(1.0, 2000.0);
    minLengthSpin->setValue(m_params["minLength"].toDouble());
    minLengthSpin->setSingleStep(5.0);
    layout->addWidget(minLengthSpin);

    layout->addWidget(new QLabel(tr("最大长度 (像素):")));
    QDoubleSpinBox* maxLengthSpin = new QDoubleSpinBox();
    maxLengthSpin->setRange(1.0, 5000.0);
    maxLengthSpin->setValue(m_params["maxLength"].toDouble());
    maxLengthSpin->setSingleStep(10.0);
    layout->addWidget(maxLengthSpin);

    layout->addWidget(new QLabel(tr("Hough阈值:")));
    QDoubleSpinBox* thresholdSpin = new QDoubleSpinBox();
    thresholdSpin->setRange(1.0, 200.0);
    thresholdSpin->setValue(m_params["threshold"].toDouble());
    thresholdSpin->setSingleStep(5.0);
    layout->addWidget(thresholdSpin);

    layout->addStretch();

    connect(minLengthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { setParam("minLength", value); });

    connect(maxLengthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { setParam("maxLength", value); });

    connect(thresholdSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { setParam("threshold", value); });

    return widget;
}

IModule* MeasureLinePlugin::cloneImpl() const {
    MeasureLinePlugin* clone = new MeasureLinePlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
