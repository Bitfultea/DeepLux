#include "MatchingPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

MatchingPlugin::MatchingPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"matchThreshold", 0.8},
        {"maxMatches", 10}
    };
    m_params = m_defaultParams;
}

MatchingPlugin::~MatchingPlugin()
{
}

bool MatchingPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "MatchingPlugin initialized";
    return true;
}

void MatchingPlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_template.release();
    m_resultMat.release();
#endif
    ModuleBase::shutdown();
}

bool MatchingPlugin::process(const ImageData& input, ImageData& output)
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

    // 获取模板
    // 通过QImage传递模板（QImage可以直接存储在QVariant中）
    // 模板可以通过其他插件（如GrabImage）设置到ImageData的元数据中
    QVariant templateQImageVar = input.data("template_qimage");

    if (templateQImageVar.isValid()) {
        // 将QImage转换为cv::Mat
        QImage templateQImage = templateQImageVar.value<QImage>();
        if (!templateQImage.isNull()) {
            m_template = qImageToMat(templateQImage);
        }
    }

    // 如果没有指定模板，使用图像的一部分作为示例
    if (m_template.empty()) {
        // 使用图像中心区域作为默认模板
        int centerX = image.cols / 2;
        int centerY = image.rows / 2;
        int templateSize = qMin(image.cols, image.rows) / 4;
        m_template = image(cv::Rect(centerX - templateSize/2, centerY - templateSize/2, templateSize, templateSize)).clone();
    }

    m_matchThreshold = m_params["matchThreshold"].toDouble();
    m_maxMatches = m_params["maxMatches"].toInt();

    // 执行模板匹配
    m_resultMat = image.clone();
    std::vector<cv::Rect> matches = matchTemplate(image, m_template, m_matchThreshold);

    if (matches.empty()) {
        emit errorOccurred(tr("未找到匹配区域"));
        return false;
    }

    m_matchCount = static_cast<int>(matches.size());

    // 绘制匹配结果
    for (size_t i = 0; i < matches.size() && i < static_cast<size_t>(m_maxMatches); ++i) {
        cv::rectangle(m_resultMat, matches[i], cv::Scalar(0, 255, 0), 2);

        // 在匹配区域旁边显示序号
        QString label = QString("Match %1").arg(i + 1);
        cv::putText(m_resultMat, label.toUtf8().constData(),
                    cv::Point(matches[i].x, matches[i].y - 5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 2);
    }

    output.setMat(m_resultMat);
    output.setData("match_count", m_matchCount);
    output.setData("match_threshold", m_matchThreshold);

    // 返回第一个匹配的位置
    if (!matches.empty()) {
        output.setData("match_x", matches[0].x);
        output.setData("match_y", matches[0].y);
        output.setData("match_width", matches[0].width);
        output.setData("match_height", matches[0].height);
    }

    Logger::instance().debug(QString("模板匹配: 找到%1个匹配, 阈值=%2")
                                .arg(m_matchCount).arg(m_matchThreshold), "Matching");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

std::vector<cv::Rect> MatchingPlugin::matchTemplate(const cv::Mat& image, const cv::Mat& templ, double threshold)
{
    std::vector<cv::Rect> results;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat gray, templGray;

    // 转灰度
    if (image.channels() == 3) {
        cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image;
    }

    if (templ.channels() == 3) {
        cvtColor(templ, templGray, cv::COLOR_BGR2GRAY);
    } else {
        templGray = templ;
    }

    // 模板匹配
    cv::Mat result;
    cv::matchTemplate(gray, templGray, result, cv::TM_CCOEFF_NORMED);

    // 使用minMaxLoc找局部最大值而不是findNonZero
    while (true) {
        double minVal, maxVal;
        cv::Point minLoc, maxLoc;
        cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

        if (maxVal >= threshold) {
            results.push_back(cv::Rect(maxLoc.x, maxLoc.y, templ.cols, templ.rows));

            // 抑制当前匹配区域的响应值，避免重叠
            cv::Rect suppressRegion(
                std::max(0, maxLoc.x - templ.cols / 2),
                std::max(0, maxLoc.y - templ.rows / 2),
                templ.cols * 2,
                templ.rows * 2
            );
            suppressRegion &= cv::Rect(0, 0, result.cols, result.rows);
            cv::Mat(result, suppressRegion) = cv::Scalar(0);
        } else {
            break;
        }

        if (static_cast<int>(results.size()) >= m_maxMatches) {
            break;
        }
    }
#endif

    return results;
}

bool MatchingPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    double threshold = params["matchThreshold"].toDouble();
    if (threshold <= 0 || threshold > 1) {
        error = tr("匹配阈值必须在0到1之间");
        return false;
    }
    return true;
}

QWidget* MatchingPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("匹配阈值 (0-1):")));
    QDoubleSpinBox* threshSpin = new QDoubleSpinBox();
    threshSpin->setRange(0.1, 1.0);
    threshSpin->setSingleStep(0.05);
    threshSpin->setValue(m_params["matchThreshold"].toDouble());
    layout->addWidget(threshSpin);

    layout->addWidget(new QLabel(tr("最大匹配数:")));
    QSpinBox* maxSpin = new QSpinBox();
    maxSpin->setRange(1, 100);
    maxSpin->setValue(m_params["maxMatches"].toInt());
    layout->addWidget(maxSpin);

    layout->addStretch();

    connect(threshSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_params["matchThreshold"] = value;
    });

    connect(maxSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["maxMatches"] = value;
    });

    return widget;
}

IModule* MatchingPlugin::cloneImpl() const
{
    MatchingPlugin* clone = new MatchingPlugin();
    return clone;
}

} // namespace DeepLux
