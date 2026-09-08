#include "EdgeDefectDetectionPlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"

#include <cmath>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

namespace {
constexpr double kEdgeGradient = 20.0; // 卡钳梯度阈值（内部常量）

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

EdgeDefectDetectionPlugin::EdgeDefectDetectionPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"threshold", 2.0}, {"searchLength", 10.0}, {"isConvex", true}};
    m_params = m_defaultParams;
}

EdgeDefectDetectionPlugin::~EdgeDefectDetectionPlugin() {}

bool EdgeDefectDetectionPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "EdgeDefectDetectionPlugin initialized";
    return true;
}

void EdgeDefectDetectionPlugin::shutdown() {
    ModuleBase::shutdown();
}

bool EdgeDefectDetectionPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();
    const QJsonValue th = params[QLatin1String("threshold")];
    if (!th.isDouble() || !std::isfinite(th.toDouble()) || th.toDouble() < 0.0 || th.toDouble() > 100.0) {
        error = tr("缺陷阈值必须为[0,100]有限数");
        return false;
    }
    const QJsonValue sl = params[QLatin1String("searchLength")];
    if (!sl.isDouble() || !std::isfinite(sl.toDouble()) || sl.toDouble() < 1.0 || sl.toDouble() > 200.0) {
        error = tr("搜索半长必须为[1,200]有限数");
        return false;
    }
    if (!params[QLatin1String("isConvex")].isBool()) {
        error = tr("isConvex 必须为布尔");
        return false;
    }
    return true;
}

bool EdgeDefectDetectionPlugin::process(const ImageData& input, ImageData& output) {
    output = input;
    QString perr;
    if (!doValidateParams(currentParams(), perr)) {
        emit errorOccurred(perr);
        return false;
    }
    const QJsonObject params = currentParams();
    const double threshold = params["threshold"].toDouble();
    const double searchLength = params["searchLength"].toDouble();
    const bool isConvex = params["isConvex"].toBool();

    const QVariant refVar = input.data("reference_edge");
    if (!refVar.isValid() || !portValueMatchesType(refVar, DataType::PointSet2D)) {
        emit errorOccurred(tr("参考边缘格式非法（须为 PointSet2D）"));
        return false;
    }
    QVector<QPointF> refPoints;
    if (refVar.canConvert<QVector<QPointF>>()) {
        refPoints = refVar.value<QVector<QPointF>>();
    } else {
        for (const QVariant& v : refVar.toList()) {
            if (v.type() == QVariant::PointF) {
                refPoints.append(v.toPointF());
            } else if (v.type() == QVariant::List) {
                const QVariantList l = v.toList();
                if (l.size() == 2 && l[0].canConvert<double>() && l[1].canConvert<double>()) {
                    refPoints.append(QPointF(l[0].toDouble(), l[1].toDouble()));
                }
            }
        }
    }
    if (refPoints.size() < 3) {
        emit errorOccurred(tr("参考边缘点不足，无法拟合基准"));
        return false;
    }

    double cx = 0.0;
    double cy = 0.0;
    double baseR = 0.0;
    if (!fitCircleAlgebraic(refPoints, cx, cy, baseR)) {
        emit errorOccurred(tr("参考边缘基准圆拟合失败"));
        return false;
    }

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

    // 沿每个参考点方向卡钳实测边缘半径，偏差 = 实测半径 - 参考半径
    QVector<double> deviations;
    for (const QPointF& p : refPoints) {
        const double dx = p.x() - cx;
        const double dy = p.y() - cy;
        const double refR = std::hypot(dx, dy);
        if (refR <= 0.0) {
            continue;
        }
        const double ux = dx / refR;
        const double uy = dy / refR;
        double bestR = -1.0;
        double bestGrad = -1.0;
        const double rStart = qMax(1.0, refR - searchLength);
        const double rEnd = refR + searchLength;
        for (double r = rStart + 1.0; r < rEnd - 1.0; r += 1.0) {
            double im = 0.0;
            double ip = 0.0;
            if (!intensityAt(cx + (r - 1) * ux, cy + (r - 1) * uy, im))
                continue;
            if (!intensityAt(cx + (r + 1) * ux, cy + (r + 1) * uy, ip))
                continue;
            const double grad = std::abs(ip - im);
            if (grad >= kEdgeGradient && grad > bestGrad) {
                bestGrad = grad;
                bestR = r;
            }
        }
        if (bestR > 0.0) {
            deviations.append(bestR - refR);
        }
    }
    if (deviations.isEmpty()) {
        emit errorOccurred(tr("未能提取实际边缘，无法计算偏差"));
        return false;
    }

    int convex = 0;
    int concave = 0;
    int defects = 0;
    double maxDev = 0.0;
    double sum = 0.0;
    for (const double d : deviations) {
        if (d > threshold)
            ++convex;
        else if (d < -threshold)
            ++concave;
        if (std::abs(d) > threshold)
            ++defects;
        maxDev = qMax(maxDev, std::abs(d));
        sum += d;
    }
    const double meanDev = sum / deviations.size();
    double var = 0.0;
    for (const double d : deviations) {
        var += (d - meanDev) * (d - meanDev);
    }
    const double stddev = std::sqrt(var / deviations.size());
    const int relevant = isConvex ? convex : concave;

    output.setData("has_defect", relevant > 0);
    output.setData("defect_count", static_cast<double>(defects));
    output.setData("convex_count", static_cast<double>(convex));
    output.setData("concave_count", static_cast<double>(concave));
    output.setData("max_deviation", maxDev);
    output.setData("mean_deviation", meanDev);
    output.setData("deviation_stddev", stddev);
    Logger::instance().debug(QString("边缘缺陷: defects=%1 convex=%2 concave=%3 max=%4")
                                 .arg(defects)
                                 .arg(convex)
                                 .arg(concave)
                                 .arg(maxDev, 0, 'f', 2),
                             "EdgeDefectDetection");
    return true;
#else
    Q_UNUSED(threshold);
    Q_UNUSED(searchLength);
    Q_UNUSED(isConvex);
    emit errorOccurred(tr("EdgeDefectDetection 需要 OpenCV 支持"));
    return false;
#endif
}

IModule* EdgeDefectDetectionPlugin::cloneImpl() const {
    EdgeDefectDetectionPlugin* clone = new EdgeDefectDetectionPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
