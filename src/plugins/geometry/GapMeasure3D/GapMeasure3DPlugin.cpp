#include "GapMeasure3DPlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"
#include "core/io/TiffLoader.h"

#include <algorithm>
#include <cmath>
#include <optional>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

namespace {
constexpr int kMinProfileSamples = 5; // 截面最少采样数（中心差分需要）
} // namespace

GapMeasure3DPlugin::GapMeasure3DPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"roiCenterX", 320},  {"roiCenterY", 240},      {"roiLength", 100},
                                  {"roiHeight", 5},     {"pixelSizeX", 1.0},      {"zScale", 1.0},
                                  {"smoothSigma", 1.0}, {"medianSize", 0},        {"derivativeThreshold", 0.05},
                                  {"edgeTrim", 2},      {"minPeakDistance", 5},   {"invalidValue", 0.0},
                                  {"offsetMm", 0.0},    {"specUpperLimit", 10.0}, {"measureFailValue", -1.0}};
    m_params = m_defaultParams;
}

GapMeasure3DPlugin::~GapMeasure3DPlugin() {}

bool GapMeasure3DPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "GapMeasure3DPlugin initialized";
    return true;
}

void GapMeasure3DPlugin::shutdown() {
    ModuleBase::shutdown();
}

bool GapMeasure3DPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
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
    // 阶7 批3复核（P2-4）：公开下限与执行需求统一为 5（中心差分+边缘剔除需要）
    if (!num("roiLength", 5, 1e6, true, tr("截面长度必须为[5,1e6]整数")))
        return false;
    if (!num("roiHeight", 1, 1e6, true, tr("行取宽必须为[1,1e6]整数")))
        return false;
    if (!num("pixelSizeX", 1e-6, 1e6, false, tr("X 像素间距必须为(0,1e6]有限数")))
        return false;
    if (params[QLatin1String("pixelSizeX")].toDouble() <= 0.0) {
        error = tr("X 像素间距必须严格大于 0");
        return false;
    }
    if (!num("zScale", 1e-6, 1e6, false, tr("Z 比例必须为(0,1e6]有限数")))
        return false;
    if (params[QLatin1String("zScale")].toDouble() <= 0.0) {
        error = tr("Z 比例必须严格大于 0");
        return false;
    }
    if (!num("smoothSigma", 0.0, 100.0, false, tr("高斯_sigma 必须为[0,100]有限数（0=关闭）")))
        return false;
    if (!num("medianSize", 0, 101, true, tr("中值窗口必须为 0 或 [3,101] 奇数")))
        return false;
    const int medianSize = params[QLatin1String("medianSize")].toInt();
    if (medianSize != 0 && (medianSize < 3 || medianSize % 2 == 0)) {
        error = tr("中值窗口必须为 0（关闭）或 >=3 的奇数");
        return false;
    }
    if (!num("derivativeThreshold", 0.0, 1e6, false, tr("导数阈值必须为[0,1e6]有限数")))
        return false;
    if (!num("edgeTrim", 0, 1000, true, tr("边缘剔除点数必须为[0,1000]整数")))
        return false;
    if (!num("minPeakDistance", 1, 1000, true, tr("峰最小间距必须为[1,1000]整数")))
        return false;
    if (!num("invalidValue", -1e9, 1e9, false, tr("无效高度值必须为[-1e9,1e9]有限数")))
        return false;
    if (!num("offsetMm", -1e6, 1e6, false, tr("偏移距离必须为[-1e6,1e6]有限数")))
        return false;
    if (!num("specUpperLimit", 1e-6, 1e6, false, tr("规格上限必须为(0,1e6]有限数")))
        return false;
    if (params[QLatin1String("specUpperLimit")].toDouble() <= 0.0) {
        error = tr("规格上限必须严格大于 0");
        return false;
    }
    if (!num("measureFailValue", -1e6, 1e6, false, tr("失败输出值必须为[-1e6,1e6]有限数")))
        return false;
    return true;
}

bool GapMeasure3DPlugin::process(const ImageData& input, ImageData& output) {
    output = input;
    const QJsonObject params = currentParams();
    QString perr;
    if (!doValidateParams(params, perr)) {
        emit errorOccurred(perr);
        return false;
    }
    const int roiCx = params["roiCenterX"].toInt();
    const int roiCy = params["roiCenterY"].toInt();
    const int roiLength = params["roiLength"].toInt();
    const int roiRows = params["roiHeight"].toInt();
    const double pixelSizeX = params["pixelSizeX"].toDouble();
    const double zScale = params["zScale"].toDouble();
    const double smoothSigma = params["smoothSigma"].toDouble();
    const int medianSize = params["medianSize"].toInt();
    const double derivThreshold = params["derivativeThreshold"].toDouble();
    const int edgeTrim = params["edgeTrim"].toInt();
    const int minPeakDistance = params["minPeakDistance"].toInt();
    const double invalidValue = params["invalidValue"].toDouble();
    const double offsetMm = params["offsetMm"].toDouble();
    const double specUpperLimit = params["specUpperLimit"].toDouble();
    const double failValue = params["measureFailValue"].toDouble();

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

    // 截面窗口（轴对齐矩形，与图像求交）
    const int xStart = roiCx - roiLength / 2;
    const int yStart = roiCy - roiRows / 2;
    const int xs = qMax(0, xStart);
    const int xe = qMin(height.cols - 1, xStart + roiLength - 1);
    const int ys = qMax(0, yStart);
    const int ye = qMin(height.rows - 1, yStart + roiRows - 1);
    if (xe < xs || ye < ys) {
        emit errorOccurred(tr("截面 ROI 与图像无交集"));
        return false;
    }
    const int m = xe - xs + 1;
    if (m < kMinProfileSamples) {
        emit errorOccurred(tr("截面有效长度不足（%1<%2）").arg(m).arg(kMinProfileSamples));
        return false;
    }

    // 阶7 批3复核（P1-1）：无效值契约贯通——输入携带的 height_invalid_value
    // （3DPreProcessing 统一契约）优先于自身参数；浮点高度图叠加 TiffLoader
    // 重复极值判据的自动 NoData 检测
    double invalid = invalidValue;
    const QVariant carriedInvalid = input.data("height_invalid_value");
    if (carriedInvalid.isValid() && portValueMatchesType(carriedInvalid, DataType::Number)) {
        invalid = carriedInvalid.toDouble();
    }
    // 阶7 批3复核（P1-2）：哨兵按源图存储精度量化，避免 CV_32F 非整数哨兵精确比较失配
    double invalidQ = invalid;
    if (src.depth() == CV_32F) {
        invalidQ = static_cast<double>(static_cast<float>(invalid));
    } else if (src.depth() != CV_64F) {
        invalidQ = std::nearbyint(invalid);
    }
    const std::optional<double> noData = TiffLoader::detectNoDataValue(src);

    // 行均值提取截面轮廓：跳过非有限与无效值；整列无效记 NaN
    const double nan = std::nan("");
    QVector<double> z(m, nan);
    for (int xi = 0; xi < m; ++xi) {
        double sum = 0.0;
        int cnt = 0;
        for (int y = ys; y <= ye; ++y) {
            const double v = height.at<double>(y, xs + xi);
            if (!std::isfinite(v) || v == invalidQ || (noData.has_value() && v == *noData)) {
                continue;
            }
            sum += v;
            ++cnt;
        }
        if (cnt > 0) {
            z[xi] = sum / cnt * zScale;
        }
    }

    // 中值平滑（窗口内有限值的中位数）。阶7 批3复核（P1-3）：无效位置原样保留
    // NaN，不得用邻域补洞——否则会在缺失条带处伪造出不存在的下降/上升沿
    if (medianSize >= 3) {
        const int half = medianSize / 2;
        QVector<double> med(m, nan);
        QVector<double> win;
        for (int i = 0; i < m; ++i) {
            if (!std::isfinite(z[i])) {
                continue;
            }
            win.clear();
            for (int k = qMax(0, i - half); k <= qMin(m - 1, i + half); ++k) {
                if (std::isfinite(z[k])) {
                    win.append(z[k]);
                }
            }
            if (!win.isEmpty()) {
                std::sort(win.begin(), win.end());
                med[i] = win[win.size() / 2];
            }
        }
        z = med;
    }

    // 高斯平滑（仅对有限值卷积并按有效权重归一）。阶7 批3复核（P1-3）：
    // 中心为无效位置的输出保持 NaN（不补洞），缺失条带继续作为区域分隔
    if (smoothSigma > 0.0) {
        const int radius = qMax(1, static_cast<int>(std::ceil(3.0 * smoothSigma)));
        QVector<double> sm(m, nan);
        for (int i = 0; i < m; ++i) {
            if (!std::isfinite(z[i])) {
                continue;
            }
            double wsum = 0.0;
            double acc = 0.0;
            for (int k = qMax(0, i - radius); k <= qMin(m - 1, i + radius); ++k) {
                if (!std::isfinite(z[k])) {
                    continue;
                }
                const double d = k - i;
                const double w = std::exp(-(d * d) / (2.0 * smoothSigma * smoothSigma));
                acc += w * z[k];
                wsum += w;
            }
            if (wsum > 0.0) {
                sm[i] = acc / wsum;
            }
        }
        z = sm;
    }

    // 边缘剔除后的搜索范围
    const int trim = qMin(edgeTrim, (m - 1) / 2);
    const int lo = trim;
    const int hi = m - 1 - trim;
    if (hi - lo < 2) {
        emit errorOccurred(tr("边缘剔除后截面采样不足"));
        return false;
    }

    // 中心差分斜率（mm/mm）：两侧邻域均有限才有效
    QVector<double> slope(m, nan);
    for (int i = lo + 1; i <= hi - 1; ++i) {
        if (std::isfinite(z[i - 1]) && std::isfinite(z[i + 1])) {
            slope[i] = (z[i + 1] - z[i - 1]) / (2.0 * pixelSizeX);
        }
    }

    // 拐角：最陡下降沿（< -阈值）为起点；其右侧 >= minPeakDistance 处
    // 最陡上升沿（> +阈值）为终点
    int fallIdx = -1;
    double fallMin = 0.0;
    for (int i = lo + 1; i <= hi - 1; ++i) {
        if (std::isfinite(slope[i]) && slope[i] < fallMin) {
            fallMin = slope[i];
            fallIdx = i;
        }
    }
    if (fallIdx >= 0 && fallMin >= -derivThreshold) {
        fallIdx = -1; // 未达导数阈值
    }
    int riseIdx = -1;
    double riseMax = 0.0;
    if (fallIdx >= 0) {
        for (int i = fallIdx + minPeakDistance; i <= hi - 1; ++i) {
            if (std::isfinite(slope[i]) && slope[i] > riseMax) {
                riseMax = slope[i];
                riseIdx = i;
            }
        }
        if (riseIdx >= 0 && riseMax <= derivThreshold) {
            riseIdx = -1;
        }
    }

    // 抛物线亚像素细化（顶点偏移 = 0.5·(s0−s2)/(s0−2s1+s2)，钳到 ±1）
    const auto refine = [&slope, m](int i) -> double {
        if (i - 1 < 0 || i + 1 >= m) {
            return i;
        }
        const double s0 = slope[i - 1];
        const double s1 = slope[i];
        const double s2 = slope[i + 1];
        if (!std::isfinite(s0) || !std::isfinite(s2)) {
            return i;
        }
        const double denom = s0 - 2.0 * s1 + s2;
        if (std::abs(denom) < 1e-12) {
            return i;
        }
        const double delta = 0.5 * (s0 - s2) / denom;
        if (!std::isfinite(delta)) {
            return i;
        }
        return i + qBound(-1.0, delta, 1.0);
    };

    bool found = (fallIdx >= 0 && riseIdx >= 0);
    double width = failValue;
    double dz = failValue;
    double cornerDist = failValue;
    if (found) {
        const double xFall = refine(fallIdx);
        const double xRise = refine(riseIdx);
        // 拐角高度取细化位置处的轮廓线性插值（与亚像素 X 口径一致；对称槽两拐角
        // 等高 → dz≈0）。整数索引采样会在模糊沿两侧系统性取到半深差。
        const auto zAt = [&z, m](double xi, int fallbackIdx) {
            const int i0 = static_cast<int>(std::floor(xi));
            const int i1 = i0 + 1;
            if (i0 < 0 || i1 >= m || !std::isfinite(z[i0]) || !std::isfinite(z[i1])) {
                return z[fallbackIdx];
            }
            const double t = xi - i0;
            return z[i0] + t * (z[i1] - z[i0]);
        };
        width = (xRise - xFall) * pixelSizeX;
        dz = std::abs(zAt(xRise, riseIdx) - zAt(xFall, fallIdx));
        if (!(width > 0.0) || !std::isfinite(dz)) {
            found = false;
            width = failValue;
            dz = failValue;
            cornerDist = failValue;
        } else {
            cornerDist = std::hypot(width, dz);
        }
    }
    const bool pass = found && width <= specUpperLimit;

    output.setData("gap_width", width);
    output.setData("gap_offset_width", found ? width + 2.0 * offsetMm : failValue);
    output.setData("corner_dx_mm", width);        // 拐角水平距离（即间隙宽度）
    output.setData("corner_dz_mm", dz);           // 拐角垂直距离
    output.setData("corner_dist_mm", cornerDist); // 拐角欧氏距离
    output.setData("is_pass", pass);
    output.setData("gap_found", found);
    output.setData("gap_algorithm", QStringLiteral("derivative-peak"));
    Logger::instance().debug(QString("3D间隙: found=%1 width=%2 dz=%3 pass=%4")
                                 .arg(found ? "yes" : "no")
                                 .arg(width, 0, 'f', 3)
                                 .arg(dz, 0, 'f', 3)
                                 .arg(pass ? "yes" : "no"),
                             "GapMeasure3D");
    return true;
#else
    Q_UNUSED(roiCx);
    Q_UNUSED(roiCy);
    Q_UNUSED(roiLength);
    Q_UNUSED(roiRows);
    Q_UNUSED(pixelSizeX);
    Q_UNUSED(zScale);
    Q_UNUSED(smoothSigma);
    Q_UNUSED(medianSize);
    Q_UNUSED(derivThreshold);
    Q_UNUSED(edgeTrim);
    Q_UNUSED(minPeakDistance);
    Q_UNUSED(invalidValue);
    Q_UNUSED(offsetMm);
    Q_UNUSED(specUpperLimit);
    Q_UNUSED(failValue);
    emit errorOccurred(tr("GapMeasure3D 需要 OpenCV 支持"));
    return false;
#endif
}

IModule* GapMeasure3DPlugin::cloneImpl() const {
    GapMeasure3DPlugin* clone = new GapMeasure3DPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
