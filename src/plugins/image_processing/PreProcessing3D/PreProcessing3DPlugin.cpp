#include "PreProcessing3DPlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"
#include "core/geometry/MeasurementData.h"
#include "core/io/TiffLoader.h"

#include <QVariant>
#include <cmath>
#include <optional>
#include <vector>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

PreProcessing3DPlugin::PreProcessing3DPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"heightFilterEnabled", false},
                                  {"heightFilterMin", 0.0},
                                  {"heightFilterMax", 65535.0},
                                  {"fillValue", -1.0},
                                  {"autoNoData", true},
                                  {"roiCenterX", 0},
                                  {"roiCenterY", 0},
                                  {"roiWidth", 0},
                                  {"roiHeight", 0},
                                  {"scaleX", 1.0},
                                  {"scaleY", 1.0},
                                  {"scaleZ", 1.0},
                                  {"downsampleStep", 1}};
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
    // 阶7 批3复核三轮（P1-1）：自动 NoData 检测必须可关闭（TiffLoader::Config::autoDetectNoData 同语义）
    if (!params[QLatin1String("autoNoData")].isBool()) {
        error = tr("autoNoData 必须为布尔");
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
    for (const char* key : {"scaleX", "scaleY", "scaleZ"}) {
        if (!num(key, 1e-6, 1e9, false, tr("XYZ 缩放必须为[1e-6,1e9]有限正数")))
            return false;
    }
    if (!num("downsampleStep", 1, 1024, true, tr("下采样步长必须为[1,1024]整数")))
        return false;
    if (!num("roiCenterX", 0, 1e6, true, tr("ROI 中心X必须为[0,1e6]整数")))
        return false;
    if (!num("roiCenterY", 0, 1e6, true, tr("ROI 中心Y必须为[0,1e6]整数")))
        return false;
    if (!num("roiWidth", 0, 1e6, true, tr("ROI 宽度必须为[0,1e6]整数（0=全宽）")))
        return false;
    if (!num("roiHeight", 0, 1e6, true, tr("ROI 高度必须为[0,1e6]整数（0=全高）")))
        return false;
    return true;
}

bool PreProcessing3DPlugin::process(const ImageData& input, ImageData& output) {
    output = input;
    const bool hasPointCloud = MeasurementData::pointCloud(input, nullptr).has_value();
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
    const bool autoNoData = params["autoNoData"].toBool();
    const int roiCx = params["roiCenterX"].toInt();
    const int roiCy = params["roiCenterY"].toInt();
    const int roiW = params["roiWidth"].toInt();
    const int roiH = params["roiHeight"].toInt();
    const double scaleX = params["scaleX"].toDouble();
    const double scaleY = params["scaleY"].toDouble();
    const double scaleZ = params["scaleZ"].toDouble();
    const int downsampleStep = params["downsampleStep"].toInt();

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
    // 阶7 批3复核三轮（P1-3）：先消费上游无效值契约——两级预处理串联时，
    // 上游 height_invalid_value 标记的旧无效像素必须统一转换为本次 fillValue，
    // 否则旧填充值会随新契约键的覆盖泄漏到下游拟合
    std::optional<double> carriedQ;
    const QVariant carriedVar = input.data("height_invalid_value");
    // 阶7 批3复核五轮（P2-3）：键存在但类型错误（字符串/布尔/列表等）必须
    // 失败关闭——静默忽略会让错误契约随 output=input 继续向下游传播
    if (carriedVar.isValid() && !portValueMatchesType(carriedVar, DataType::Number)) {
        emit errorOccurred(tr("height_invalid_value 契约类型非法（须为数值）"));
        return false;
    }
    if (carriedVar.isValid()) {
        // 阶7 批3复核三轮（P1-2）：哨兵按源图存储精度量化（与下游插件同一规则）
        const double carried = carriedVar.toDouble();
        double quantized = carried;
        if (depthSrc.depth() == CV_32F) {
            quantized = static_cast<double>(static_cast<float>(carried));
        } else if (depthSrc.depth() != CV_64F) {
            quantized = std::nearbyint(carried);
        }
        // 阶7 批3复核四轮（P1-2）：契约值或其量化结果非有限（NaN/±Inf/
        // 1e300→float 溢出为 Inf）必须失败关闭——DataType::Number 只验证
        // QVariant 类型不验证有限性，v==NaN 恒假会让自动检测被无效契约
        // 关闭后所有有限哨兵全部放行
        if (!std::isfinite(carried) || !std::isfinite(quantized)) {
            emit errorOccurred(tr("携带的 height_invalid_value 契约值非法（非有限或量化后溢出）: %1").arg(carried));
            return false;
        }
        carriedQ = quantized;
    }
    // 阶7 批3复核三轮（P1-1/P2-6）：自动 NoData 检测（TiffLoader 重复极值判据，
    // 原始像素单位启发式）可经 autoNoData 关闭，且在输入已携带无效值契约时让位。
    // 阶7 批3复核五轮（P1-1）：歧义（双侧极值同满足哨兵判据，如两值图）失败
    // 关闭——哨兵语义无法从像素分布推导，猜测可能反向删除全部合法数据
    std::optional<double> noData;
    if (autoNoData && !carriedQ.has_value()) {
        const TiffLoader::NoDataResult detected = TiffLoader::detectNoData(depthSrc);
        if (detected.status == TiffLoader::NoDataStatus::Ambiguous) {
            // 阶7 批3复核六轮（P1-1）：提示只给出真实可行的恢复路径——本插件无
            // invalidValue 参数，高度筛选也无法先于歧义检查生效
            emit errorOccurred(tr("NoData 检测歧义：两侧极值同时满足哨兵判据（如两值图），无法从分布判定哨兵——"
                                  "请提供上游 height_invalid_value 契约，或关闭 autoNoData 后以高度筛选区间显式剔除"));
            return false;
        }
        if (detected.status == TiffLoader::NoDataStatus::InvalidConfig) {
            emit errorOccurred(tr("NoData 检测配置非法（间隙下限非有限或为负）"));
            return false;
        }
        if (detected.status == TiffLoader::NoDataStatus::Found) {
            noData = detected.value;
        }
    }

    cv::Mat height;
    depthSrc.convertTo(height, CV_64F);

    // 任一维度为 0 时，该轴使用全幅；这样 ROI 宽高可在检查器中逐项编辑。
    const bool roiOn = roiW > 0 || roiH > 0;
    const int effectiveRoiWidth = roiW > 0 ? roiW : height.cols;
    const int effectiveRoiHeight = roiH > 0 ? roiH : height.rows;
    cv::Rect roi(roiW > 0 ? roiCx - roiW / 2 : 0, roiH > 0 ? roiCy - roiH / 2 : 0, effectiveRoiWidth,
                 effectiveRoiHeight);
    roi &= cv::Rect(0, 0, height.cols, height.rows);
    if (roiOn) {
        if (roi.area() <= 0) {
            emit errorOccurred(tr("ROI 与图像无交集"));
            return false;
        }
    }

    // ROI 使用原图像素坐标；下采样每隔 N 个原始像素保留一个样本。
    const int outputRows = (height.rows - 1) / downsampleStep + 1;
    const int outputCols = (height.cols - 1) / downsampleStep + 1;
    cv::Mat out32(outputRows, outputCols, CV_32F);
    const float fill32 = static_cast<float>(fillValue * scaleZ);
    if (!std::isfinite(fill32)) {
        emit errorOccurred(tr("填充值乘以 Z 缩放后超出 32 位浮点范围"));
        return false;
    }
    int valid = 0;
    int filtered = 0;
    double minH = 0.0;
    double maxH = 0.0;
    bool haveStats = false;
    bool fillCollides = false; // 阶7 批3复核三轮（P1-2）：存活高度与填充值碰撞检测
    for (int y = 0; y < outputRows; ++y) {
        const int sourceY = y * downsampleStep;
        const double* srcRow = height.ptr<double>(sourceY);
        float* dstRow = out32.ptr<float>(y);
        const bool rowInRoi = !roiOn || (sourceY >= roi.y && sourceY < roi.y + roi.height);
        for (int x = 0; x < outputCols; ++x) {
            const int sourceX = x * downsampleStep;
            const double v = srcRow[sourceX];
            bool keep = rowInRoi && (!roiOn || (sourceX >= roi.x && sourceX < roi.x + roi.width)) && std::isfinite(v);
            // 上游契约标记的旧无效像素统一转为本次 fillValue
            if (keep && carriedQ.has_value() && v == *carriedQ) {
                keep = false;
            }
            // 检测到的 NoData 哨兵与源值逐位一致（float→double 无损），精确比较安全
            if (keep && noData.has_value() && v == *noData) {
                keep = false;
            }
            if (keep && filterEnabled) {
                keep = (v >= hMin && v <= hMax);
            }
            if (keep) {
                dstRow[x] = static_cast<float>(v * scaleZ);
                if (!std::isfinite(dstRow[x])) {
                    emit errorOccurred(tr("高度乘以 Z 缩放后超出 32 位浮点范围"));
                    return false;
                }
                if (dstRow[x] == fill32) {
                    fillCollides = true;
                }
                ++valid;
                if (!haveStats) {
                    minH = dstRow[x];
                    maxH = dstRow[x];
                    haveStats = true;
                } else {
                    minH = std::min(minH, static_cast<double>(dstRow[x]));
                    maxH = std::max(maxH, static_cast<double>(dstRow[x]));
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
    // 阶7 批3复核三轮（P1-2）：有限填充值与存活合法高度相同时，契约无法区分
    // "填充像素"与"合法高度"（如零平面上出现一个 NaN 且 fillValue=0，下游会
    // 删除全部合法零高度）——失败关闭而非写出歧义契约
    if (filtered > 0 && fillCollides) {
        emit errorOccurred(
            tr("填充值 %1 与存活合法高度相同，无法表达无效契约（请改用不碰撞的 fillValue）").arg(fillValue));
        return false;
    }

    output.setMat(out32);
    output.setData("valid_pixel_count", static_cast<double>(valid));
    output.setData("filtered_pixel_count", static_cast<double>(filtered));
    output.setData("min_height", minH);
    output.setData("max_height", maxH);
    // 阶7 批3复核（P1-1）：统一无效值契约——实际填充过像素时记录填充哨兵，
    // 下游（3DPreProcessing/FitPlane/GapMeasure3D）优先采用该携带值而非各自
    // 参数默认值；未填充任何像素时不写键，下游回退自身 invalidValue 参数。
    // 阶7 批3复核三轮（P2-4）：写入 float 量化后的值，与 CV_32F 像素实际存储
    // 逐位一致（非精确可表示的 double fillValue 不再造成键与像素不一致）
    if (filtered > 0) {
        output.setData("height_invalid_value", static_cast<double>(fill32));
    } else if (carriedQ.has_value()) {
        output.setData("height_invalid_value", static_cast<double>(static_cast<float>(*carriedQ * scaleZ)));
    }
    if (hasPointCloud) {
        PointCloudData cloud;
        cloud.points.reserve(static_cast<size_t>(valid));
        const QVariant invalidValue = output.data("height_invalid_value");
        const float invalidHeight = static_cast<float>(invalidValue.toDouble());
        for (int y = 0; y < out32.rows; ++y) {
            const float* row = out32.ptr<float>(y);
            for (int x = 0; x < out32.cols; ++x) {
                const float z = row[x];
                if (!std::isfinite(z) || (invalidValue.isValid() && z == invalidHeight))
                    continue;
                cloud.points.emplace_back(x * downsampleStep * scaleX, y * downsampleStep * scaleY, z);
            }
        }
        MeasurementData::setPointCloud(output, cloud);
    }
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
    Q_UNUSED(autoNoData);
    Q_UNUSED(roiCx);
    Q_UNUSED(roiCy);
    Q_UNUSED(roiW);
    Q_UNUSED(roiH);
    Q_UNUSED(scaleX);
    Q_UNUSED(scaleY);
    Q_UNUSED(scaleZ);
    Q_UNUSED(downsampleStep);
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
