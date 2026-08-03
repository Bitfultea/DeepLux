#include "QRCodePlugin.h"

#include "common/Logger.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/objdetect.hpp>
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

QRCodePlugin::QRCodePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"codeType", "QR_Code"}};
    m_params = m_defaultParams;
}

QRCodePlugin::~QRCodePlugin() {}

bool QRCodePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "QRCodePlugin initialized";
    return true;
}

void QRCodePlugin::shutdown() {
    ModuleBase::shutdown();
}

bool QRCodePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

#ifdef DEEPLUX_HAS_OPENCV
    QJsonObject params = currentParams();
    QString codeType = params["codeType"].toString();

    if (codeType == "QR_Code") {
        return decodeQRCode(input, output);
    } else {
        CodeType type = CodeType::QR_Code;
        if (codeType == "Code_128")
            type = CodeType::Code_128;
        else if (codeType == "Code_39")
            type = CodeType::Code_39;
        else if (codeType == "EAN_13")
            type = CodeType::EAN_13;
        else if (codeType == "EAN_8")
            type = CodeType::EAN_8;
        return decodeBarCode(input, output, type);
    }
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool QRCodePlugin::decodeQRCode(const ImageData& input, ImageData& output) {
#ifdef DEEPLUX_HAS_OPENCV
    try {
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

        // 确保是灰度图
        cv::Mat gray;
        if (mat.channels() == 3) {
            cvtColor(mat, gray, cv::COLOR_BGR2GRAY);
        } else {
            gray = mat;
        }

        QString data;
        cv::Mat straightBarcode;

        // 使用OpenCV的QRCodeDetector
        bool found = m_qrDetector.detect(gray, straightBarcode);
        if (found) {
            data = QString::fromUtf8(m_qrDetector.decode(gray, straightBarcode).data());
        }

        // 如果QR码检测失败，尝试用简单方法
        if (data.isEmpty()) {
            // 尝试直接解码（某些情况下可以工作）
            data = QString::fromUtf8(m_qrDetector.decode(gray, cv::noArray()).data());
        }

        if (data.isEmpty()) {
            emit errorOccurred(tr("未检测到二维码"));
            return false;
        }

        m_decodedData = data;
        output.setData("qr_result", m_decodedData);
        output.setData("qr_type", "QR_Code");

        Logger::instance().debug(QString("二维码解码成功: %1").arg(m_decodedData), "QRCode");
        return true;

    } catch (const cv::Exception& e) {
        emit errorOccurred(QString("二维码解码失败: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
#else
    Q_UNUSED(input);
    Q_UNUSED(output);
    return false;
#endif
}

bool QRCodePlugin::decodeBarCode(const ImageData& input, ImageData& output, CodeType type) {
#ifdef DEEPLUX_HAS_OPENCV
    Q_UNUSED(input);
    Q_UNUSED(output);
    Q_UNUSED(type);
    emit errorOccurred(tr("条码检测需要额外库支持(如zbar)，当前仅支持QR码"));
    return false;
#else
    return false;
#endif
}

#ifdef DEEPLUX_HAS_OPENCV
// qImageToMat is defined in ImageData.cpp
#endif

bool QRCodePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    if (params["codeType"].toString() != QStringLiteral("QR_Code")) {
        error = tr("当前版本仅支持二维码识别");
        return false;
    }
    error.clear();
    return true;
}

QWidget* QRCodePlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("码类型:")));
    QComboBox* typeCombo = new QComboBox();
    typeCombo->addItem(tr("二维码"), "QR_Code");

    QString currentType = m_params["codeType"].toString();
    int index = typeCombo->findData(currentType);
    if (index >= 0)
        typeCombo->setCurrentIndex(index);
    layout->addWidget(typeCombo);

    layout->addStretch();

    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this, typeCombo](int index) { setParam("codeType", typeCombo->itemData(index).toString()); });

    return widget;
}

IModule* QRCodePlugin::cloneImpl() const {
    auto* clone = new QRCodePlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
