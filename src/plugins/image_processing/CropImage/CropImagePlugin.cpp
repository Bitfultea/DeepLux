#include "CropImagePlugin.h"

#include "common/Logger.h"

#include <QJsonArray>
#include <QVariant>
#include <cmath>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

namespace {
constexpr int kMaxRects = 64;  // 单次执行的旋转矩形数上限（防参数数组失控）
constexpr int kRectFields = 5; // [cx, cy, l1, l2, deg]
} // namespace

CropImagePlugin::CropImagePlugin(QObject* parent) : ModuleBase(parent) {
    QJsonArray rects;
    rects.append(QJsonArray{320.0, 240.0, 100.0, 100.0, 0.0}); // [[cx,cy,l1,l2,deg]]（单层嵌套）
    m_defaultParams = QJsonObject{{"rectangles", rects}, {"outputFirstAsImage", true}};
    m_params = m_defaultParams;
}

CropImagePlugin::~CropImagePlugin() {}

bool CropImagePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "CropImagePlugin initialized";
    return true;
}

void CropImagePlugin::shutdown() {
    ModuleBase::shutdown();
}

bool CropImagePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();
    if (!params[QLatin1String("outputFirstAsImage")].isBool()) {
        error = tr("outputFirstAsImage 必须为布尔");
        return false;
    }
    const QJsonValue rectsVal = params[QLatin1String("rectangles")];
    if (!rectsVal.isArray()) {
        error = tr("rectangles 必须为数组 [[cx,cy,l1,l2,deg],...]");
        return false;
    }
    const QJsonArray rects = rectsVal.toArray();
    if (rects.isEmpty() || rects.size() > kMaxRects) {
        error = tr("rectangles 数量必须为[1,%1]").arg(kMaxRects);
        return false;
    }
    for (int i = 0; i < rects.size(); ++i) {
        if (!rects[i].isArray()) {
            error = tr("rectangles[%1] 必须为 5 元数值数组").arg(i);
            return false;
        }
        const QJsonArray r = rects[i].toArray();
        if (r.size() != kRectFields) {
            error = tr("rectangles[%1] 必须恰含 5 个数值 [cx,cy,l1,l2,deg]").arg(i);
            return false;
        }
        for (int k = 0; k < kRectFields; ++k) {
            if (!r[k].isDouble() || !std::isfinite(r[k].toDouble())) {
                error = tr("rectangles[%1][%2] 必须为有限数").arg(i).arg(k);
                return false;
            }
        }
        const double cx = r[0].toDouble();
        const double cy = r[1].toDouble();
        const double l1 = r[2].toDouble();
        const double l2 = r[3].toDouble();
        const double deg = r[4].toDouble();
        if (cx < -1e6 || cx > 1e6 || cy < -1e6 || cy > 1e6) {
            error = tr("rectangles[%1] 中心必须为[-1e6,1e6]有限数").arg(i);
            return false;
        }
        if (!(l1 > 0.0) || l1 > 1e6 || !(l2 > 0.0) || l2 > 1e6) {
            error = tr("rectangles[%1] 边长必须为(0,1e6]").arg(i);
            return false;
        }
        if (deg < -360.0 || deg > 360.0) {
            error = tr("rectangles[%1] 角度必须为[-360,360]").arg(i);
            return false;
        }
    }
    return true;
}

bool CropImagePlugin::process(const ImageData& input, ImageData& output) {
    output = input;
    const QJsonObject params = currentParams();
    QString perr;
    if (!doValidateParams(params, perr)) {
        emit errorOccurred(perr);
        return false;
    }
    const QJsonArray rects = params["rectangles"].toArray();
    const bool outputFirst = params["outputFirstAsImage"].toBool();

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat src = input.toMat();
    if (src.empty()) {
        emit errorOccurred(tr("输入图像为空"));
        return false;
    }

    // 逐旋转矩形：解析包围盒（|ux|+|vx| 半宽）→ 四舍五入半开区间 → 与图像求交
    QVariantList cropRows;
    cv::Mat firstCrop;
    int cropCount = 0;
    for (int i = 0; i < rects.size(); ++i) {
        const QJsonArray r = rects[i].toArray();
        const double cx = r[0].toDouble();
        const double cy = r[1].toDouble();
        const double l1 = r[2].toDouble();
        const double l2 = r[3].toDouble();
        const double deg = r[4].toDouble();
        const double theta = deg * M_PI / 180.0;
        const double halfUx = std::abs(std::cos(theta)) * l1 / 2.0;
        const double halfUy = std::abs(std::sin(theta)) * l1 / 2.0;
        const double halfVx = std::abs(std::sin(theta)) * l2 / 2.0;
        const double halfVy = std::abs(std::cos(theta)) * l2 / 2.0;
        const int x0 = qMax(0, qRound(cx - halfUx - halfVx));
        const int y0 = qMax(0, qRound(cy - halfUy - halfVy));
        const int x1 = qMin(src.cols, qRound(cx + halfUx + halfVx));
        const int y1 = qMin(src.rows, qRound(cy + halfUy + halfVy));
        const int w = x1 - x0;
        const int h = y1 - y0;
        const bool valid = (w > 0 && h > 0);

        QVariantMap row;
        row.insert(QStringLiteral("index"), i);
        row.insert(QStringLiteral("valid"), valid);
        row.insert(QStringLiteral("center_x"), cx);
        row.insert(QStringLiteral("center_y"), cy);
        row.insert(QStringLiteral("length1"), l1);
        row.insert(QStringLiteral("length2"), l2);
        row.insert(QStringLiteral("angle_deg"), deg);
        row.insert(QStringLiteral("crop_x"), valid ? x0 : 0);
        row.insert(QStringLiteral("crop_y"), valid ? y0 : 0);
        row.insert(QStringLiteral("crop_width"), valid ? w : 0);
        row.insert(QStringLiteral("crop_height"), valid ? h : 0);
        if (valid) {
            const cv::Mat crop = src(cv::Rect(x0, y0, w, h)).clone();
            if (firstCrop.empty()) {
                firstCrop = crop;
            }
            row.insert(QStringLiteral("image"), QVariant::fromValue(ImageData(crop)));
            ++cropCount;
        }
        cropRows.append(row);
    }
    if (cropCount == 0) {
        emit errorOccurred(tr("所有裁剪矩形与图像无交集（%1 个矩形全部无效）").arg(rects.size()));
        return false;
    }

    // 旧版 IsOutputCropImage 对应物：为真时 image 端口输出首个有效裁剪图，
    // 供下游测量模块直接消费；否则原样透传输入
    if (outputFirst) {
        output.setMat(firstCrop);
    }
    output.setData("crop_count", static_cast<double>(cropCount));
    output.setData("crop_images", cropRows);
    Logger::instance().debug(QString("图像裁剪: %1/%2 个矩形有效，首裁剪 %3x%4")
                                 .arg(cropCount)
                                 .arg(rects.size())
                                 .arg(firstCrop.cols)
                                 .arg(firstCrop.rows),
                             "CropImage");
    return true;
#else
    Q_UNUSED(rects);
    Q_UNUSED(outputFirst);
    emit errorOccurred(tr("CropImage 需要 OpenCV 支持"));
    return false;
#endif
}

IModule* CropImagePlugin::cloneImpl() const {
    CropImagePlugin* clone = new CropImagePlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
