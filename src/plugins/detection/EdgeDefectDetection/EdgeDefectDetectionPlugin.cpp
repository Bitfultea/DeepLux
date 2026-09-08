#include "EdgeDefectDetectionPlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"

#include <QVariant>
#include <algorithm>
#include <cmath>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

namespace {
constexpr double kEdgeGradient = 20.0; // 卡钳梯度阈值（内部常量）
// 阶7 批2 复核三轮（P1-2）：最低成功射线数与覆盖率门禁（失败关闭）
constexpr int kMinEdgeRays = 3;
constexpr double kMinCoverage = 0.5;

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
    // 阶7 批2 复核（P1-3）：只取一次参数快照，验证与执行复用同一快照。
    const QJsonObject params = currentParams();
    QString perr;
    if (!doValidateParams(params, perr)) {
        emit errorOccurred(perr);
        return false;
    }
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

    // 阶7 批2 复核三轮（P1-1）：PointSet2D 契约无顺序保证——先按 atan2 角序排序
    // 再做卡钳与环形区域分析；每条射线记录原始索引，区域输出按原始索引寻址。
    struct Ray {
        int origIndex; // reference_edge 中的原始索引
        double angle;  // 方向角（弧度，atan2）
        double ux;
        double uy;
    };
    QVector<Ray> rays;
    rays.reserve(refPoints.size());
    for (int i = 0; i < refPoints.size(); ++i) {
        const double dx = refPoints[i].x() - cx;
        const double dy = refPoints[i].y() - cy;
        const double dirR = std::hypot(dx, dy);
        if (dirR <= 0.0) {
            continue; // 与圆心重合的点无方向，无法卡钳
        }
        rays.append(Ray{i, std::atan2(dy, dx), dx / dirR, dy / dirR});
    }
    if (rays.size() < kMinEdgeRays) {
        emit errorOccurred(tr("参考边缘有效射线方向不足，无法测量偏差"));
        return false;
    }
    std::sort(rays.begin(), rays.end(), [](const Ray& a, const Ray& b) { return a.angle < b.angle; });

    // 阶7 批2 复核三轮（P1-2）：保留每条射线（含失败项）——失败射线记 NaN 并作为
    // 区域分隔，不再压缩删除（压缩会使失败射线两侧的缺陷伪相邻而被合并）。
    // 阶7 批2 复核二轮（P1-4）：偏差 = 实测半径 - 拟合基准圆半径 baseR。
    const int n = rays.size();
    QVector<double> devs;
    devs.fill(std::nan(""), n);
    int success = 0;
    for (int i = 0; i < n; ++i) {
        const Ray& ray = rays[i];
        double bestR = -1.0;
        double bestGrad = -1.0;
        // 阶7 批2 复核三轮（P2-5）：搜索范围 = [max(1, baseR-半长), baseR+半长] 闭区间，
        // 符合"搜索半长"语义（旧 ±1 收缩使合法最小参数组合产生空循环）
        const double rLow = qMax(1.0, baseR - searchLength);
        const double rHigh = baseR + searchLength;
        for (double r = rLow; r <= rHigh; r += 1.0) {
            double im = 0.0;
            double ip = 0.0;
            if (!intensityAt(cx + (r - 1) * ray.ux, cy + (r - 1) * ray.uy, im))
                continue;
            if (!intensityAt(cx + (r + 1) * ray.ux, cy + (r + 1) * ray.uy, ip))
                continue;
            const double grad = std::abs(ip - im);
            // 阶7 批2 复核二轮（P1-1）：严格大于，零梯度不算边缘
            if (grad > kEdgeGradient && grad > bestGrad) {
                bestGrad = grad;
                bestR = r;
            }
        }
        if (bestR > 0.0) {
            devs[i] = bestR - baseR;
            ++success;
        }
    }
    // 阶7 批2 复核三轮（P1-2）：最低成功数与覆盖率双门禁，失败关闭
    if (success < kMinEdgeRays) {
        emit errorOccurred(tr("有效边缘提取不足（%1/%2 条射线），无法计算偏差").arg(success).arg(n));
        return false;
    }
    if (static_cast<double>(success) < kMinCoverage * n) {
        emit errorOccurred(
            tr("边缘射线覆盖率不足（%1/%2 < %3%），结果不可信").arg(success).arg(n).arg(qRound(kMinCoverage * 100.0)));
        return false;
    }

    // 逐采样偏差统计（仅成功射线，全极性，与区域计数分离）
    double maxDev = 0.0;
    double sum = 0.0;
    for (const double d : devs) {
        if (std::isnan(d)) {
            continue;
        }
        maxDev = qMax(maxDev, std::abs(d));
        sum += d;
    }
    const double meanDev = sum / success;
    double var = 0.0;
    for (const double d : devs) {
        if (std::isnan(d)) {
            continue;
        }
        var += (d - meanDev) * (d - meanDev);
    }
    const double stddev = std::sqrt(var / success);

    // 阶7 批2 复核三轮（P1-3）：分类由偏差符号决定——凸异常(+1)/凹异常(-1)/
    // 正常或失败射线(0，作为分隔)。相邻的凸、凹异常必须切分为两个区域，
    // 不得合并后按平均值归类（正负偏差会互相抵消甚至整体分类错误）。
    QVector<int> cls;
    cls.fill(0, n);
    for (int i = 0; i < n; ++i) {
        if (std::isnan(devs[i])) {
            continue;
        }
        if (devs[i] > threshold) {
            cls[i] = 1;
        } else if (devs[i] < -threshold) {
            cls[i] = -1;
        }
    }

    // 环形区域扫描：同类相邻射线合并为一个区域；类变化（含极性翻转）或分隔结束区域。
    // defect_count = isConvex 选定极性的区域数，has_defect = defect_count > 0（恒一致）。
    struct Region {
        int start;    // 角序起始索引
        int length;   // 区域内射线数
        int polarity; // +1 凸 / -1 凹
        double maxAbs;
        double mean;
    };
    QVector<Region> regions;
    int scanStart = -1;
    for (int i = 0; i < n; ++i) {
        if (cls[i] != 0 && cls[(i + n - 1) % n] != cls[i]) {
            scanStart = i; // 区域起点：非分隔且环形前驱类别不同
            break;
        }
    }
    if (scanStart >= 0) {
        for (int i = 0; i < n;) {
            const int idx = (scanStart + i) % n;
            if (cls[idx] == 0) {
                ++i;
                continue;
            }
            Region rg{idx, 0, cls[idx], 0.0, 0.0};
            double rsum = 0.0;
            while (i < n) {
                const int j = (scanStart + i) % n;
                if (cls[j] != rg.polarity) {
                    break; // 分隔或极性翻转：区域结束
                }
                rsum += devs[j];
                rg.maxAbs = qMax(rg.maxAbs, std::abs(devs[j]));
                ++rg.length;
                ++i;
            }
            rg.mean = rsum / rg.length;
            regions.append(rg);
        }
    } else if (cls[0] != 0) {
        // 整圆同类异常（无任何分隔/翻转）：单一环形区域
        regions.append(Region{0, n, cls[0], maxDev, meanDev});
    }

    int convexRegions = 0;
    int concaveRegions = 0;
    // 阶7 批2 复核三轮（P2-6）：defect_regions 改为结构化 Table 记录
    // （QVariantList<QVariantMap>）：极性/起止原始索引/起止角度/射线数/最大与平均偏差
    QVariantList regionRows;
    for (const Region& rg : regions) {
        const bool convex = rg.polarity > 0;
        if (convex) {
            ++convexRegions;
        } else {
            ++concaveRegions;
        }
        const Ray& first = rays[rg.start];
        const Ray& last = rays[(rg.start + rg.length - 1) % n];
        const auto deg = [](double rad) {
            const double d = rad * 180.0 / M_PI;
            return d < 0.0 ? d + 360.0 : d;
        };
        QVariantMap row;
        row.insert(QStringLiteral("polarity"), convex ? QStringLiteral("convex") : QStringLiteral("concave"));
        row.insert(QStringLiteral("start_index"), first.origIndex);
        row.insert(QStringLiteral("end_index"), last.origIndex);
        row.insert(QStringLiteral("start_angle_deg"), deg(first.angle));
        row.insert(QStringLiteral("end_angle_deg"), deg(last.angle));
        row.insert(QStringLiteral("sample_count"), rg.length);
        row.insert(QStringLiteral("max_deviation"), rg.maxAbs);
        row.insert(QStringLiteral("mean_deviation"), rg.mean);
        regionRows.append(row);
    }
    const int defectRegions = isConvex ? convexRegions : concaveRegions;

    output.setData("has_defect", defectRegions > 0);
    output.setData("defect_count", static_cast<double>(defectRegions));
    output.setData("convex_count", static_cast<double>(convexRegions));
    output.setData("concave_count", static_cast<double>(concaveRegions));
    output.setData("max_deviation", maxDev);
    output.setData("mean_deviation", meanDev);
    output.setData("deviation_stddev", stddev);
    output.setData("defect_regions", regionRows);
    Logger::instance().debug(QString("边缘缺陷: regions=%1 convex=%2 concave=%3 max=%4")
                                 .arg(defectRegions)
                                 .arg(convexRegions)
                                 .arg(concaveRegions)
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
