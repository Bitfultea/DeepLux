#include "PerProcessingPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

PerProcessingPlugin::PerProcessingPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"processType", "GaussianBlur"},
        {"kernelSize", 5},
        {"sigmaX", 1.5},
        {"iterations", 1}
    };
    m_params = m_defaultParams;
}

PerProcessingPlugin::~PerProcessingPlugin()
{
}

bool PerProcessingPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "PerProcessingPlugin initialized";
    return true;
}

void PerProcessingPlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_resultMat.release();
#endif
    ModuleBase::shutdown();
}

bool PerProcessingPlugin::process(const ImageData& input, ImageData& output)
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

    // 获取参数
    QString typeStr = m_params["processType"].toString();
    m_kernelSize = m_params["kernelSize"].toInt();
    m_sigmaX = m_params["sigmaX"].toDouble();
    m_iterations = m_params["iterations"].toInt();

    // 确保kernel size是奇数
    if (m_kernelSize % 2 == 0) m_kernelSize++;

    cv::Mat gray, processed;

    // 根据类型处理
    if (typeStr == "GaussianBlur") {
        cv::GaussianBlur(mat, m_resultMat, cv::Size(m_kernelSize, m_kernelSize), m_sigmaX);
    }
    else if (typeStr == "MedianBlur") {
        cv::medianBlur(mat, m_resultMat, m_kernelSize);
    }
    else if (typeStr == "BilateralFilter") {
        cv::bilateralFilter(mat, m_resultMat, m_kernelSize, m_sigmaX * 2, m_sigmaX / 2);
    }
    else if (typeStr == "Sobel") {
        cvtColor(mat, gray, cv::COLOR_BGR2GRAY);
        cv::Sobel(gray, m_resultMat, CV_16S, 1, 1);
        cv::convertScaleAbs(m_resultMat, m_resultMat);
    }
    else if (typeStr == "Laplacian") {
        cvtColor(mat, gray, cv::COLOR_BGR2GRAY);
        cv::Laplacian(gray, m_resultMat, CV_16S, m_kernelSize);
        cv::convertScaleAbs(m_resultMat, m_resultMat);
    }
    else if (typeStr == "Canny") {
        cvtColor(mat, gray, cv::COLOR_BGR2GRAY);
        cv::Canny(gray, m_resultMat, 50, 150, m_kernelSize);
    }
    else if (typeStr == "Dilate") {
        m_resultMat = applyMorphology(mat, cv::MORPH_DILATE);
    }
    else if (typeStr == "Erode") {
        m_resultMat = applyMorphology(mat, cv::MORPH_ERODE);
    }
    else if (typeStr == "Open") {
        m_resultMat = applyMorphology(mat, cv::MORPH_OPEN);
    }
    else if (typeStr == "Close") {
        m_resultMat = applyMorphology(mat, cv::MORPH_CLOSE);
    }
    else if (typeStr == "Gradient") {
        m_resultMat = applyMorphology(mat, cv::MORPH_GRADIENT);
    }
    else {
        m_resultMat = mat;
    }

    output.setMat(m_resultMat);
    output.setData("processed_type", typeStr);
    output.setData("processed_width", m_resultMat.cols);
    output.setData("processed_height", m_resultMat.rows);

    Logger::instance().debug(QString("预处理: %1, 尺寸: %2x%3")
                                .arg(typeStr).arg(m_resultMat.cols).arg(m_resultMat.rows), "PerProcessing");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

cv::Mat PerProcessingPlugin::applyMorphology(const cv::Mat& input, int operation)
{
#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(m_kernelSize, m_kernelSize));
    cv::Mat result;
    cv::morphologyEx(input, result, operation, kernel, cv::Point(-1, -1), m_iterations);
    return result;
#else
    return input;
#endif
}

bool PerProcessingPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    int kernelSize = params["kernelSize"].toInt();
    if (kernelSize < 1) {
        error = tr("核大小必须大于0");
        return false;
    }
    return true;
}

QWidget* PerProcessingPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("处理类型:")));
    QComboBox* typeCombo = new QComboBox();
    typeCombo->addItem("高斯模糊", "GaussianBlur");
    typeCombo->addItem("中值滤波", "MedianBlur");
    typeCombo->addItem("双边滤波", "BilateralFilter");
    typeCombo->addItem("Sobel边缘", "Sobel");
    typeCombo->addItem("Laplacian边缘", "Laplacian");
    typeCombo->addItem("Canny边缘", "Canny");
    typeCombo->addItem("膨胀", "Dilate");
    typeCombo->addItem("腐蚀", "Erode");
    typeCombo->addItem("开运算", "Open");
    typeCombo->addItem("闭运算", "Close");
    typeCombo->addItem("形态学梯度", "Gradient");

    QString currentType = m_params["processType"].toString();
    int idx = typeCombo->findData(currentType);
    if (idx >= 0) typeCombo->setCurrentIndex(idx);
    layout->addWidget(typeCombo);

    layout->addWidget(new QLabel(tr("核大小:")));
    QSpinBox* kernelSpin = new QSpinBox();
    kernelSpin->setRange(1, 31);
    kernelSpin->setSingleStep(2);
    kernelSpin->setValue(m_params["kernelSize"].toInt());
    layout->addWidget(kernelSpin);

    layout->addWidget(new QLabel(tr("迭代次数:")));
    QSpinBox* iterSpin = new QSpinBox();
    iterSpin->setRange(1, 10);
    iterSpin->setValue(m_params["iterations"].toInt());
    layout->addWidget(iterSpin);

    layout->addStretch();

    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, typeCombo](int) {
        m_params["processType"] = typeCombo->currentData().toString();
    });

    connect(kernelSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["kernelSize"] = value;
    });

    connect(iterSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["iterations"] = value;
    });

    return widget;
}

IModule* PerProcessingPlugin::cloneImpl() const
{
    PerProcessingPlugin* clone = new PerProcessingPlugin();
    clone->m_kernelSize = this->m_kernelSize;
    clone->m_sigmaX = this->m_sigmaX;
    clone->m_iterations = this->m_iterations;
    return clone;
}

} // namespace DeepLux