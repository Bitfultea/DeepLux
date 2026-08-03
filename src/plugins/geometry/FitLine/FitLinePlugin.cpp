#include "FitLinePlugin.h"

#include "common/Logger.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <cmath>
#include <random>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

FitLinePlugin::FitLinePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"fitMethod", "RANSAC"}, {"threshold", 3.0}, {"iterations", 100}};
    m_params = m_defaultParams;
}

FitLinePlugin::~FitLinePlugin() {}

bool FitLinePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FitLinePlugin initialized";
    return true;
}

void FitLinePlugin::shutdown() {
#ifdef DEEPLUX_HAS_OPENCV
    m_contour.release();
#endif
    ModuleBase::shutdown();
}

bool FitLinePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    // 阶段 3: 从 currentParams() 读取参数到局部变量
    QJsonObject params = currentParams();
    const QString fitMethod = params["fitMethod"].toString("RANSAC");
    const double threshold = params["threshold"].toDouble(3.0);
    const int iterations = params["iterations"].toInt(100);

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

    if (points.size() < 2) {
        emit errorOccurred(tr("拟合点数量不足，至少需要2个点"));
        return false;
    }

    double rx, ry, px, py;
    bool success = fitLineRANSAC(points, rx, ry, px, py, fitMethod, threshold, iterations);

    if (!success) {
        emit errorOccurred(tr("直线拟合失败"));
        return false;
    }

    // 计算直线上两个点
    double length = 100.0;
    double dx = rx * length;
    double dy = ry * length;

    m_resultRow1 = py - dy;
    m_resultCol1 = px - dx;
    m_resultRow2 = py + dy;
    m_resultCol2 = px + dx;

    // 计算 rho 和 phi
    m_resultRho = fabs(ry * px - rx * py);
    m_resultPhi = atan2(rx, ry) * 180.0 / CV_PI;

    // 计算拟合误差
    double totalError = 0.0;
    for (const QPointF& p : points) {
        double dist = fabs(ry * p.x() - rx * p.y() + m_resultRho) / sqrt(rx * rx + ry * ry);
        totalError += dist;
    }
    m_resultError = totalError / points.size();

    // 设置输出数据
    output.setData("line_row1", m_resultRow1);
    output.setData("line_col1", m_resultCol1);
    output.setData("line_row2", m_resultRow2);
    output.setData("line_col2", m_resultCol2);
    output.setData("line_phi", m_resultPhi);
    output.setData("line_rho", m_resultRho);
    output.setData("line_error", m_resultError);

    QString result = QString("直线: phi=%1°, rho=%2, error=%3")
                         .arg(m_resultPhi, 0, 'f', 2)
                         .arg(m_resultRho, 0, 'f', 2)
                         .arg(m_resultError, 0, 'f', 3);
    Logger::instance().debug(result, "FitLine");

    return true;
}

bool FitLinePlugin::fitLineRANSAC(const QVector<QPointF>& points, double& rx, double& ry, double& px, double& py,
                                  const QString& method, double threshold, int iterations) {
#ifdef DEEPLUX_HAS_OPENCV
    QVector<QPointF> fitPoints = points;
    if (method == "RANSAC") {
        QVector<QPointF> bestInliers;
        std::mt19937 random(0xD33F1u);
        std::uniform_int_distribution<int> pick(0, points.size() - 1);

        for (int attempt = 0; attempt < iterations; ++attempt) {
            const QPointF first = points[pick(random)];
            const QPointF second = points[pick(random)];
            const double dx = second.x() - first.x();
            const double dy = second.y() - first.y();
            const double length = std::hypot(dx, dy);
            if (length < 1e-9) {
                continue;
            }

            QVector<QPointF> inliers;
            for (const QPointF& point : points) {
                const double distance = std::abs(dy * (point.x() - first.x()) - dx * (point.y() - first.y())) / length;
                if (distance <= threshold) {
                    inliers.append(point);
                }
            }
            if (inliers.size() > bestInliers.size()) {
                bestInliers = inliers;
            }
        }
        if (bestInliers.size() < 2) {
            return false;
        }
        fitPoints = bestInliers;
    }

    cv::Mat pointsMat(fitPoints.size(), 1, CV_32FC2);
    for (int i = 0; i < fitPoints.size(); ++i) {
        pointsMat.at<cv::Point2f>(i) = cv::Point2f(fitPoints[i].x(), fitPoints[i].y());
    }
    cv::Vec4f line;
    cv::fitLine(pointsMat, line, cv::DIST_L2, 0, 0.01, 0.01);
    rx = line[0];
    ry = line[1];
    px = line[2];
    py = line[3];
    return true;
#else
    Q_UNUSED(method);
    Q_UNUSED(threshold);
    Q_UNUSED(iterations);
    // 无 OpenCV 时使用最小二乘法
    if (points.size() < 2)
        return false;
    double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    int n = points.size();
    for (const QPointF& p : points) {
        sumX += p.x();
        sumY += p.y();
        sumXY += p.x() * p.y();
        sumX2 += p.x() * p.x();
    }
    double denom = n * sumX2 - sumX * sumX;
    if (fabs(denom) < 1e-10)
        return false;
    double a = (n * sumXY - sumX * sumY) / denom;
    double b = -1.0;
    double c = (sumY - a * sumX) / n;
    double norm = sqrt(a * a + 1);
    rx = a / norm;
    ry = b / norm;
    px = -c * a / (a * a + 1);
    py = -c / (a * a + 1);
    return true;
#endif
}

bool FitLinePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();

    const QString fitMethod = params["fitMethod"].toString("RANSAC");
    if (fitMethod != "RANSAC" && fitMethod != "LS") {
        error = tr("拟合方法无效");
        return false;
    }

    if (params["threshold"].toDouble() <= 0.0) {
        error = tr("阈值必须大于0");
        return false;
    }

    if (params["iterations"].toInt() <= 0) {
        error = tr("迭代次数必须大于0");
        return false;
    }

    return true;
}

QWidget* FitLinePlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("拟合方法:")));
    QComboBox* methodCombo = new QComboBox();
    methodCombo->addItem("RANSAC", "RANSAC");
    methodCombo->addItem("最小二乘", "LS");

    QString method = m_params["fitMethod"].toString();
    int index = methodCombo->findData(method);
    if (index >= 0)
        methodCombo->setCurrentIndex(index);
    layout->addWidget(methodCombo);

    layout->addWidget(new QLabel(tr("阈值 (像素):")));
    QDoubleSpinBox* thresholdSpin = new QDoubleSpinBox();
    thresholdSpin->setRange(0.1, 100.0);
    thresholdSpin->setValue(m_params["threshold"].toDouble());
    thresholdSpin->setSingleStep(0.5);
    layout->addWidget(thresholdSpin);

    layout->addWidget(new QLabel(tr("迭代次数:")));
    QSpinBox* iterSpin = new QSpinBox();
    iterSpin->setRange(10, 1000);
    iterSpin->setValue(m_params["iterations"].toInt());
    layout->addWidget(iterSpin);

    layout->addStretch();

    connect(methodCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this, methodCombo](int) { m_params["fitMethod"] = methodCombo->currentData().toString(); });

    connect(thresholdSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { m_params["threshold"] = value; });

    connect(iterSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int value) { m_params["iterations"] = value; });

    return widget;
}

IModule* FitLinePlugin::cloneImpl() const {
    FitLinePlugin* clone = new FitLinePlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
