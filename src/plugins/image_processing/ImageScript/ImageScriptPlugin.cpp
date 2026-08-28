#include "ImageScriptPlugin.h"

#include "common/Logger.h"

#include <QComboBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QtMath>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

ImageScriptPlugin::ImageScriptPlugin(QObject* parent) : ModuleBase(parent) {
    // 阶段 2：仅保留 scriptType 持久化字段；无效的脚本文本输入已删除
    m_defaultParams = QJsonObject{{"scriptType", 0}};
    m_params = m_defaultParams;
}

ImageScriptPlugin::~ImageScriptPlugin() {}

bool ImageScriptPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "ImageScriptPlugin initialized";
    return true;
}

void ImageScriptPlugin::shutdown() {
#ifdef DEEPLUX_HAS_OPENCV
    m_resultMat.release();
#endif
    ModuleBase::shutdown();
}

bool ImageScriptPlugin::process(const ImageData& input, ImageData& output) {
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

    // 防御：setParam 等路径不经过 validateParams，运行前再次确认类型合法
    const QJsonValue typeValue = m_params.value("scriptType");
    if (!typeValue.isDouble()) {
        emit errorOccurred(tr("scriptType 类型非法，必须是 0–3 的整数"));
        return false;
    }
    m_scriptType = typeValue.toInt();

    // 阶段 2：执行失败即失败关闭——不得复制输入冒充成功，不得置 script_executed=true
    if (!executeBuiltinOperation(mat, m_resultMat)) {
        emit errorOccurred(tr("内置图像操作执行失败（类型 %1）").arg(m_scriptType));
        return false;
    }

    output.setMat(m_resultMat);
    output.setData("script_type", m_scriptType);
    output.setData("script_executed", true);

    Logger::instance().debug(QString("内置图像操作执行完成, 类型: %1").arg(m_scriptType), "ImageScript");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool ImageScriptPlugin::executeBuiltinOperation(const cv::Mat& input, cv::Mat& output) {
    // 内置图像操作（非脚本解释执行）：类型与 UI 下拉框严格对应
    switch (m_scriptType) {
    case 0: { // 反转
        cv::bitwise_not(input, output);
        break;
    }
    case 1: { // 灰度
        const int channels = input.channels();
        if (channels == 1) {
            output = input.clone(); // 已是单通道灰度
        } else if (channels == 3) {
            cvtColor(input, output, cv::COLOR_BGR2GRAY);
            cvtColor(output, output, cv::COLOR_GRAY2BGR);
        } else if (channels == 4) {
            // 四通道 BGRA：拆分后灰度化 BGR 并保留原 alpha，避免"复制原图"的假成功
            std::vector<cv::Mat> ch;
            cv::split(input, ch);
            cv::Mat bgr, gray;
            cv::merge(std::vector<cv::Mat>{ch[0], ch[1], ch[2]}, bgr);
            cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            cv::merge(std::vector<cv::Mat>{gray, gray, gray, ch[3]}, output);
        } else {
            return false; // 其余通道数不支持，失败关闭
        }
        break;
    }
    case 2: { // 模糊
        cv::blur(input, output, cv::Size(5, 5));
        break;
    }
    case 3: { // 锐化
        cv::Mat kernel = (cv::Mat_<float>(3, 3) << 0, -1, 0, -1, 5, -1, 0, -1, 0);
        cv::filter2D(input, output, input.depth(), kernel);
        break;
    }
    default:
        return false;
    }

    return true;
}

bool ImageScriptPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    // 阶段 2：严格校验 0–3 整数；缺失、类型错误、越界、非整数一律拒绝（失败关闭）。
    // 必须先检查 isDouble()：toDouble() 会把字符串/布尔/null 默转为 0，造成非法值被当作反转操作。
    const QJsonValue value = params.value("scriptType");
    if (value.isUndefined() || value.isNull()) {
        error = tr("缺少 scriptType 参数");
        return false;
    }
    if (!value.isDouble()) {
        error = tr("scriptType 必须是数值类型（0–3 的内置操作类型）");
        return false;
    }
    const double v = value.toDouble();
    if (v < 0 || v > 3 || qFloor(v) != v) {
        error = tr("scriptType 必须是 0–3 的整数（内置操作类型）");
        return false;
    }
    error.clear();
    return true;
}

QWidget* ImageScriptPlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    // 阶段 2：界面为"内置图像操作"，不再提供无效的脚本文本输入
    layout->addWidget(new QLabel(tr("内置图像操作:")));
    QComboBox* typeCombo = new QComboBox();
    typeCombo->addItem(tr("图像反转"), 0);
    typeCombo->addItem(tr("转灰度"), 1);
    typeCombo->addItem(tr("模糊"), 2);
    typeCombo->addItem(tr("锐化"), 3);
    typeCombo->setCurrentIndex(qBound(0, m_params["scriptType"].toInt(), 3));
    layout->addWidget(typeCombo);

    layout->addStretch();

    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, typeCombo](int) { setParam("scriptType", typeCombo->currentData().toInt()); });

    return widget;
}

IModule* ImageScriptPlugin::cloneImpl() const {
    ImageScriptPlugin* clone = new ImageScriptPlugin();
    return clone;
}

} // namespace DeepLux
