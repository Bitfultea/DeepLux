#include "FitEllipsePlugin.h"

#include "common/Logger.h"

#include <cmath>
#include <random>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

namespace {

// 阶7 批1 复核三轮（P1-2）：RANSAC 采样硬上限，防长时间无响应。
constexpr int kMaxRansacAttempts = 1000;

// 阶7 批1 复核：严格参数解析——校验 JSON 类型/整数性/有限性/范围，
// 验证与执行共用同一份快照，杜绝 toDouble()/toInt() 宽松转换假成功。
bool parseParamsStrict(const QJsonObject& params, FitEllipsePlugin::ParsedParams& out, QString& error) {
    error.clear();
    auto num = [&params, &error](const char* key, double& value, bool integer, double lo, double hiExclusive,
                                 const QString& msg) {
        const QJsonValue v = params[QLatin1String(key)];
        if (!v.isDouble()) { // JSON 整数亦为 isDouble==true；字符串/bool/缺失均拒绝
            error = msg;
            return false;
        }
        const double d = v.toDouble();
        if (!std::isfinite(d)) {
            error = msg;
            return false;
        }
        if (integer && std::floor(d) != d) {
            error = msg;
            return false;
        }
        if (d < lo || d >= hiExclusive) {
            error = msg;
            return false;
        }
        value = d;
        return true;
    };
    if (!num("threshold", out.threshold, false, 0.0, 1e9, QObject::tr("阈值必须为有限非负数")))
        return false;
    if (!num("iterations", out.iterations, true, 1.0, static_cast<double>(kMaxRansacAttempts) + 1.0,
             QObject::tr("迭代次数必须为[1,1000]的整数")))
        return false;
    if (!num("minAxis", out.minAxis, false, 1e-9, 1e9, QObject::tr("最小半轴必须为有限正数")))
        return false;
    if (!num("maxAxis", out.maxAxis, false, 1e-9, 1e9, QObject::tr("最大半轴必须为有限正数")))
        return false;
    if (out.maxAxis <= out.minAxis) {
        error = QObject::tr("最大半轴必须大于最小半轴");
        return false;
    }
    return true;
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

#ifdef DEEPLUX_HAS_OPENCV
bool fitEllipseCv(const QVector<QPointF>& points, FitEllipsePlugin::EllipseResult& result) {
    if (points.size() < 5) {
        return false;
    }
    std::vector<cv::Point2f> cvPoints;
    cvPoints.reserve(points.size());
    for (const QPointF& p : points) {
        cvPoints.emplace_back(static_cast<float>(p.x()), static_cast<float>(p.y()));
    }
    // 真实算法：OpenCV 直接最小二乘椭圆拟合（Fitzgibbon 约束圆锥）
    const cv::RotatedRect rr = cv::fitEllipse(cvPoints);
    const double axisA = rr.size.width * 0.5;
    const double axisB = rr.size.height * 0.5;
    if (axisA <= 0.0 || axisB <= 0.0 || !std::isfinite(axisA) || !std::isfinite(axisB)) {
        return false;
    }
    result.majorR = std::max(axisA, axisB);
    result.minorR = std::min(axisA, axisB);
    // fitEllipse 的 angle 为 width 轴方向；长轴方向按长短轴归正
    double phi = (axisA >= axisB) ? rr.angle : rr.angle + 90.0;
    // 阶7 批1 复核：phi 契约归一化到 [0,180) 度
    phi = std::fmod(phi, 180.0);
    if (phi < 0.0) {
        phi += 180.0;
    }
    result.phi = phi;
    result.centerX = rr.center.x;
    result.centerY = rr.center.y;
    return result.majorR > 0.0;
}
#endif

} // namespace

FitEllipsePlugin::FitEllipsePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"threshold", 2.0}, {"iterations", 100}, {"minAxis", 0.5}, {"maxAxis", 5000.0}};
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

bool FitEllipsePlugin::fitEllipseRobust(const QVector<QPointF>& points, double threshold, int iterations,
                                        EllipseResult& result) const {
#ifdef DEEPLUX_HAS_OPENCV
    // 阶7 批1 复核三轮（P0-1）：预先去重，唯一点不足 5 直接失败关闭，
    // 避免采样循环因唯一坐标不足而无限循环。
    QVector<QPointF> uniq;
    for (const QPointF& p : points) {
        bool dup = false;
        for (const QPointF& u : uniq) {
            if (std::abs(u.x() - p.x()) < 1e-9 && std::abs(u.y() - p.y()) < 1e-9) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            uniq.append(p);
        }
    }
    if (uniq.size() < 5) {
        return false;
    }
    if (threshold <= 0.0) {
        return fitEllipseCv(uniq, result);
    }
    // RANSAC 稳健估计：5 点最小采样拟合椭圆，按阈值统计内点，取最优内点集重拟合。
    // 采样次数硬上限 kMaxRansacAttempts 并检查取消令牌，防长时间无响应。
    const int attempts = qBound(1, iterations, kMaxRansacAttempts);
    std::mt19937 rng(0xE11F5Eu);
    std::uniform_int_distribution<int> pick(0, uniq.size() - 1);
    QVector<QPointF> bestInliers;
    double bestError = 0.0;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (isCancellationRequested()) {
            return false;
        }
        QVector<QPointF> sample;
        while (sample.size() < 5) {
            const QPointF candidate = uniq[pick(rng)];
            bool dup = false;
            for (const QPointF& s : sample) {
                if (std::abs(s.x() - candidate.x()) < 1e-9 && std::abs(s.y() - candidate.y()) < 1e-9) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                sample.append(candidate);
            }
        }
        EllipseResult candidate;
        if (!fitEllipseCv(sample, candidate)) {
            continue;
        }
        QVector<QPointF> inliers;
        double err = 0.0;
        for (const QPointF& p : uniq) {
            const double r = pointEllipseResidual(p, candidate);
            if (r <= threshold) {
                inliers.append(p);
                err += r;
            }
        }
        if (inliers.size() > bestInliers.size() ||
            (inliers.size() == bestInliers.size() && inliers.size() >= 5 && err < bestError)) {
            bestInliers = inliers;
            bestError = err;
        }
    }
    if (bestInliers.size() < 5) {
        return false; // 失败关闭，不返回被离群点拉偏的结果
    }
    if (!fitEllipseCv(bestInliers, result)) {
        return false;
    }
    double total = 0.0;
    for (const QPointF& p : bestInliers) {
        total += pointEllipseResidual(p, result);
    }
    result.error = total / bestInliers.size();
    return true;
#else
    Q_UNUSED(points);
    Q_UNUSED(threshold);
    Q_UNUSED(iterations);
    Q_UNUSED(result);
    return false;
#endif
}

bool FitEllipsePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    // 阶7 批1 复核：执行前用与验证同一份严格解析快照，非法参数失败关闭。
    ParsedParams parsed;
    QString perr;
    if (!parseParamsStrict(currentParams(), parsed, perr)) {
        emit errorOccurred(perr);
        return false;
    }

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
            if (!v.canConvert<QPointF>()) {
                emit errorOccurred(tr("拟合点集包含非法点"));
                return false;
            }
            points.append(v.toPointF());
        }
    }
    if (points.size() < 5) {
        emit errorOccurred(tr("拟合点数量不足，至少需要5个点"));
        return false;
    }

    EllipseResult result;
    if (!fitEllipseRobust(points, parsed.threshold, static_cast<int>(parsed.iterations), result)) {
        emit errorOccurred(tr("椭圆拟合失败（内点不足或退化）"));
        return false;
    }
    if (result.majorR < parsed.minAxis || result.majorR > parsed.maxAxis || result.minorR < parsed.minAxis) {
        emit errorOccurred(tr("拟合半轴超出参数范围"));
        return false;
    }
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
    ParsedParams parsed;
    return parseParamsStrict(params, parsed, error);
}

IModule* FitEllipsePlugin::cloneImpl() const {
    FitEllipsePlugin* clone = new FitEllipsePlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
