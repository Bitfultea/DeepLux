#include "JiErHanDefectsDetPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

JiErHanDefectsDetPlugin::JiErHanDefectsDetPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"threshold", 0.5}
    };
    m_params = m_defaultParams;
}

JiErHanDefectsDetPlugin::~JiErHanDefectsDetPlugin()
{
}

bool JiErHanDefectsDetPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "JiErHanDefectsDetPlugin initialized";
    return true;
}

void JiErHanDefectsDetPlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_resultMat.release();
#endif
    ModuleBase::shutdown();
}

bool JiErHanDefectsDetPlugin::process(const ImageData& input, ImageData& output)
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

    m_threshold = m_params["threshold"].toDouble();

    // 检测缺陷
    std::vector<DefectResult> defects = detectDefects(image);

    if (defects.empty()) {
        emit errorOccurred(tr("未检测到缺陷"));
        return false;
    }

    m_defectCount = static_cast<int>(defects.size());

    // 绘制缺陷区域
    m_resultMat = image.clone();
    for (size_t i = 0; i < defects.size(); ++i) {
        const auto& defect = defects[i];
        cv::rectangle(m_resultMat, defect.boundingBox, cv::Scalar(0, 0, 255), 2);

        QString label = QString("%1: %2%").arg(defect.type).arg(defect.confidence * 100, 0, 'f', 1);
        cv::putText(m_resultMat, label.toUtf8().constData(),
                    cv::Point(defect.boundingBox.x, defect.boundingBox.y - 5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 255), 2);
    }

    output.setMat(m_resultMat);
    output.setData("defect_count", m_defectCount);
    output.setData("defect_threshold", m_threshold);

    // 返回第一个缺陷的位置
    if (!defects.empty()) {
        output.setData("defect_x", defects[0].boundingBox.x);
        output.setData("defect_y", defects[0].boundingBox.y);
        output.setData("defect_width", defects[0].boundingBox.width);
        output.setData("defect_height", defects[0].boundingBox.height);
        output.setData("defect_confidence", defects[0].confidence);
    }

    Logger::instance().debug(QString("剑二韩缺陷检测: 发现%1个缺陷").arg(m_defectCount), "JiErHanDefectsDet");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

std::vector<JiErHanDefectsDetPlugin::DefectResult> JiErHanDefectsDetPlugin::detectDefects(const cv::Mat& image)
{
    std::vector<DefectResult> results;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat gray, binary;
    if (image.channels() == 3) {
        cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image;
    }

    // 边缘检测
    cv::Canny(gray, binary, 50, 150, 3);

    // 形态学操作
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, kernel);

    // 找轮廓
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    for (const auto& contour : contours) {
        double area = cv::contourArea(contour);
        if (area > 100 && area < image.cols * image.rows * 0.1) {
            cv::Rect bbox = cv::boundingRect(contour);

            DefectResult result;
            result.type = "焊接缺陷";
            result.confidence = qMin(1.0, area / 1000.0);  // 简化的置信度计算
            result.boundingBox = bbox;

            results.push_back(result);
        }
    }
#endif

    return results;
}

bool JiErHanDefectsDetPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    double threshold = params["threshold"].toDouble();
    if (threshold <= 0 || threshold > 1) {
        error = tr("阈值必须在0到1之间");
        return false;
    }
    return true;
}

QWidget* JiErHanDefectsDetPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("检测阈值 (0-1):")));
    QDoubleSpinBox* threshSpin = new QDoubleSpinBox();
    threshSpin->setRange(0.1, 1.0);
    threshSpin->setSingleStep(0.05);
    threshSpin->setValue(m_params["threshold"].toDouble());
    layout->addWidget(threshSpin);

    layout->addStretch();

    connect(threshSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_params["threshold"] = value;
    });

    return widget;
}

IModule* JiErHanDefectsDetPlugin::cloneImpl() const
{
    JiErHanDefectsDetPlugin* clone = new JiErHanDefectsDetPlugin();
    return clone;
}

} // namespace DeepLux
