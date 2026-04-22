#include "FreeformSurfacePlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

FreeformSurfacePlugin::FreeformSurfacePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"samplingInterval", 1.0}
    };
    m_params = m_defaultParams;
}

FreeformSurfacePlugin::~FreeformSurfacePlugin()
{
}

bool FreeformSurfacePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FreeformSurfacePlugin initialized";
    return true;
}

void FreeformSurfacePlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_pointCloud.release();
#endif
    ModuleBase::shutdown();
}

bool FreeformSurfacePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

#ifdef DEEPLUX_HAS_OPENCV
    m_samplingInterval = m_params["samplingInterval"].toDouble();

    // 获取点云数据
    QVariant pointsVar = input.data("point_cloud");
    std::vector<cv::Point3f> points;

    if (pointsVar.isValid()) {
        // 从QVariant解析点云
        QList<QVariant> pointsList = pointsVar.toList();
        for (const QVariant& v : pointsList) {
            QList<QVariant> pt = v.toList();
            if (pt.size() >= 3) {
                points.push_back(cv::Point3f(pt[0].toFloat(), pt[1].toFloat(), pt[2].toFloat()));
            }
        }
    }

    if (points.empty()) {
        emit errorOccurred(tr("未提供点云数据"));
        return false;
    }

    m_pointCount = static_cast<int>(points.size());

    // 计算表面积（简化：使用 bounding box 面积 * 系数）
    if (points.size() >= 3) {
        float minX = points[0].x, maxX = points[0].x;
        float minY = points[0].y, maxY = points[0].y;
        float minZ = points[0].z, maxZ = points[0].z;

        for (const auto& p : points) {
            minX = qMin(minX, p.x); maxX = qMax(maxX, p.x);
            minY = qMin(minY, p.y); maxY = qMax(maxY, p.y);
            minZ = qMin(minZ, p.z); maxZ = qMax(maxZ, p.z);
        }

        float spanX = maxX - minX;
        float spanY = maxY - minY;
        float spanZ = maxZ - minZ;

        // 简化表面积计算
        m_surfaceArea = spanX * spanY * 1.2;  // 系数用于估算曲面面积

        // 计算粗糙度（点到拟合平面的标准差）
        cv::Vec4f planeCoeffs;
        if (fitPlaneToPoints(points, planeCoeffs)) {
            double sumSqDist = 0;
            for (const auto& p : points) {
                double dist = fabs(planeCoeffs[0]*p.x + planeCoeffs[1]*p.y +
                                   planeCoeffs[2]*p.z + planeCoeffs[3]) /
                              sqrt(planeCoeffs[0]*planeCoeffs[0] + planeCoeffs[1]*planeCoeffs[1] +
                                   planeCoeffs[2]*planeCoeffs[2]);
                sumSqDist += dist * dist;
            }
            m_surfaceRoughness = sqrt(sumSqDist / points.size());
        }
    }

    // 设置输出数据
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

bool FreeformSurfacePlugin::fitPlaneToPoints(const std::vector<cv::Point3f>& points, cv::Vec4f& planeCoeffs)
{
#ifdef DEEPLUX_HAS_OPENCV
    if (points.size() < 3) return false;

    // 使用SVD拟合平面 ax + by + cz + d = 0
    cv::Mat A(points.size(), 3, CV_32FC1);
    cv::Mat B(points.size(), 1, CV_32FC1);

    for (size_t i = 0; i < points.size(); ++i) {
        A.at<float>(i, 0) = points[i].x;
        A.at<float>(i, 1) = points[i].y;
        A.at<float>(i, 2) = points[i].z;
        B.at<float>(i, 0) = -1.0f;  // 假设 z = ax + by + d
    }

    cv::Mat X;
    cv::solve(A, B, X, cv::DECOMP_SVD);

    planeCoeffs[0] = X.at<float>(0, 0);
    planeCoeffs[1] = X.at<float>(1, 0);
    planeCoeffs[2] = 1.0f;
    planeCoeffs[3] = X.at<float>(2, 0);

    return true;
#else
    return false;
#endif
}

bool FreeformSurfacePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    double interval = params["samplingInterval"].toDouble();
    if (interval <= 0) {
        error = tr("采样间隔必须大于0");
        return false;
    }
    return true;
}

QWidget* FreeformSurfacePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("采样间隔:")));
    QDoubleSpinBox* intervalSpin = new QDoubleSpinBox();
    intervalSpin->setRange(0.1, 100.0);
    intervalSpin->setValue(m_params["samplingInterval"].toDouble());
    intervalSpin->setSingleStep(0.1);
    layout->addWidget(intervalSpin);

    layout->addStretch();

    connect(intervalSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_params["samplingInterval"] = value;
    });

    return widget;
}

IModule* FreeformSurfacePlugin::cloneImpl() const
{
    FreeformSurfacePlugin* clone = new FreeformSurfacePlugin();
    return clone;
}

} // namespace DeepLux
