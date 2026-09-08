#include "MeasureCirclePlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"

#include <algorithm>
#include <cmath>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

namespace {
constexpr double kMaxGray = 255.0;
// 阶7 批2 复核四轮（P1-1）：拟合设计矩阵严重病态/秩亏阈值（相对最小奇异值）
constexpr double kFitConditionEps = 1e-9;

bool fitCircleAlgebraic(const QVector<QPointF>& pts, double& cx, double& cy, double& r) {
    if (pts.size() < 3) {
        return false;
    }
#ifdef DEEPLUX_HAS_OPENCV
    // 阶7 批2 复核四轮（P1-1）：退化防护——DECOMP_SVD 对重复点/共线/秩亏点集
    // 仍会"成功"并返回有限半径，产生虚假测量圆。先 epsilon 去重（唯一点 >= 3），
    // 再显式检查设计矩阵奇异值：最小奇异值相对最大奇异值过小即失败关闭。
    QVector<QPointF> uniq = pts;
    std::sort(uniq.begin(), uniq.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y(); });
    uniq.erase(
        std::unique(uniq.begin(), uniq.end(),
                    [](const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()) < 1e-9; }),
        uniq.end());
    if (uniq.size() < 3) {
        return false; // 重复点集：唯一点不足
    }
    cv::Mat A(uniq.size(), 3, CV_64FC1);
    cv::Mat B(uniq.size(), 1, CV_64FC1);
    for (int i = 0; i < uniq.size(); ++i) {
        const double x = uniq[i].x();
        const double y = uniq[i].y();
        A.at<double>(i, 0) = x;
        A.at<double>(i, 1) = y;
        A.at<double>(i, 2) = 1.0;
        B.at<double>(i, 0) = x * x + y * y;
    }
    const cv::SVD svd(A);
    const double sMax = svd.w.at<double>(0);
    const double sMin = svd.w.at<double>(svd.w.rows - 1);
    if (!(sMax > 0.0) || !(sMin > kFitConditionEps * sMax)) {
        return false; // 秩亏（共线/重复）或严重病态
    }
    cv::Mat C;
    svd.backSubst(B, C);
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
    // 阶7 批2 复核（P1-3）：只取一次参数快照，验证与执行复用同一快照，
    // 避免并发 setParam 使执行用未经验证的新参数。
    const QJsonObject params = currentParams();
    QString perr;
    if (!doValidateParams(params, perr)) {
        emit errorOccurred(perr);
        return false;
    }
    const double cx0 = params["initialCenterX"].toDouble();
    const double cy0 = params["initialCenterY"].toDouble();
    const double r0 = params["initialRadius"].toDouble();
    const double threshold = params["threshold"].toDouble();
    const int measureCount = params["measureCount"].toInt();
    const double searchLength = params["searchLength"].toDouble();
    const double exclusionRadius = params["exclusionRadius"].toDouble();

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat src = input.toMat();
    if (src.empty()) {
        emit errorOccurred(tr("输入图像为空"));
        return false;
    }
    // 阶7 批2 复核（P1-6）：支持 1/3/4 通道；非 8 位归一化到 0..255；2 通道明确失败
    cv::Mat gray;
    if (src.channels() == 2) {
        emit errorOccurred(tr("不支持 2 通道图像"));
        return false;
    }
    if (src.channels() == 1) {
        gray = src;
    } else {
        cv::cvtColor(src, gray, src.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    }
    if (gray.depth() != CV_8U) {
        cv::normalize(gray, gray, 0, 255, cv::NORM_MINMAX, CV_8UC1);
    }

    auto intensityAt = [&gray](double x, double y, double& v) {
        const int xi = cvRound(x);
        const int yi = cvRound(y);
        if (xi < 0 || yi < 0 || xi >= gray.cols || yi >= gray.rows) {
            return false;
        }
        v = static_cast<double>(gray.at<uchar>(yi, xi));
        return true;
    };

    // 径向卡钳：每个角度沿半径搜索梯度峰值 > 阈值（严格大于，零梯度不算边缘）的边缘点
    QVector<QPointF> edgePoints;
    for (int i = 0; i < measureCount; ++i) {
        const double ang = 2.0 * M_PI * i / measureCount;
        const double dx = std::cos(ang);
        const double dy = std::sin(ang);
        double bestR = -1.0;
        double bestGrad = -1.0;
        // 阶7 批2 复核三轮（P2-5）：搜索范围 = [max(1, r0-半长), r0+半长] 闭区间，
        // 符合"搜索半长"语义；合法最小组合（initialRadius=1, searchLength=1）
        // 不再产生空循环（旧 ±1 收缩为 [2,0]）
        const double rLow = qMax(1.0, r0 - searchLength);
        const double rHigh = r0 + searchLength;
        for (double r = rLow; r <= rHigh; r += 1.0) {
            double im = 0.0;
            double ip = 0.0;
            if (!intensityAt(cx0 + (r - 1) * dx, cy0 + (r - 1) * dy, im))
                continue;
            if (!intensityAt(cx0 + (r + 1) * dx, cy0 + (r + 1) * dy, ip))
                continue;
            const double grad = std::abs(ip - im);
            if (grad > threshold && grad > bestGrad) {
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
        // 阶7 批2 复核三轮（P1-4）：启用剔除即失败关闭——剩余点不足或重拟合失败
        // 必须显式报错，不得静默返回未过滤结果（参数看似生效、实际被忽略）
        if (kept.size() < 3) {
            emit errorOccurred(tr("剔除后边缘点不足（%1<3），无法重拟合圆").arg(kept.size()));
            return false;
        }
        if (!fitCircleAlgebraic(kept, cx, cy, radius)) {
            emit errorOccurred(tr("剔除后重拟合圆失败"));
            return false;
        }
        edgePoints = kept;
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
