#include "PreProcessing3DPlugin.h"

#include "common/Logger.h"

#include <cmath>
#include <vector>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

PreProcessing3DPlugin::PreProcessing3DPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"heightFilterEnabled", false},
                                  {"heightFilterMin", 0.0},
                                  {"heightFilterMax", 65535.0},
                                  {"fillValue", 0.0},
                                  {"roiCenterX", 0},
                                  {"roiCenterY", 0},
                                  {"roiWidth", 0},
                                  {"roiHeight", 0}};
    m_params = m_defaultParams;
}

PreProcessing3DPlugin::~PreProcessing3DPlugin() {}

bool PreProcessing3DPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "PreProcessing3DPlugin initialized";
    return true;
}

void PreProcessing3DPlugin::shutdown() {
    ModuleBase::shutdown();
}

bool PreProcessing3DPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();
    if (!params[QLatin1String("heightFilterEnabled")].isBool()) {
        error = tr("heightFilterEnabled 必须为布尔");
        return false;
    }
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
    if (!num("heightFilterMin", -1e6, 1e6, false, tr("高度下限必须为[-1e6,1e6]有限数")))
        return false;
    if (!num("heightFilterMax", -1e6, 1e6, false, tr("高度上限必须为[-1e6,1e6]有限数")))
        return false;
    // 交叉约束：区间必须非空（筛选启用与否均校验，避免参数漂移后静默全滤/全过）
    if (params[QLatin1String("heightFilterMin")].toDouble() > params[QLatin1String("heightFilterMax")].toDouble()) {
        error = tr("高度下限不得大于上限");
        return false;
    }
    if (!num("fillValue", -1e6, 1e6, false, tr("填充值必须为[-1e6,1e6]有限数")))
        return false;
    if (!num("roiCenterX", 0, 1e6, true, tr("ROI 中心X必须为[0,1e6]整数")))
        return false;
    if (!num("roiCenterY", 0, 1e6, true, tr("ROI 中心Y必须为[0,1e6]整数")))
        return false;
    if (!num("roiWidth", 0, 1e6, true, tr("ROI 宽度必须为[0,1e6]整数（0=全图）")))
        return false;
    if (!num("roiHeight", 0, 1e6, true, tr("ROI 高度必须为[0,1e6]整数（0=全图）")))
        return false;
    return true;
}

bool PreProcessing3DPlugin::process(const ImageData& input, ImageData& output) {
    output = input;
    // 阶7 批2 复核沿用：只取一次参数快照，验证与执行复用同一快照
    const QJsonObject params = currentParams();
    QString perr;
    if (!doValidateParams(params, perr)) {
        emit errorOccurred(perr);
        return false;
    }
    const bool filterEnabled = params["heightFilterEnabled"].toBool();
    const double hMin = params["heightFilterMin"].toDouble();
    const double hMax = params["heightFilterMax"].toDouble();
    const double fillValue = params["fillValue"].toDouble();
    const int roiCx = params["roiCenterX"].toInt();
    const int roiCy = params["roiCenterY"].toInt();
    const int roiW = params["roiWidth"].toInt();
    const int roiH = params["roiHeight"].toInt();

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat src = input.toMat();
    if (src.empty()) {
        emit errorOccurred(tr("输入图像为空"));
        return false;
    }
    // 通道提取：1 通道直接作为高度；2 通道按旧版 Decompose2 语义取第 2 通道（深度）；
    // 3/4 通道不是高度图载荷，明确失败
    cv::Mat depthSrc;
    if (src.channels() == 1) {
        depthSrc = src;
    } else if (src.channels() == 2) {
        std::vector<cv::Mat> chs;
        cv::split(src, chs);
        depthSrc = chs[1];
    } else {
        emit errorOccurred(tr("高度图须为 1 或 2 通道，收到 %1 通道").arg(src.channels()));
        return false;
    }
    cv::Mat height;
    depthSrc.convertTo(height, CV_64F);

    // ROI（宽或高 <=0 视为全图）
    const bool roiOn = roiW > 0 && roiH > 0;
    cv::Rect roi(0, 0, height.cols, height.rows);
    if (roiOn) {
        roi = cv::Rect(roiCx - roiW / 2, roiCy - roiH / 2, roiW, roiH) & cv::Rect(0, 0, height.cols, height.rows);
        if (roi.area() <= 0) {
            emit errorOccurred(tr("ROI 与图像无交集"));
            return false;
        }
    }

    // 高度筛选：非有限值、区间外与 ROI 外像素统一填充 fillValue；输出 CV_32F 高度图
    cv::Mat out32(height.rows, height.cols, CV_32F, cv::Scalar(static_cast<float>(fillValue)));
    const float fill32 = static_cast<float>(fillValue);
    int valid = 0;
    int filtered = 0;
    double minH = 0.0;
    double maxH = 0.0;
    bool haveStats = false;
    for (int y = 0; y < height.rows; ++y) {
        const double* srcRow = height.ptr<double>(y);
        float* dstRow = out32.ptr<float>(y);
        const bool rowInRoi = !roiOn || (y >= roi.y && y < roi.y + roi.height);
        for (int x = 0; x < height.cols; ++x) {
            const double v = srcRow[x];
            bool keep = rowInRoi && (!roiOn || (x >= roi.x && x < roi.x + roi.width)) && std::isfinite(v);
            if (keep && filterEnabled) {
                keep = (v >= hMin && v <= hMax);
            }
            if (keep) {
                dstRow[x] = static_cast<float>(v);
                ++valid;
                if (!haveStats) {
                    minH = v;
                    maxH = v;
                    haveStats = true;
                } else {
                    minH = std::min(minH, v);
                    maxH = std::max(maxH, v);
                }
            } else {
                dstRow[x] = fill32;
                ++filtered;
            }
        }
    }
    if (valid == 0) {
        emit errorOccurred(tr("预处理后无有效像素（ROI/高度筛选过严或输入全无效）"));
        return false;
    }

    output.setMat(out32);
    output.setData("valid_pixel_count", static_cast<double>(valid));
    output.setData("filtered_pixel_count", static_cast<double>(filtered));
    output.setData("min_height", minH);
    output.setData("max_height", maxH);
    Logger::instance().debug(QString("3D预处理: valid=%1 filtered=%2 值域[%3,%4]")
                                 .arg(valid)
                                 .arg(filtered)
                                 .arg(minH, 0, 'f', 3)
                                 .arg(maxH, 0, 'f', 3),
                             "3DPreProcessing");
    return true;
#else
    Q_UNUSED(filterEnabled);
    Q_UNUSED(hMin);
    Q_UNUSED(hMax);
    Q_UNUSED(fillValue);
    Q_UNUSED(roiCx);
    Q_UNUSED(roiCy);
    Q_UNUSED(roiW);
    Q_UNUSED(roiH);
    emit errorOccurred(tr("3DPreProcessing 需要 OpenCV 支持"));
    return false;
#endif
}

IModule* PreProcessing3DPlugin::cloneImpl() const {
    PreProcessing3DPlugin* clone = new PreProcessing3DPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
