#include "EdgeDefectDetectionPlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"

#include <QStringList>
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

    // 阶7 批2 复核（P1-4）：沿每个参考点方向卡钳实测边缘半径；搜索以拟合基准圆半径
    // baseR 为中心，偏差 = 实测半径 - baseR（基准圆是唯一的比较基准，不再使用
    // 各参考点自身半径，否则参考边缘的噪声会抵消实测偏差）。
    QVector<double> deviations;
    for (const QPointF& p : refPoints) {
        const double dx = p.x() - cx;
        const double dy = p.y() - cy;
        const double dirR = std::hypot(dx, dy);
        if (dirR <= 0.0) {
            continue;
        }
        const double ux = dx / dirR;
        const double uy = dy / dirR;
        double bestR = -1.0;
        double bestGrad = -1.0;
        const double rStart = qMax(1.0, baseR - searchLength);
        const double rEnd = baseR + searchLength;
        // 阶7 批2 复核（P1-2）：闭区间 [rStart+1, rEnd-1]，searchLength=1 时仍有单点搜索
        for (double r = rStart + 1.0; r <= rEnd - 1.0; r += 1.0) {
            double im = 0.0;
            double ip = 0.0;
            if (!intensityAt(cx + (r - 1) * ux, cy + (r - 1) * uy, im))
                continue;
            if (!intensityAt(cx + (r + 1) * ux, cy + (r + 1) * uy, ip))
                continue;
            const double grad = std::abs(ip - im);
            // 阶7 批2 复核（P1-1）：严格大于，零梯度不算边缘
            if (grad > kEdgeGradient && grad > bestGrad) {
                bestGrad = grad;
                bestR = r;
            }
        }
        if (bestR > 0.0) {
            deviations.append(bestR - baseR);
        }
    }
    if (deviations.isEmpty()) {
        emit errorOccurred(tr("未能提取实际边缘，无法计算偏差"));
        return false;
    }

    // 逐采样偏差统计（全极性，与区域计数分离）
    const int n = deviations.size();
    double maxDev = 0.0;
    double sum = 0.0;
    for (const double d : deviations) {
        maxDev = qMax(maxDev, std::abs(d));
        sum += d;
    }
    const double meanDev = sum / n;
    double var = 0.0;
    for (const double d : deviations) {
        var += (d - meanDev) * (d - meanDev);
    }
    const double stddev = std::sqrt(var / n);

    // 阶7 批2 复核（P1-5）：|偏差|>阈值 的连续采样（按参考点角序、环形相邻）合并为
    // 缺陷区域，区域极性由区域内平均偏差决定；defect_count = isConvex 选定极性的
    // 区域数，has_defect = defect_count > 0（两者恒一致），并输出 defect_regions 列表。
    struct Region {
        int start;  // deviations 索引（环形）
        int length; // 区域内采样数
        double maxAbs;
        double mean;
    };
    QVector<Region> regions;
    int firstOk = -1;
    for (int i = 0; i < n; ++i) {
        if (std::abs(deviations[i]) <= threshold) {
            firstOk = i;
            break;
        }
    }
    if (firstOk < 0) {
        // 全部采样异常：整体为一个环形区域
        regions.append(Region{0, n, maxDev, meanDev});
    } else {
        // 从第一个正常采样开始线性扫描，环形段在首尾自然合并
        for (int i = 0; i < n;) {
            const int idx = (firstOk + i) % n;
            if (std::abs(deviations[idx]) <= threshold) {
                ++i;
                continue;
            }
            Region rg{idx, 0, 0.0, 0.0};
            double rsum = 0.0;
            while (i < n) {
                const int j = (firstOk + i) % n;
                if (std::abs(deviations[j]) <= threshold) {
                    break;
                }
                rsum += deviations[j];
                rg.maxAbs = qMax(rg.maxAbs, std::abs(deviations[j]));
                ++rg.length;
                ++i;
            }
            rg.mean = rsum / rg.length;
            regions.append(rg);
        }
    }

    int convexRegions = 0;
    int concaveRegions = 0;
    QStringList regionDescs;
    for (const Region& rg : regions) {
        const bool convex = rg.mean > 0.0;
        if (convex) {
            ++convexRegions;
        } else {
            ++concaveRegions;
        }
        // 区域描述：起-止索引（环形，起可大于止）:极性:区域内最大|偏差|
        regionDescs << QString("%1-%2:%3:%4")
                           .arg(rg.start)
                           .arg((rg.start + rg.length - 1) % n)
                           .arg(convex ? QLatin1String("convex") : QLatin1String("concave"))
                           .arg(rg.maxAbs, 0, 'f', 2);
    }
    const int defectRegions = isConvex ? convexRegions : concaveRegions;

    output.setData("has_defect", defectRegions > 0);
    output.setData("defect_count", static_cast<double>(defectRegions));
    output.setData("convex_count", static_cast<double>(convexRegions));
    output.setData("concave_count", static_cast<double>(concaveRegions));
    output.setData("max_deviation", maxDev);
    output.setData("mean_deviation", meanDev);
    output.setData("deviation_stddev", stddev);
    output.setData("defect_regions", regionDescs.join(QLatin1Char(';')));
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
