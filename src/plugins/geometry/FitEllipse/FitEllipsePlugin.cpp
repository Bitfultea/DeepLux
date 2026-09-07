#include "FitEllipsePlugin.h"

#include "common/Logger.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <cmath>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

FitEllipsePlugin::FitEllipsePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"threshold", 2.0}, {"iterations", 3}, {"minAxis", 1.0}, {"maxAxis", 5000.0}};
    m_params = m_defaultParams;
}

FitEllipsePlugin::~FitEllipsePlugin() {}

bool FitEllipsePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FitEllipsePlugin initialized";
    return true;
}

void FitEllipsePlugin::shutdown() {
    ModuleBase::shutdown();
}

bool FitEllipsePlugin::fitEllipseLeastSquares(const QVector<QPointF>& points, EllipseResult& result) const {
#ifdef DEEPLUX_HAS_OPENCV
    if (points.size() < 5) {
        return false;
    }
    std::vector<cv::Point2f> cvPoints;
    cvPoints.reserve(points.size());
    for (const QPointF& p : points) {
        cvPoints.emplace_back(static_cast<float>(p.x()), static_cast<float>(p.y()));
    }
    // 真实算法：OpenCV 直接最小二乘椭圆拟合（Fitzgibbon 约束圆锥拟合）
    cv::RotatedRect rr = cv::fitEllipse(cvPoints);
    const double axisA = rr.size.width * 0.5;
    const double axisB = rr.size.height * 0.5;
    if (axisA <= 0.0 || axisB <= 0.0 || !std::isfinite(axisA) || !std::isfinite(axisB)) {
        return false;
    }
    result.majorR = std::max(axisA, axisB);
    result.minorR = std::min(axisA, axisB);
    // fitEllipse 的 angle 为长轴方向；width 对应 angle 方向
    result.phi = (axisA >= axisB) ? rr.angle : rr.angle + 90.0;
    result.centerX = rr.center.x;
    result.centerY = rr.center.y;
    if (result.majorR <= 0.0) {
        return false;
    }
    return true;
#else
    Q_UNUSED(points);
    Q_UNUSED(result);
    return false;
#endif
}

double pointEllipseResidual(const QPointF& p, const FitEllipsePlugin::EllipseResult& e) {
    // 旋转回椭圆坐标系后的归一化半径偏差（几何近似残差）
    const double rad = e.phi * M_PI / 180.0;
    const double cosA = std::cos(rad);
    const double sinA = std::sin(rad);
    const double dx = p.x() - e.centerX;
    const double dy = p.y() - e.centerY;
    const double xr = dx * cosA + dy * sinA;
    const double yr = -dx * sinA + dy * cosA;
    const double a = e.majorR;
    const double b = e.minorR > 0.0 ? e.minorR : 1e-6;
    const double rNorm = std::sqrt((xr * xr) / (a * a) + (yr * yr) / (b * b));
    return std::abs(rNorm - 1.0) * a;
}

bool FitEllipsePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    QJsonObject params = currentParams();
    const double threshold = params["threshold"].toDouble(2.0);
    const int iterations = params["iterations"].toInt(3);
    const double minAxis = params["minAxis"].toDouble(1.0);
    const double maxAxis = params["maxAxis"].toDouble(5000.0);

    QVariant pointsVar = input.data("fit_points");
    if (!pointsVar.isValid()) {
        emit errorOccurred(tr("未提供拟合点集，请先使用边缘/轮廓提取模块"));
        return false;
    }
    QVector<QPointF> points;
    if (pointsVar.canConvert<QVector<QPointF>>()) {
        points = pointsVar.value<QVector<QPointF>>();
    } else {
        const QList<QVariant> pointsList = pointsVar.toList();
        for (const QVariant& v : pointsList) {
            points.append(v.toPointF());
        }
    }
    if (points.size() < 5) {
        emit errorOccurred(tr("拟合点数量不足，至少需要5个点"));
        return false;
    }

    // 迭代离群剔除：拟合→按阈值剔除→重拟合
    QVector<QPointF> inliers = points;
    EllipseResult result;
    bool ok = false;
    for (int iter = 0; iter < qMax(1, iterations); ++iter) {
        EllipseResult candidate;
        if (!fitEllipseLeastSquares(inliers, candidate)) {
            break;
        }
        ok = true;
        result = candidate;
        if (threshold <= 0.0) {
            break;
        }
        QVector<QPointF> next;
        for (const QPointF& p : inliers) {
            if (pointEllipseResidual(p, candidate) <= threshold) {
                next.append(p);
            }
        }
        if (next.size() < 5 || next.size() == inliers.size()) {
            inliers = next.size() < 5 ? inliers : next;
            break;
        }
        inliers = next;
    }
    if (!ok) {
        emit errorOccurred(tr("椭圆拟合失败"));
        return false;
    }
    if (result.majorR < minAxis || result.majorR > maxAxis || result.minorR < minAxis) {
        emit errorOccurred(tr("拟合半轴超出参数范围"));
        return false;
    }

    // 平均几何残差作为拟合误差
    double total = 0.0;
    for (const QPointF& p : inliers) {
        total += pointEllipseResidual(p, result);
    }
    result.error = inliers.isEmpty() ? 0.0 : total / inliers.size();
    m_result = result;

    const double ellipticity = result.majorR > 0.0 ? result.minorR / result.majorR : 0.0;
    output.setData("ellipse_center_x", result.centerX);
    output.setData("ellipse_center_y", result.centerY);
    output.setData("ellipse_phi", result.phi);
    output.setData("ellipse_major_r", result.majorR);
    output.setData("ellipse_minor_r", result.minorR);
    output.setData("ellipse_ellipticity", ellipticity);
    output.setData("ellipse_error", result.error);

    Logger::instance().debug(QString("椭圆: 中心(%1,%2) phi=%3 a=%4 b=%5 e=%6")
                                 .arg(result.centerX, 0, 'f', 2)
                                 .arg(result.centerY, 0, 'f', 2)
                                 .arg(result.phi, 0, 'f', 2)
                                 .arg(result.majorR, 0, 'f', 2)
                                 .arg(result.minorR, 0, 'f', 2)
                                 .arg(result.error, 0, 'f', 3),
                             "FitEllipse");
    return true;
}

bool FitEllipsePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();
    if (params["threshold"].toDouble() < 0.0) {
        error = tr("离群阈值不能为负");
        return false;
    }
    if (params["iterations"].toInt() < 1) {
        error = tr("剔除迭代次数必须>=1");
        return false;
    }
    const double minAxis = params["minAxis"].toDouble();
    const double maxAxis = params["maxAxis"].toDouble();
    if (minAxis <= 0.0) {
        error = tr("最小半轴必须大于0");
        return false;
    }
    if (maxAxis <= minAxis) {
        error = tr("最大半轴必须大于最小半轴");
        return false;
    }
    return true;
}

QWidget* FitEllipsePlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("离群阈值:")));
    QDoubleSpinBox* thresholdSpin = new QDoubleSpinBox();
    thresholdSpin->setRange(0.0, 100.0);
    thresholdSpin->setValue(m_params["threshold"].toDouble());
    thresholdSpin->setSingleStep(0.5);
    layout->addWidget(thresholdSpin);

    layout->addWidget(new QLabel(tr("最小半轴:")));
    QDoubleSpinBox* minSpin = new QDoubleSpinBox();
    minSpin->setRange(0.1, 5000.0);
    minSpin->setValue(m_params["minAxis"].toDouble());
    minSpin->setSingleStep(0.5);
    layout->addWidget(minSpin);

    layout->addWidget(new QLabel(tr("最大半轴:")));
    QDoubleSpinBox* maxSpin = new QDoubleSpinBox();
    maxSpin->setRange(1.0, 20000.0);
    maxSpin->setValue(m_params["maxAxis"].toDouble());
    maxSpin->setSingleStep(0.5);
    layout->addWidget(maxSpin);

    layout->addStretch();

    connect(thresholdSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { m_params["threshold"] = value; });
    connect(minSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { m_params["minAxis"] = value; });
    connect(maxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { m_params["maxAxis"] = value; });

    return widget;
}

IModule* FitEllipsePlugin::cloneImpl() const {
    FitEllipsePlugin* clone = new FitEllipsePlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
