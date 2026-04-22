#include "DisplayDataPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QLineEdit>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

DisplayDataPlugin::DisplayDataPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"displayText", ""},
        {"fontSize", 20},
        {"positionX", 10},
        {"positionY", 30}
    };
    m_params = m_defaultParams;
}

DisplayDataPlugin::~DisplayDataPlugin()
{
}

bool DisplayDataPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "DisplayDataPlugin initialized";
    return true;
}

void DisplayDataPlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_displayMat.release();
#endif
    ModuleBase::shutdown();
}

bool DisplayDataPlugin::process(const ImageData& input, ImageData& output)
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

    m_displayMat = mat.clone();

    // 获取要显示的文本
    QString text = m_params["displayText"].toString();
    if (text.isEmpty()) {
        // 自动生成所有数据项的文本
        QMap<QString, QVariant> allDataMap = input.allData();
        QStringList keys = allDataMap.keys();
        QStringList dataLines;
        for (const QString& key : keys) {
            QVariant value = input.data(key);
            dataLines << QString("%1: %2").arg(key).arg(value.toString());
        }
        text = dataLines.join("\n");
    }

    // 获取位置和字体大小
    int posX = m_params["positionX"].toInt();
    int posY = m_params["positionY"].toInt();
    int fontSize = m_params["fontSize"].toInt();

    // 在图像上绘制文本
    cv::Point point(posX, posY);
    cv::Scalar color(0, 255, 0);  // 绿色
    int thickness = 2;

    // 支持多行文本
    QStringList lines = text.split('\n');
    int lineHeight = fontSize + 5;
    for (int i = 0; i < lines.size(); ++i) {
        cv::putText(m_displayMat, lines[i].toUtf8().constData(),
                    cv::Point(posX, posY + i * lineHeight),
                    cv::FONT_HERSHEY_SIMPLEX, fontSize / 30.0,
                    color, thickness);
    }

    // 设置输出
    output.setMat(m_displayMat);
    output.setData("displayed_text", text);
    output.setData("display_line_count", lines.size());

    Logger::instance().debug(QString("显示数据: %1行").arg(lines.size()), "DisplayData");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool DisplayDataPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* DisplayDataPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("显示文本:")));
    QLineEdit* textEdit = new QLineEdit(m_params["displayText"].toString());
    layout->addWidget(textEdit);

    layout->addWidget(new QLabel(tr("字体大小:")));
    QSpinBox* fontSizeSpin = new QSpinBox();
    fontSizeSpin->setRange(8, 100);
    fontSizeSpin->setValue(m_params["fontSize"].toInt());
    layout->addWidget(fontSizeSpin);

    layout->addWidget(new QLabel(tr("X位置:")));
    QSpinBox* posXSpin = new QSpinBox();
    posXSpin->setRange(0, 2000);
    posXSpin->setValue(m_params["positionX"].toInt());
    layout->addWidget(posXSpin);

    layout->addWidget(new QLabel(tr("Y位置:")));
    QSpinBox* posYSpin = new QSpinBox();
    posYSpin->setRange(0, 2000);
    posYSpin->setValue(m_params["positionY"].toInt());
    layout->addWidget(posYSpin);

    layout->addStretch();

    connect(textEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_params["displayText"] = text;
    });

    connect(fontSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["fontSize"] = value;
    });

    connect(posXSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["positionX"] = value;
    });

    connect(posYSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["positionY"] = value;
    });

    return widget;
}

} // namespace DeepLux