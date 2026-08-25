#include "FreeformSurfacePlugin.h"

#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QVBoxLayout>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

FreeformSurfacePlugin::FreeformSurfacePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"samplingInterval", 1.0}};
    m_params = m_defaultParams;
}

FreeformSurfacePlugin::~FreeformSurfacePlugin() {}

bool FreeformSurfacePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FreeformSurfacePlugin initialized";
    return true;
}

void FreeformSurfacePlugin::shutdown() {
#ifdef DEEPLUX_HAS_OPENCV
    m_pointCloud.release();
#endif
    ModuleBase::shutdown();
}

bool FreeformSurfacePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    // P0-2: 每次执行先重置结果，避免点数不足/拟合失败时沿用上次结果
    m_pointCount = 0;
    m_surfaceArea = 0;
    m_surfaceRoughness = 0;

#ifdef DEEPLUX_HAS_OPENCV
    QJsonObject params = currentParams();
    m_samplingInterval = params["samplingInterval"].toDouble();

    // 获取点云数据 - 优先使用 MeasurementData (PointCloudData)
    std::vector<cv::Point3f> points;

    QString cloudError;
    auto cloudData = MeasurementData::pointCloud(input, &cloudError);
    if (cloudData) {
        for (const auto& pt : cloudData->points) {
            points.push_back(
                cv::Point3f(static_cast<float>(pt.x()), static_cast<float>(pt.y()), static_cast<float>(pt.z())));
        }
    } else {
        QVariant pointsVar = input.data("point_cloud");
        if (pointsVar.isValid()) {
            QList<QVariant> pointsList = pointsVar.toList();
            for (const QVariant& v : pointsList) {
                QList<QVariant> pt = v.toList();
                if (pt.size() >= 3) {
                    points.push_back(cv::Point3f(pt[0].toFloat(), pt[1].toFloat(), pt[2].toFloat()));
                }
            }
        }
    }

    if (points.empty()) {
        emit errorOccurred(tr("未提供点云数据"));
        return false;
    }

    m_pointCount = static_cast<int>(points.size());

    if (points.size() >= 3) {
        // 拟合平面 ax + by + cz + d = 0
        cv::Vec4f planeCoeffs;
        if (fitPlaneToPoints(points, planeCoeffs)) {
            // 粗糙度：点到拟合平面的距离标准差
            double sumSqDist = 0;
            float a = planeCoeffs[0], b = planeCoeffs[1], c = planeCoeffs[2], d = planeCoeffs[3];
            float normLen = sqrt(a * a + b * b + c * c);
            if (normLen < 1e-10f) {
                m_surfaceRoughness = 0;
            } else {
                for (const auto& p : points) {
                    double dist = fabs(a * p.x + b * p.y + c * p.z + d) / normLen;
                    sumSqDist += dist * dist;
                }
                m_surfaceRoughness = sqrt(sumSqDist / points.size());
            }

            // 表面积：将点投影到拟合平面，计算凸包面积
            computeProjectedArea(points, planeCoeffs);
        } else {
            m_surfaceArea = 0;
            m_surfaceRoughness = 0;
        }
    }

    output.setData("point_count", m_pointCount);
    output.setData("surface_area", m_surfaceArea);
    output.setData("surface_roughness", m_surfaceRoughness);

    QString result = QString("自由曲面: 点数=%1, 面积=%2, 粗糙度=%3")
                         .arg(m_pointCount)
                         .arg(m_surfaceArea, 0, 'f', 2)
                         .arg(m_surfaceRoughness, 0, 'f', 4);
    Logger::instance().debug(result, "FreeformSurface");

    return true;
#else
    Q_UNUSED(input);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool FreeformSurfacePlugin::fitPlaneToPoints(const std::vector<cv::Point3f>& points, cv::Vec4f& planeCoeffs) {
#ifdef DEEPLUX_HAS_OPENCV
    if (points.size() < 3)
        return false;

    // 正确的 SVD 平面拟合：对中心化后的点做 SVD，最小奇异值对应的右奇异向量即为法向量
    cv::Point3f centroid(0, 0, 0);
    for (const auto& p : points) {
        centroid.x += p.x;
        centroid.y += p.y;
        centroid.z += p.z;
    }
    float n = static_cast<float>(points.size());
    centroid.x /= n;
    centroid.y /= n;
    centroid.z /= n;

    cv::Mat A(points.size(), 3, CV_32FC1);
    for (size_t i = 0; i < points.size(); ++i) {
        A.at<float>(i, 0) = points[i].x - centroid.x;
        A.at<float>(i, 1) = points[i].y - centroid.y;
        A.at<float>(i, 2) = points[i].z - centroid.z;
    }

    cv::Mat w, u, vt;
    cv::SVD::compute(A, w, u, vt, cv::SVD::MODIFY_A);

    // 最后一行 vt 对应最小奇异值，即平面法向量
    float a = vt.at<float>(2, 0);
    float b = vt.at<float>(2, 1);
    float c = vt.at<float>(2, 2);
    float normLen = sqrt(a * a + b * b + c * c);
    if (normLen < 1e-10f)
        return false;

    planeCoeffs[0] = a / normLen;
    planeCoeffs[1] = b / normLen;
    planeCoeffs[2] = c / normLen;
    planeCoeffs[3] = -(planeCoeffs[0] * centroid.x + planeCoeffs[1] * centroid.y + planeCoeffs[2] * centroid.z);

    return true;
#else
    return false;
#endif
}

void FreeformSurfacePlugin::computeProjectedArea(const std::vector<cv::Point3f>& points, const cv::Vec4f& planeCoeffs) {
#ifdef DEEPLUX_HAS_OPENCV
    // 将点投影到拟合平面，然后在平面局部坐标系中计算 2D 凸包面积
    float a = planeCoeffs[0], b = planeCoeffs[1], c = planeCoeffs[2], d = planeCoeffs[3];
    float normLen = sqrt(a * a + b * b + c * c);
    if (normLen < 1e-10f) {
        m_surfaceArea = 0;
        return;
    }

    cv::Point3f normal(a, b, c);
    // 选择一个不平行于法向量的参考向量来构造平面局部坐标系
    cv::Point3f ref = (fabs(normal.x) < 0.9f) ? cv::Point3f(1, 0, 0) : cv::Point3f(0, 1, 0);
    cv::Point3f uAxis = normal.cross(ref);
    float uLen = sqrt(uAxis.x * uAxis.x + uAxis.y * uAxis.y + uAxis.z * uAxis.z);
    if (uLen < 1e-10f) {
        m_surfaceArea = 0;
        return;
    }
    uAxis.x /= uLen;
    uAxis.y /= uLen;
    uAxis.z /= uLen;

    cv::Point3f vAxis = normal.cross(uAxis);

    std::vector<cv::Point2f> projected2D;
    projected2D.reserve(points.size());
    for (const auto& p : points) {
        float dx = p.x, dy = p.y, dz = p.z;
        float u = dx * uAxis.x + dy * uAxis.y + dz * uAxis.z;
        float v = dx * vAxis.x + dy * vAxis.y + dz * vAxis.z;
        projected2D.emplace_back(u, v);
    }

    std::vector<int> hullIndices;
    cv::convexHull(projected2D, hullIndices);
    if (hullIndices.size() >= 3) {
        // P0-2: 对"真实凸包点"计算面积，而非原始无序点集。
        // 旧实现对 projected2D（随输入顺序）调 contourArea，面积随顺序漂移。
        std::vector<cv::Point2f> hullPoints;
        hullPoints.reserve(hullIndices.size());
        for (int idx : hullIndices) {
            hullPoints.push_back(projected2D[idx]);
        }
        m_surfaceArea = static_cast<float>(cv::contourArea(hullPoints));
    } else {
        // 共线/退化：凸包不足 3 点，面积为 0
        m_surfaceArea = 0;
    }
#else
    m_surfaceArea = 0;
#endif
}

bool FreeformSurfacePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    double interval = params["samplingInterval"].toDouble();
    if (interval <= 0) {
        error = tr("采样间隔必须大于0");
        return false;
    }
    return true;
}

QWidget* FreeformSurfacePlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("采样间隔:")));
    QDoubleSpinBox* intervalSpin = new QDoubleSpinBox();
    intervalSpin->setRange(0.1, 100.0);
    intervalSpin->setValue(m_params["samplingInterval"].toDouble());
    intervalSpin->setSingleStep(0.1);
    layout->addWidget(intervalSpin);

    layout->addStretch();

    connect(intervalSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { m_params["samplingInterval"] = value; });

    return widget;
}

IModule* FreeformSurfacePlugin::cloneImpl() const {
    FreeformSurfacePlugin* clone = new FreeformSurfacePlugin();
    return clone;
}

} // namespace DeepLux
