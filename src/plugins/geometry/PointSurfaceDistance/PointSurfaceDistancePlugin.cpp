#include "PointSurfaceDistancePlugin.h"
#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"
#include <QVBoxLayout>
#include <QLabel>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

PointSurfaceDistancePlugin::PointSurfaceDistancePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
    };
    m_params = m_defaultParams;
}

PointSurfaceDistancePlugin::~PointSurfaceDistancePlugin()
{
}

bool PointSurfaceDistancePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "PointSurfaceDistancePlugin initialized";
    return true;
}

void PointSurfaceDistancePlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool PointSurfaceDistancePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    // 获取点和平面三点
    QVariant pointVar = input.data("point");
    QVariant planeVar = input.data("plane");

    if (!pointVar.isValid()) {
        emit errorOccurred(tr("未提供输入点"));
        return false;
    }

    if (!planeVar.isValid()) {
        emit errorOccurred(tr("未提供平面数据"));
        return false;
    }

    // 解析点 (支持 [x,y] 或 [x,y,z])
    QString parseError;
    auto point = MeasurementData::parsePoint3D(pointVar, &parseError);
    if (!point) {
        emit errorOccurred(tr("点数据格式无效: %1").arg(parseError));
        return false;
    }

    // 解析平面 (9个值: x1,y1,z1,x2,y2,z2,x3,y3,z3)
    auto plane = MeasurementData::parsePlane3D(planeVar, &parseError);
    if (!plane) {
        emit errorOccurred(tr("平面数据格式无效: %1").arg(parseError));
        return false;
    }

    // 计算点到平面的距离
    m_resultDistance = calculatePointToPlaneDistance(point->x, point->y, point->z,
                                                     plane->p1.x, plane->p1.y, plane->p1.z,
                                                     plane->p2.x, plane->p2.y, plane->p2.z,
                                                     plane->p3.x, plane->p3.y, plane->p3.z);

    // 计算垂足
    // 平面法向量
    double nx = (plane->p2.y - plane->p1.y) * (plane->p3.z - plane->p1.z)
              - (plane->p2.z - plane->p1.z) * (plane->p3.y - plane->p1.y);
    double ny = (plane->p2.z - plane->p1.z) * (plane->p3.x - plane->p1.x)
              - (plane->p2.x - plane->p1.x) * (plane->p3.z - plane->p1.z);
    double nz = (plane->p2.x - plane->p1.x) * (plane->p3.y - plane->p1.y)
              - (plane->p2.y - plane->p1.y) * (plane->p3.x - plane->p1.x);
    double norm = sqrt(nx * nx + ny * ny + nz * nz);

    if (norm > 1e-10) {
        // 平面方程: nx*(x-p1.x) + ny*(y-p1.y) + nz*(z-p1.z) = 0
        // 计算垂足
        double t = (nx * (plane->p1.x - point->x) + ny * (plane->p1.y - point->y)
                    + nz * (plane->p1.z - point->z)) / (norm * norm);
        m_resultFootX = point->x + t * nx;
        m_resultFootY = point->y + t * ny;
        m_resultFootZ = point->z + t * nz;
    }

    // 设置输出数据
    output.setData("distance", m_resultDistance);
    output.setData("foot_x", m_resultFootX);
    output.setData("foot_y", m_resultFootY);
    output.setData("foot_z", m_resultFootZ);

    QString result = QString("点面距离: %1, 垂足: (%2, %3, %4)")
                        .arg(m_resultDistance, 0, 'f', 3)
                        .arg(m_resultFootX, 0, 'f', 3)
                        .arg(m_resultFootY, 0, 'f', 3)
                        .arg(m_resultFootZ, 0, 'f', 3);
    Logger::instance().debug(result, "PointSurfaceDistance");

    return true;
}

double PointSurfaceDistancePlugin::calculatePointToPlaneDistance(double pointX, double pointY, double pointZ,
                                                               double planeX1, double planeY1, double planeZ1,
                                                               double planeX2, double planeY2, double planeZ2,
                                                               double planeX3, double planeY3, double planeZ3)
{
    // 计算平面法向量
    double nx = (planeY2 - planeY1) * (planeZ3 - planeZ1) - (planeZ2 - planeZ1) * (planeY3 - planeY1);
    double ny = (planeZ2 - planeZ1) * (planeX3 - planeX1) - (planeX2 - planeX1) * (planeZ3 - planeZ1);
    double nz = (planeX2 - planeX1) * (planeY3 - planeY1) - (planeY2 - planeY1) * (planeX3 - planeX1);

    double norm = sqrt(nx * nx + ny * ny + nz * nz);

    if (norm < 1e-10) {
        return -1;  // 平面三点共线
    }

    // 点到平面距离公式: |Ax + By + Cz + D| / sqrt(A^2 + B^2 + C^2)
    // 其中 D = -(Ax1 + By1 + Cz1)
    double D = -(nx * planeX1 + ny * planeY1 + nz * planeZ1);
    double distance = fabs(nx * pointX + ny * pointY + nz * pointZ + D) / norm;

    return distance;
}

bool PointSurfaceDistancePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* PointSurfaceDistancePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("计算点到平面的距离")));
    layout->addStretch();
    return widget;
}

IModule* PointSurfaceDistancePlugin::cloneImpl() const
{
    return new PointSurfaceDistancePlugin();
}

} // namespace DeepLux
