#include "ImageScriptPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QComboBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

ImageScriptPlugin::ImageScriptPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"scriptType", 0},
        {"script", ""}
    };
    m_params = m_defaultParams;
}

ImageScriptPlugin::~ImageScriptPlugin()
{
}

bool ImageScriptPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "ImageScriptPlugin initialized";
    return true;
}

void ImageScriptPlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_resultMat.release();
#endif
    ModuleBase::shutdown();
}

bool ImageScriptPlugin::process(const ImageData& input, ImageData& output)
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

    m_script = m_params["script"].toString();
    m_scriptType = m_params["scriptType"].toInt();

    // 执行脚本
    if (!executeScript(m_script, mat, m_resultMat)) {
        // 如果脚本执行失败，至少复制原图
        m_resultMat = mat.clone();
    }

    output.setMat(m_resultMat);
    output.setData("script_type", m_scriptType);
    output.setData("script_executed", true);

    Logger::instance().debug(QString("图像脚本执行完成, 类型: %1").arg(m_scriptType), "ImageScript");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool ImageScriptPlugin::executeScript(const QString& script, const cv::Mat& input, cv::Mat& output)
{
    Q_UNUSED(script);
    Q_UNUSED(input);
    Q_UNUSED(output);

    // 简单的内置脚本来处理常见操作
    // 由于脚本引擎比较复杂，这里提供一个简化版本
    // 实际项目中可以使用 Lua 或 Python 脚本

    switch (m_scriptType) {
    case 0: { // 反转
        cv::bitwise_not(input, output);
        break;
    }
    case 1: { // 灰度
        if (input.channels() == 3) {
            cvtColor(input, output, cv::COLOR_BGR2GRAY);
            cvtColor(output, output, cv::COLOR_GRAY2BGR);
        } else {
            output = input.clone();
        }
        break;
    }
    case 2: { // 模糊
        cv::blur(input, output, cv::Size(5, 5));
        break;
    }
    case 3: { // 锐化
        cv::Mat kernel = (cv::Mat_<float>(3, 3) <<
            0, -1, 0,
            -1, 5, -1,
            0, -1, 0);
        cv::filter2D(input, output, input.depth(), kernel);
        break;
    }
    default:
        output = input.clone();
        return false;
    }

    return true;
}

bool ImageScriptPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* ImageScriptPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("脚本类型:")));
    QComboBox* typeCombo = new QComboBox();
    typeCombo->addItem("图像反转", 0);
    typeCombo->addItem("转灰度", 1);
    typeCombo->addItem("模糊", 2);
    typeCombo->addItem("锐化", 3);
    typeCombo->setCurrentIndex(m_params["scriptType"].toInt());
    layout->addWidget(typeCombo);

    layout->addWidget(new QLabel(tr("脚本内容 (可选):")));
    QTextEdit* scriptEdit = new QTextEdit();
    scriptEdit->setPlainText(m_params["script"].toString());
    scriptEdit->setMaximumHeight(100);
    layout->addWidget(scriptEdit);

    layout->addStretch();

    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, typeCombo](int) {
        m_params["scriptType"] = typeCombo->currentData().toInt();
    });

    connect(scriptEdit, &QTextEdit::textChanged, this, [this, scriptEdit]() {
        m_params["script"] = scriptEdit->toPlainText();
    });

    return widget;
}

IModule* ImageScriptPlugin::cloneImpl() const
{
    ImageScriptPlugin* clone = new ImageScriptPlugin();
    return clone;
}

} // namespace DeepLux
