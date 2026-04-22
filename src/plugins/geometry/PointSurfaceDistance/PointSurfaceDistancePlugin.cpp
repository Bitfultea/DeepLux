#include "PointSurfaceDistancePlugin.h"
#include "common/Logger.h"
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

    // 解析点 (x, y, z)
    double px = 0, py = 0, pz = 0;
    QList<QVariant> pointList = pointVar.toList();
    if (pointList.size() >= 3) {
        px = pointList[0].toDouble();
        py = pointList[1].toDouble();
        pz = pointList[2].toDouble();
    } else if (pointList.size() >= 2) {
        px = pointList[0].toDouble();
        py = pointList[1].toDouble();
    }

    // 解析平面 (9个值: x1,y1,z1,x2,y2,z2,x3,y3,z3)
    double x1=0, y1=0, z1=0, x2=0, y2=0, z2=0, x3=0, y3=0, z3=0;
    QList<QVariant> planeList = planeVar.toList();
    if (planeList.size() >= 9) {
        x1 = planeList[0].toDouble(); y1 = planeList[1].toDouble(); z1 = planeList[2].toDouble();
        x2 = planeList[3].toDouble(); y2 = planeList[4].toDouble(); z2 = planeList[5].toDouble();
        x3 = planeList[6].toDouble(); y3 = planeList[7].toDouble(); z3 = planeList[8].toDouble();
    }

    // 计算点到平面的距离
    m_resultDistance = calculatePointToPlaneDistance(px, py, pz, x1, y1, z1, x2, y2, z2, x3, y3, z3);

    // 计算垂足（简化计算）
    // 平面法向量
    double nx = (y2-y1)*(z3-z1) - (z2-z1)*(y3-y1);
    double ny = (z2-z1)*(x3-x1) - (x2-x1)*(z3-z1);
    double nz = (x2-x1)*(y3-y1) - (y2-y1)*(x3-x1);
    double norm = sqrt(nx*nx + ny*ny + nz*nz);

    if (norm > 1e-10) {
        // 平面方程: nx*(x-x1) + ny*(y-y1) + nz*(z-z1) = 0
        // 计算垂足
        double t = (nx*(x1-px) + ny*(y1-py) + nz*(z1-pz)) / (norm*norm);
        m_resultFootX = px + t * nx;
        m_resultFootY = py + t * ny;
        m_resultFootZ = pz + t * nz;
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

} // namespace DeepLux