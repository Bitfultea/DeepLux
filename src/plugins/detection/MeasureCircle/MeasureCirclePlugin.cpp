#include "MeasureCirclePlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"

#include <cmath>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

namespace {
constexpr double kMaxGray = 255.0;

bool fitCircleAlgebraic(const QVector<QPointF>& pts, double& cx, double& cy, double& r) {
    if (pts.size() < 3) {
        return false;
    }
#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat A(pts.size(), 3, CV_64FC1);
    cv::Mat B(pts.size(), 1, CV_64FC1);
    for (int i = 0; i < pts.size(); ++i) {
        const double x = pts[i].x();
        const double y = pts[i].y();
        A.at<double>(i, 0) = x;
        A.at<double>(i, 1) = y;
        A.at<double>(i, 2) = 1.0;
        B.at<double>(i, 0) = x * x + y * y;
    }
    cv::Mat C;
    if (!cv::solve(A, B, C, cv::DECOMP_SVD)) {
        return false;
    }
    cx = C.at<double>(0, 0) / 2.0;
    cy = C.at<double>(1, 0) / 2.0;
    const double rs = cx * cx + cy * cy + C.at<double>(2, 0);
    if (rs <= 0.0) {
        return false;
    }
    r = std::sqrt(rs);
    return std::isfinite(cx) && std::isfinite(cy) && std::isfinite(r);
#else
    Q_UNUSED(pts);
    Q_UNUSED(cx);
    Q_UNUSED(cy);
    Q_UNUSED(r);
    return false;
#endif
}
} // namespace

MeasureCirclePlugin::MeasureCirclePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams =
        QJsonObject{{"initialCenterX", 320.0}, {"initialCenterY", 240.0}, {"initialRadius", 100.0}, {"threshold", 20.0},
                    {"measureCount", 36},      {"searchLength", 20.0},    {"exclusionRadius", 0.0}};
    m_params = m_defaultParams;
}

MeasureCirclePlugin::~MeasureCirclePlugin() {}

bool MeasureCirclePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "MeasureCirclePlugin initialized";
    return true;
}

void MeasureCirclePlugin::shutdown() {
    ModuleBase::shutdown();
}

bool MeasureCirclePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();
    auto num = [&params, &error](const char* key, double lo, double hi, bool integer, const QString& msg) {
        const QJsonValue v = params[QLatin1String(key)];
        if (!v.isDouble()) {
            error = msg;
            return false;
        }
        const double d = v.toDouble();
        if (!std::isfinite(d) || d < lo || d > hi || (integer && std::floor(d) != d)) {
            error = msg;
            return false;
        }
        return true;
    };
    if (!num("initialCenterX", 0.0, 1e6, false, tr("初始圆心X必须为有限非负数")))
        return false;
    if (!num("initialCenterY", 0.0, 1e6, false, tr("初始圆心Y必须为有限非负数")))
        return false;
    if (!num("initialRadius", 1.0, 1e6, false, tr("初始半径必须>=1")))
        return false;
    if (!num("threshold", 0.0, kMaxGray, false, tr("梯度阈值必须为[0,255]")))
        return false;
    if (!num("measureCount", 8.0, 360.0, true, tr("卡钳数必须为[8,360]整数")))
        return false;
    if (!num("searchLength", 1.0, 500.0, false, tr("搜索半长必须为[1,500]")))
        return false;
    if (!num("exclusionRadius", 0.0, 500.0, false, tr("剔除半径必须为[0,500]")))
        return false;
    return true;
}

bool MeasureCirclePlugin::process(const ImageData& input, ImageData& output) {
    output = input;
    QString perr;
    if (!doValidateParams(currentParams(), perr)) {
        emit errorOccurred(perr);
        return false;
    }
    const QJsonObject params = currentParams();
    const double cx0 = params["initialCenterX"].toDouble();
    const double cy0 = params["initialCenterY"].toDouble();
    const double r0 = params["initialRadius"].toDouble();
    const double threshold = params["threshold"].toDouble();
    const int measureCount = params["measureCount"].toInt();
    const double searchLength = params["searchLength"].toDouble();
    const double exclusionRadius = params["exclusionRadius"].toDouble();

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat gray = input.toMat();
    if (gray.empty()) {
        emit errorOccurred(tr("输入图像为空"));
        return false;
    }
    if (gray.channels() > 1) {
        cv::cvtColor(gray, gray, cv::COLOR_BGR2GRAY);
    }
    gray.convertTo(gray, CV_8UC1);

    auto intensityAt = [&gray](double x, double y, double& v) {
        const int xi = cvRound(x);
        const int yi = cvRound(y);
        if (xi < 0 || yi < 0 || xi >= gray.cols || yi >= gray.rows) {
            return false;
        }
        v = static_cast<double>(gray.at<uchar>(yi, xi));
        return true;
    };

    // 径向卡钳：每个角度沿半径搜索梯度峰值 >= 阈值的边缘点
    QVector<QPointF> edgePoints;
    for (int i = 0; i < measureCount; ++i) {
        const double ang = 2.0 * M_PI * i / measureCount;
        const double dx = std::cos(ang);
        const double dy = std::sin(ang);
        double bestR = -1.0;
        double bestGrad = -1.0;
        const double rStart = qMax(1.0, r0 - searchLength);
        const double rEnd = r0 + searchLength;
        for (double r = rStart + 1.0; r < rEnd - 1.0; r += 1.0) {
            double im = 0.0;
            double ip = 0.0;
            if (!intensityAt(cx0 + (r - 1) * dx, cy0 + (r - 1) * dy, im))
                continue;
            if (!intensityAt(cx0 + (r + 1) * dx, cy0 + (r + 1) * dy, ip))
                continue;
            const double grad = std::abs(ip - im);
            if (grad >= threshold && grad > bestGrad) {
                bestGrad = grad;
                bestR = r;
            }
        }
        if (bestR > 0.0) {
            edgePoints.append(QPointF(cx0 + bestR * dx, cy0 + bestR * dy));
        }
    }
    if (edgePoints.size() < 3) {
        emit errorOccurred(tr("卡钳提取边缘点不足，无法拟合圆"));
        return false;
    }

    double cx = 0.0;
    double cy = 0.0;
    double radius = 0.0;
    if (!fitCircleAlgebraic(edgePoints, cx, cy, radius)) {
        emit errorOccurred(tr("圆拟合失败"));
        return false;
    }
    // 剔除半径：剔除残差过大的离群边缘点后重拟合
    if (exclusionRadius > 0.0) {
        QVector<QPointF> kept;
        for (const QPointF& p : edgePoints) {
            if (std::abs(std::hypot(p.x() - cx, p.y() - cy) - radius) <= exclusionRadius) {
                kept.append(p);
            }
        }
        if (kept.size() >= 3 && fitCircleAlgebraic(kept, cx, cy, radius)) {
            edgePoints = kept;
        }
    }

    // 圆度 = 1 - 径向残差标准差/半径（clamp 到 [0,1]）
    double sum = 0.0;
    for (const QPointF& p : edgePoints) {
        sum += std::hypot(p.x() - cx, p.y() - cy) - radius;
    }
    const double meanRes = edgePoints.isEmpty() ? 0.0 : sum / edgePoints.size();
    double var = 0.0;
    for (const QPointF& p : edgePoints) {
        const double d = std::hypot(p.x() - cx, p.y() - cy) - radius - meanRes;
        var += d * d;
    }
    const double stdRes = edgePoints.isEmpty() ? 0.0 : std::sqrt(var / edgePoints.size());
    const double roundness = qBound(0.0, 1.0 - (radius > 0.0 ? stdRes / radius : 0.0), 1.0);

    output.setData("circle_center_x", cx);
    output.setData("circle_center_y", cy);
    output.setData("circle_radius", radius);
    output.setData("circle_diameter", radius * 2.0);
    output.setData("circle_roundness", roundness);
    output.setData("edge_point_count", static_cast<double>(edgePoints.size()));
    Logger::instance().debug(QString("圆测量: 中心(%1,%2) r=%3 圆度=%4 边缘点=%5")
                                 .arg(cx, 0, 'f', 2)
                                 .arg(cy, 0, 'f', 2)
                                 .arg(radius, 0, 'f', 2)
                                 .arg(roundness, 0, 'f', 3)
                                 .arg(edgePoints.size()),
                             "MeasureCircle");
    return true;
#else
    Q_UNUSED(cx0);
    Q_UNUSED(cy0);
    Q_UNUSED(r0);
    Q_UNUSED(threshold);
    Q_UNUSED(measureCount);
    Q_UNUSED(searchLength);
    Q_UNUSED(exclusionRadius);
    emit errorOccurred(tr("MeasureCircle 需要 OpenCV 支持"));
    return false;
#endif
}

IModule* MeasureCirclePlugin::cloneImpl() const {
    MeasureCirclePlugin* clone = new MeasureCirclePlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
