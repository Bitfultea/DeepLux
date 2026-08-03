#include "FitCirclePlugin.h"

#include "common/Logger.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <cmath>
#include <random>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

FitCirclePlugin::FitCirclePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"threshold", 2.0}, {"iterations", 100}, {"minRadius", 1.0}, {"maxRadius", 1000.0}};
    m_params = m_defaultParams;
}

FitCirclePlugin::~FitCirclePlugin() {}

bool FitCirclePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FitCirclePlugin initialized";
    return true;
}

void FitCirclePlugin::shutdown() {
#ifdef DEEPLUX_HAS_OPENCV
    m_pointsMat.release();
#endif
    ModuleBase::shutdown();
}

bool FitCirclePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    // 阶段 3: 从 currentParams() 读取参数到局部变量
    QJsonObject params = currentParams();
    const double threshold = params["threshold"].toDouble(2.0);
    const int iterations = params["iterations"].toInt(100);
    const double minRadius = params["minRadius"].toDouble(1.0);
    const double maxRadius = params["maxRadius"].toDouble(1000.0);

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
    bool success = fitCircleRANSAC(points, centerX, centerY, radius, threshold, iterations, minRadius, maxRadius);

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

bool FitCirclePlugin::fitCircleRANSAC(const QVector<QPointF>& points, double& centerX, double& centerY, double& radius,
                                      double threshold, int iterations, double minRadius, double maxRadius) {
#ifdef DEEPLUX_HAS_OPENCV
    if (points.size() < 3) {
        return false;
    }

    auto fitAlgebraic = [](const QVector<QPointF>& fitPoints, double& fittedCenterX, double& fittedCenterY,
                           double& fittedRadius) {
        cv::Mat A(fitPoints.size(), 3, CV_64FC1);
        cv::Mat B(fitPoints.size(), 1, CV_64FC1);
        for (int i = 0; i < fitPoints.size(); ++i) {
            const double x = fitPoints[i].x();
            const double y = fitPoints[i].y();
            A.at<double>(i, 0) = x;
            A.at<double>(i, 1) = y;
            A.at<double>(i, 2) = 1;
            B.at<double>(i, 0) = x * x + y * y;
        }
        cv::Mat C;
        if (!cv::solve(A, B, C, cv::DECOMP_SVD)) {
            return false;
        }
        fittedCenterX = C.at<double>(0, 0) / 2.0;
        fittedCenterY = C.at<double>(1, 0) / 2.0;
        const double radiusSquared = fittedCenterX * fittedCenterX + fittedCenterY * fittedCenterY + C.at<double>(2, 0);
        if (radiusSquared <= 0.0) {
            return false;
        }
        fittedRadius = std::sqrt(radiusSquared);
        return std::isfinite(fittedRadius);
    };

    QVector<QPointF> bestInliers;
    std::mt19937 random(0xC1AC1Eu);
    std::uniform_int_distribution<int> pick(0, points.size() - 1);
    for (int attempt = 0; attempt < iterations; ++attempt) {
        const QPointF first = points[pick(random)];
        const QPointF second = points[pick(random)];
        const QPointF third = points[pick(random)];
        const double determinant = 2.0 * (first.x() * (second.y() - third.y()) + second.x() * (third.y() - first.y()) +
                                          third.x() * (first.y() - second.y()));
        if (std::abs(determinant) < 1e-9) {
            continue;
        }

        const double firstSquared = first.x() * first.x() + first.y() * first.y();
        const double secondSquared = second.x() * second.x() + second.y() * second.y();
        const double thirdSquared = third.x() * third.x() + third.y() * third.y();
        const double candidateCenterX =
            (firstSquared * (second.y() - third.y()) + secondSquared * (third.y() - first.y()) +
             thirdSquared * (first.y() - second.y())) /
            determinant;
        const double candidateCenterY =
            (firstSquared * (third.x() - second.x()) + secondSquared * (first.x() - third.x()) +
             thirdSquared * (second.x() - first.x())) /
            determinant;
        const double candidateRadius = std::hypot(first.x() - candidateCenterX, first.y() - candidateCenterY);
        if (!std::isfinite(candidateRadius)) {
            continue;
        }

        QVector<QPointF> inliers;
        for (const QPointF& point : points) {
            const double residual =
                std::abs(std::hypot(point.x() - candidateCenterX, point.y() - candidateCenterY) - candidateRadius);
            if (residual <= threshold) {
                inliers.append(point);
            }
        }
        if (inliers.size() > bestInliers.size()) {
            bestInliers = inliers;
        }
    }
    if (bestInliers.size() < 3 || !fitAlgebraic(bestInliers, centerX, centerY, radius)) {
        return false;
    }

    if (radius < minRadius || radius > maxRadius) {
        return false;
    }
    return true;
#else
    Q_UNUSED(points);
    Q_UNUSED(centerX);
    Q_UNUSED(centerY);
    Q_UNUSED(radius);
    Q_UNUSED(threshold);
    Q_UNUSED(iterations);
    Q_UNUSED(minRadius);
    Q_UNUSED(maxRadius);
    return false;
#endif
}

bool FitCirclePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();

    if (params["threshold"].toDouble() <= 0.0) {
        error = tr("阈值必须大于0");
        return false;
    }

    if (params["iterations"].toInt() <= 0) {
        error = tr("迭代次数必须大于0");
        return false;
    }

    const double minRadius = params["minRadius"].toDouble();
    const double maxRadius = params["maxRadius"].toDouble();
    if (minRadius <= 0.0) {
        error = tr("最小半径必须大于0");
        return false;
    }

    if (maxRadius <= minRadius) {
        error = tr("最大半径必须大于最小半径");
        return false;
    }

    return true;
}

QWidget* FitCirclePlugin::createConfigWidget() {
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

    connect(minRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { m_params["minRadius"] = value; });

    connect(maxRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { m_params["maxRadius"] = value; });

    connect(iterSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { m_params["iterations"] = value; });

    return widget;
}

IModule* FitCirclePlugin::cloneImpl() const {
    FitCirclePlugin* clone = new FitCirclePlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
