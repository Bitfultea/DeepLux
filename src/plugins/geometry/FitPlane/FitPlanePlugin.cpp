#include "FitPlanePlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"
#include "core/io/TiffLoader.h"

#include <QVariant>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

namespace {
// 阶7 批3：拟合门禁常量（沿用批2五轮结论：归一化后奇异值比对平移/尺度不变）
constexpr int kMinPlanePoints = 3;        // 唯一确定平面的最少有效像素数
constexpr double kFitConditionEps = 1e-9; // 设计矩阵秩亏/严重病态阈值（相对最小奇异值）
} // namespace

FitPlanePlugin::FitPlanePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"roiCenterX", 0},     {"roiCenterY", 0},   {"roiLength1", 0},   {"roiLength2", 0},
                                  {"roiAngle", 0.0},     {"pixelSizeX", 1.0}, {"pixelSizeY", 1.0}, {"zScale", 1.0},
                                  {"invalidValue", 0.0}, {"autoNoData", true}};
    m_params = m_defaultParams;
}

FitPlanePlugin::~FitPlanePlugin() {}

bool FitPlanePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FitPlanePlugin initialized";
    return true;
}

void FitPlanePlugin::shutdown() {
    ModuleBase::shutdown();
}

bool FitPlanePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
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
    if (!num("roiCenterX", 0, 1e6, true, tr("ROI 中心X必须为[0,1e6]整数")))
        return false;
    if (!num("roiCenterY", 0, 1e6, true, tr("ROI 中心Y必须为[0,1e6]整数")))
        return false;
    if (!num("roiLength1", 0, 1e6, true, tr("ROI 长边必须为[0,1e6]整数（0=全图）")))
        return false;
    if (!num("roiLength2", 0, 1e6, true, tr("ROI 短边必须为[0,1e6]整数（0=全图）")))
        return false;
    if (!num("roiAngle", -360.0, 360.0, false, tr("ROI 角度必须为[-360,360]有限数")))
        return false;
    // 阶7 批3复核（P2-5）：ROI 两个维度必须同时为 0（全图）或同时 >0，
    // 单维度配置不再静默退回全图
    const double l1v = params[QLatin1String("roiLength1")].toDouble();
    const double l2v = params[QLatin1String("roiLength2")].toDouble();
    if ((l1v > 0.0) != (l2v > 0.0)) {
        error = tr("ROI 长边与短边必须同时为 0（全图）或同时大于 0");
        return false;
    }
    // 像素物理尺寸与 Z 比例必须严格为正（0 会使法向量/物理坐标退化）
    if (!num("pixelSizeX", 1e-6, 1e6, false, tr("X 像素尺寸必须为(0,1e6]有限数")))
        return false;
    if (params[QLatin1String("pixelSizeX")].toDouble() <= 0.0) {
        error = tr("X 像素尺寸必须严格大于 0");
        return false;
    }
    if (!num("pixelSizeY", 1e-6, 1e6, false, tr("Y 像素尺寸必须为(0,1e6]有限数")))
        return false;
    if (params[QLatin1String("pixelSizeY")].toDouble() <= 0.0) {
        error = tr("Y 像素尺寸必须严格大于 0");
        return false;
    }
    if (!num("zScale", 1e-6, 1e6, false, tr("Z 比例必须为(0,1e6]有限数")))
        return false;
    if (params[QLatin1String("zScale")].toDouble() <= 0.0) {
        error = tr("Z 比例必须严格大于 0");
        return false;
    }
    if (!num("invalidValue", -1e9, 1e9, false, tr("无效值必须为[-1e9,1e9]有限数")))
        return false;
    // 阶7 批3复核三轮（P1-1）：自动 NoData 检测必须可关闭
    if (!params[QLatin1String("autoNoData")].isBool()) {
        error = tr("autoNoData 必须为布尔");
        return false;
    }
    return true;
}

bool FitPlanePlugin::process(const ImageData& input, ImageData& output) {
    output = input;
    const QJsonObject params = currentParams();
    QString perr;
    if (!doValidateParams(params, perr)) {
        emit errorOccurred(perr);
        return false;
    }
    const double roiCx = params["roiCenterX"].toDouble();
    const double roiCy = params["roiCenterY"].toDouble();
    const double len1 = params["roiLength1"].toDouble();
    const double len2 = params["roiLength2"].toDouble();
    const double angleDeg = params["roiAngle"].toDouble();
    const double pixelSizeX = params["pixelSizeX"].toDouble();
    const double pixelSizeY = params["pixelSizeY"].toDouble();
    const double zScale = params["zScale"].toDouble();
    const double invalidValue = params["invalidValue"].toDouble();
    const bool autoNoData = params["autoNoData"].toBool();

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat src = input.toMat();
    if (src.empty()) {
        emit errorOccurred(tr("输入图像为空"));
        return false;
    }
    if (src.channels() != 1) {
        emit errorOccurred(tr("高度图须为单通道（%1 通道请先经 3DPreProcessing）").arg(src.channels()));
        return false;
    }
    cv::Mat height;
    src.convertTo(height, CV_64F);

    // ROI：长边或短边 <=0 为全图；否则旋转矩形（roiAngle 度）
    const bool roiOn = len1 > 0.0 && len2 > 0.0;
    const double theta = angleDeg * M_PI / 180.0;
    const double cosT = std::cos(theta);
    const double sinT = std::sin(theta);
    const double h1 = len1 / 2.0;
    const double h2 = len2 / 2.0;
    // 旋转矩形的轴对齐包围盒（与图像求交后作为扫描窗口）
    int x0 = 0;
    int y0 = 0;
    int x1 = height.cols - 1;
    int y1 = height.rows - 1;
    if (roiOn) {
        const double bw = std::abs(h1 * cosT) + std::abs(h2 * sinT);
        const double bh = std::abs(h1 * sinT) + std::abs(h2 * cosT);
        x0 = qMax(0, static_cast<int>(std::floor(roiCx - bw)));
        y0 = qMax(0, static_cast<int>(std::floor(roiCy - bh)));
        x1 = qMin(height.cols - 1, static_cast<int>(std::ceil(roiCx + bw)));
        y1 = qMin(height.rows - 1, static_cast<int>(std::ceil(roiCy + bh)));
        if (x1 < x0 || y1 < y0) {
            emit errorOccurred(tr("ROI 与图像无交集"));
            return false;
        }
    }

    // 阶7 批3复核（P1-1）：无效值契约贯通——输入携带的 height_invalid_value
    // （3DPreProcessing 写出的统一契约）优先于自身参数，直接输入原图时回退参数；
    // 浮点高度图再叠加 TiffLoader 重复极值判据的自动 NoData 检测
    double invalid = invalidValue;
    const QVariant carriedInvalid = input.data("height_invalid_value");
    const bool hasCarried = carriedInvalid.isValid() && portValueMatchesType(carriedInvalid, DataType::Number);
    if (hasCarried) {
        invalid = carriedInvalid.toDouble();
    }
    // 阶7 批3复核（P1-2）：哨兵按源图存储精度量化——-21474.8359 写入 CV_32F 后
    // 实为 -21474.8359375，double 参数精确比较必然失配、像素混入拟合
    double invalidQ = invalid;
    if (src.depth() == CV_32F) {
        invalidQ = static_cast<double>(static_cast<float>(invalid));
    } else if (src.depth() != CV_64F) {
        invalidQ = std::nearbyint(invalid);
    }
    // 阶7 批3复核三轮（P1-1/P2-6）：自动检测可经 autoNoData 关闭，且输入已携带
    // 无效值契约时让位（不重复判定、不误删合法平台，并省去整幅图两遍扫描）
    const std::optional<double> noData =
        (autoNoData && !hasCarried) ? TiffLoader::detectNoDataValue(src) : std::nullopt;
    const auto isInvalid = [&](double v) {
        return !std::isfinite(v) || v == invalidQ || (noData.has_value() && v == *noData);
    };
    const auto inRoi = [&](int x, int y) {
        if (!roiOn) {
            return true;
        }
        const double dx = x - roiCx;
        const double dy = y - roiCy;
        return std::abs(dx * cosT + dy * sinT) <= h1 && std::abs(-dx * sinT + dy * cosT) <= h2;
    };

    // 阶7 批3复核（P2-6）：多遍扫描累计正规方程（O(1) 内存，不再按像素分配
    // Xs/Ys/Zs/A/B），每行检查取消令牌
    // Pass 1：有效点数与质心
    qint64 n = 0;
    double sumX = 0.0;
    double sumY = 0.0;
    for (int y = y0; y <= y1; ++y) {
        if (isCancellationRequested()) {
            return false;
        }
        const double* row = height.ptr<double>(y);
        for (int x = x0; x <= x1; ++x) {
            if (!inRoi(x, y) || isInvalid(row[x])) {
                continue;
            }
            ++n;
            sumX += x * pixelSizeX;
            sumY += y * pixelSizeY;
        }
    }
    if (n < kMinPlanePoints) {
        emit errorOccurred(tr("ROI 内有效像素不足（%1<%2），无法拟合平面").arg(n).arg(kMinPlanePoints));
        return false;
    }
    const double mx = sumX / static_cast<double>(n);
    const double my = sumY / static_cast<double>(n);

    // Pass 2：去质心二阶矩与 z 交叉项（直接累计中心量，无灾难性抵消）
    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;
    double sz = 0.0;
    double sxz = 0.0;
    double syz = 0.0;
    for (int y = y0; y <= y1; ++y) {
        if (isCancellationRequested()) {
            return false;
        }
        const double* row = height.ptr<double>(y);
        for (int x = x0; x <= x1; ++x) {
            if (!inRoi(x, y) || isInvalid(row[x])) {
                continue;
            }
            const double dx = x * pixelSizeX - mx;
            const double dy = y * pixelSizeY - my;
            const double zv = row[x] * zScale;
            sxx += dx * dx;
            sxy += dx * dy;
            syy += dy * dy;
            sz += zv;
            sxz += dx * zv;
            syz += dy * zv;
        }
    }
    const double sum2 = sxx + syy;
    if (!(sum2 > 0.0)) {
        emit errorOccurred(tr("有效像素坐标退化（全部重合），无法拟合平面"));
        return false;
    }

    // 归一化 Gram 门禁：设计矩阵 [u,v,1]（u=dx/s, v=dy/s, s=RMS 展布）去质心后
    // 与常数列正交，σmin² = 2×2 块最小特征值、σmax² = n（迹恒等），门禁等价于
    // λmin > eps²·n，且对平移/尺度不变（与批2复核五轮 SVD 门禁同一判据）
    const double s = std::sqrt(sum2 / static_cast<double>(n));
    const double guu = sxx / (s * s);
    const double guv = sxy / (s * s);
    const double gvv = syy / (s * s);
    const double trG = guu + gvv; // == n
    const double detG = guu * gvv - guv * guv;
    const double disc = std::sqrt(std::max(0.0, trG * trG - 4.0 * detG));
    const double lamMin = (detG > 0.0) ? (2.0 * detG / (trG + disc)) : 0.0; // 稳定形式
    if (!(lamMin > kFitConditionEps * kFitConditionEps * trG)) {
        emit errorOccurred(tr("平面拟合失败：有效像素共线或严重病态"));
        return false;
    }
    // z = α·u + β·v + γ（去质心后 γ = z̄ 精确成立），还原物理系数 z = aX+bY+c
    const double suz = sxz / s;
    const double svz = syz / s;
    const double alpha = (gvv * suz - guv * svz) / detG;
    const double beta = (guu * svz - guv * suz) / detG;
    const double gamma = sz / static_cast<double>(n);
    const double a = alpha / s;
    const double b = beta / s;
    const double c = gamma - a * mx - b * my;

    // 法向量（指向 +Z）与平面距离：-aX - bY + z - c = 0 → n·P + D = 0
    const double nLen = std::sqrt(a * a + b * b + 1.0);
    const double nx = -a / nLen;
    const double ny = -b / nLen;
    const double nz = 1.0 / nLen;
    const double planeD = -c / nLen;

    // Z 向残差统计（mm，最小二乘基准；平面度 = 最大偏差 - 最小偏差）
    // Pass 3：逐像素重算残差（O(1) 内存），每行响应取消
    double maxRes = -std::numeric_limits<double>::infinity();
    double minRes = std::numeric_limits<double>::infinity();
    double sumSqRes = 0.0;
    for (int y = y0; y <= y1; ++y) {
        if (isCancellationRequested()) {
            return false;
        }
        const double* row = height.ptr<double>(y);
        for (int x = x0; x <= x1; ++x) {
            if (!inRoi(x, y) || isInvalid(row[x])) {
                continue;
            }
            const double res = row[x] * zScale - (a * (x * pixelSizeX) + b * (y * pixelSizeY) + c);
            maxRes = qMax(maxRes, res);
            minRes = qMin(minRes, res);
            sumSqRes += res * res;
        }
    }
    const double flatness = maxRes - minRes;
    const double rms = std::sqrt(sumSqRes / static_cast<double>(n));

    // Plane3D 契约输出：拟合平面上 3 个非共线点 [x1,y1,z1, x2,y2,z2, x3,y3,z3]
    // （u=(1,0,a)/|..| 与 v=(0,1,b)/|..| 为平面内两个线性无关方向，步长取点云
    // RMS 展布且不小于 1mm，保证下游 parsePlane3D 的共线防护通过）
    const double zm = gamma; // 质心处拟合值（最小二乘残差均值为 0，等于 z̄）
    const double step = qMax(1.0, s);
    const double uLen = std::sqrt(1.0 + a * a);
    const double vLen = std::sqrt(1.0 + b * b);
    QVariantList plane9;
    plane9 << (mx + step / uLen) << my << (zm + step * a / uLen);
    plane9 << (mx - step / uLen) << my << (zm - step * a / uLen);
    plane9 << mx << (my + step / vLen) << (zm + step * b / vLen);

    output.setData("plane", plane9);
    output.setData("plane_nx", nx);
    output.setData("plane_ny", ny);
    output.setData("plane_nz", nz);
    output.setData("plane_d", planeD);
    output.setData("flatness", flatness);
    output.setData("max_deviation", maxRes);
    output.setData("min_deviation", minRes);
    output.setData("rms", rms);
    output.setData("valid_pixel_count", static_cast<double>(n));
    Logger::instance().debug(QString("平面拟合: n=(%1,%2,%3) D=%4 平面度=%5 RMS=%6 点数=%7")
                                 .arg(nx, 0, 'f', 4)
                                 .arg(ny, 0, 'f', 4)
                                 .arg(nz, 0, 'f', 4)
                                 .arg(planeD, 0, 'f', 3)
                                 .arg(flatness, 0, 'f', 4)
                                 .arg(rms, 0, 'f', 4)
                                 .arg(n),
                             "FitPlane");
    return true;
#else
    Q_UNUSED(roiCx);
    Q_UNUSED(roiCy);
    Q_UNUSED(len1);
    Q_UNUSED(len2);
    Q_UNUSED(angleDeg);
    Q_UNUSED(pixelSizeX);
    Q_UNUSED(pixelSizeY);
    Q_UNUSED(zScale);
    Q_UNUSED(invalidValue);
    Q_UNUSED(autoNoData);
    emit errorOccurred(tr("FitPlane 需要 OpenCV 支持"));
    return false;
#endif
}

IModule* FitPlanePlugin::cloneImpl() const {
    FitPlanePlugin* clone = new FitPlanePlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
