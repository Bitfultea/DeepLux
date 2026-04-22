#include "FitCirclePlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

FitCirclePlugin::FitCirclePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"threshold", 2.0},
        {"iterations", 100},
        {"minRadius", 1.0},
        {"maxRadius", 1000.0}
    };
    m_params = m_defaultParams;
}

FitCirclePlugin::~FitCirclePlugin()
{
}

bool FitCirclePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FitCirclePlugin initialized";
    return true;
}

void FitCirclePlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_pointsMat.release();
#endif
    ModuleBase::shutdown();
}

bool FitCirclePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    // 获取输入点集
    QVariant pointsVar = input.data("fit_points");
    if (!pointsVar.isValid()) {
        emit errorOccurred(tr("未提供拟合点集，请先使用ROI或特征点提取模块"));
        return false;
    }

    QVector<QPointF> points;
    if (pointsVar.canConvert<QVector<QPointF>>()) {
        points = pointsVar.value<QVector<QPointF>>();
    } else {
        QList<QVariant> pointsList = pointsVar.toList();
        for (const QVariant& v : pointsList) {
            QPointF p = v.toPointF();
            points.append(p);
        }
    }

    if (points.size() < 3) {
        emit errorOccurred(tr("拟合点数量不足，至少需要3个点"));
        return false;
    }

    double centerX, centerY, radius;
    bool success = fitCircleRANSAC(points, centerX, centerY, radius);

    if (!success) {
        emit errorOccurred(tr("圆拟合失败"));
        return false;
    }

    m_resultCenterX = centerX;
    m_resultCenterY = centerY;
    m_resultRadius = radius;

    // 计算拟合误差
    double totalError = 0.0;
    for (const QPointF& p : points) {
        double dist = sqrt(pow(p.x() - centerX, 2) + pow(p.y() - centerY, 2));
        totalError += fabs(dist - radius);
    }
    m_resultError = totalError / points.size();

    // 设置输出数据
    output.setData("circle_center_x", m_resultCenterX);
    output.setData("circle_center_y", m_resultCenterY);
    output.setData("circle_radius", m_resultRadius);
    output.setData("circle_error", m_resultError);

    QString result = QString("圆: 中心(%1, %2), 半径=%3, 误差=%4")
                        .arg(m_resultCenterX, 0, 'f', 2)
                        .arg(m_resultCenterY, 0, 'f', 2)
                        .arg(m_resultRadius, 0, 'f', 2)
                        .arg(m_resultError, 0, 'f', 3);
    Logger::instance().debug(result, "FitCircle");

    return true;
}

bool FitCirclePlugin::fitCircleRANSAC(const QVector<QPointF>& points,
                                      double& centerX, double& centerY, double& radius)
{
#ifdef DEEPLUX_HAS_OPENCV
    int n = points.size();
    if (n < 3) return false;

    // 使用代数距离最小二乘法进行圆拟合
    // 圆方程: (x-a)² + (y-b)² = r²
    // 展开: x² - 2ax + a² + y² - 2by + b² = r²
    // 即: x² + y² = 2ax + 2by + (r² - a² - b²)
    // 设 C = [2a, 2b, r² - a² - b²]，则 X * C = x² + y²

    cv::Mat A(n, 3, CV_64FC1);
    cv::Mat B(n, 1, CV_64FC1);

    for (int i = 0; i < n; ++i) {
        double x = points[i].x();
        double y = points[i].y();
        A.at<double>(i, 0) = x;
        A.at<double>(i, 1) = y;
        A.at<double>(i, 2) = 1;
        B.at<double>(i, 0) = x * x + y * y;
    }

    cv::Mat C;
    cv::solve(A, B, C, cv::DECOMP_SVD);

    centerX = C.at<double>(0, 0) / 2.0;
    centerY = C.at<double>(1, 0) / 2.0;
    double c = C.at<double>(2, 0);
    radius = sqrt(centerX * centerX + centerY * centerY + c);

    return radius > m_minRadius && radius < m_maxRadius;
#else
    Q_UNUSED(points);
    Q_UNUSED(centerX);
    Q_UNUSED(centerY);
    Q_UNUSED(radius);
    return false;
#endif
}

bool FitCirclePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* FitCirclePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("最小半径:")));
    QDoubleSpinBox* minRadiusSpin = new QDoubleSpinBox();
    minRadiusSpin->setRange(1.0, 5000.0);
    minRadiusSpin->setValue(m_params["minRadius"].toDouble());
    minRadiusSpin->setSingleStep(1.0);
    layout->addWidget(minRadiusSpin);

    layout->addWidget(new QLabel(tr("最大半径:")));
    QDoubleSpinBox* maxRadiusSpin = new QDoubleSpinBox();
    maxRadiusSpin->setRange(1.0, 10000.0);
    maxRadiusSpin->setValue(m_params["maxRadius"].toDouble());
    maxRadiusSpin->setSingleStep(1.0);
    layout->addWidget(maxRadiusSpin);

    layout->addWidget(new QLabel(tr("迭代次数:")));
    QDoubleSpinBox* iterSpin = new QDoubleSpinBox();
    iterSpin->setRange(10, 1000);
    iterSpin->setValue(m_params["iterations"].toDouble());
    iterSpin->setSingleStep(10);
    layout->addWidget(iterSpin);

    layout->addStretch();

    connect(minRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["minRadius"] = value; });

    connect(maxRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["maxRadius"] = value; });

    connect(iterSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) { m_params["iterations"] = value; });

    return widget;
}

IModule* FitCirclePlugin::cloneImpl() const
{
    FitCirclePlugin* clone = new FitCirclePlugin();
    return clone;
}

} // namespace DeepLux
