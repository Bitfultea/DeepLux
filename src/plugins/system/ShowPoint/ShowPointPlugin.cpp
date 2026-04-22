#include "ShowPointPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

ShowPointPlugin::ShowPointPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"markerSize", 5},
        {"colorR", 0},
        {"colorG", 255},
        {"colorB", 0}
    };
    m_params = m_defaultParams;
}

ShowPointPlugin::~ShowPointPlugin()
{
}

bool ShowPointPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "ShowPointPlugin initialized";
    return true;
}

void ShowPointPlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_resultMat.release();
#endif
    ModuleBase::shutdown();
}

bool ShowPointPlugin::process(const ImageData& input, ImageData& output)
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

    m_resultMat = image.clone();

    // 获取点坐标
    QVariant pointVar = input.data("point");
    if (!pointVar.isValid()) {
        emit errorOccurred(tr("未提供点坐标"));
        return false;
    }

    double x = 0, y = 0;
    if (pointVar.canConvert<QPointF>()) {
        QPointF p = pointVar.toPointF();
        x = p.x();
        y = p.y();
    } else {
        QList<QVariant> list = pointVar.toList();
        if (list.size() >= 2) {
            x = list[0].toDouble();
            y = list[1].toDouble();
        }
    }

    m_markerSize = m_params["markerSize"].toInt();
    m_markerColorR = m_params["colorR"].toInt();
    m_markerColorG = m_params["colorG"].toInt();
    m_markerColorB = m_params["colorB"].toInt();

    // 绘制点
    cv::Point2i pt(static_cast<int>(x), static_cast<int>(y));
    cv::circle(m_resultMat, pt, m_markerSize,
               cv::Scalar(m_markerColorB, m_markerColorG, m_markerColorR), -1);

    // 显示坐标文本
    QString label = QString("(%1, %2)").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1);
    cv::putText(m_resultMat, label.toUtf8().constData(),
                cv::Point(pt.x + 10, pt.y - 10),
                cv::FONT_HERSHEY_SIMPLEX, 0.5,
                cv::Scalar(m_markerColorB, m_markerColorG, m_markerColorR), 2);

    output.setMat(m_resultMat);
    output.setData("point_x", x);
    output.setData("point_y", y);
    output.setData("point_displayed", true);

    Logger::instance().debug(QString("显示点: (%1, %2)").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1), "ShowPoint");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool ShowPointPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* ShowPointPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("点大小:")));
    QSpinBox* sizeSpin = new QSpinBox();
    sizeSpin->setRange(1, 50);
    sizeSpin->setValue(m_params["markerSize"].toInt());
    layout->addWidget(sizeSpin);

    layout->addWidget(new QLabel(tr("颜色R:")));
    QSpinBox* rSpin = new QSpinBox();
    rSpin->setRange(0, 255);
    rSpin->setValue(m_params["colorR"].toInt());
    layout->addWidget(rSpin);

    layout->addWidget(new QLabel(tr("颜色G:")));
    QSpinBox* gSpin = new QSpinBox();
    gSpin->setRange(0, 255);
    gSpin->setValue(m_params["colorG"].toInt());
    layout->addWidget(gSpin);

    layout->addWidget(new QLabel(tr("颜色B:")));
    QSpinBox* bSpin = new QSpinBox();
    bSpin->setRange(0, 255);
    bSpin->setValue(m_params["colorB"].toInt());
    layout->addWidget(bSpin);

    layout->addStretch();

    connect(sizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["markerSize"] = value;
    });

    connect(rSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["colorR"] = value;
    });

    connect(gSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["colorG"] = value;
    });

    connect(bSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_params["colorB"] = value;
    });

    return widget;
}

IModule* ShowPointPlugin::cloneImpl() const
{
    ShowPointPlugin* clone = new ShowPointPlugin();
    return clone;
}

} // namespace DeepLux
