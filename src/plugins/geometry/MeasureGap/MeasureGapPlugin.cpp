#include "MeasureGapPlugin.h"
#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"
#include <QVBoxLayout>
#include <QLabel>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

MeasureGapPlugin::MeasureGapPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
    };
    m_params = m_defaultParams;
}

MeasureGapPlugin::~MeasureGapPlugin()
{
}

bool MeasureGapPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "MeasureGapPlugin initialized";
    return true;
}

void MeasureGapPlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool MeasureGapPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    // 获取两个3D点
    QVariant point1Var = input.data("point1");
    QVariant point2Var = input.data("point2");

    if (!point1Var.isValid()) {
        emit errorOccurred(tr("未提供第一个点"));
        return false;
    }

    if (!point2Var.isValid()) {
        emit errorOccurred(tr("未提供第二个点"));
        return false;
    }

    // 解析点1 (支持 [x,y] 或 [x,y,z] 格式)
    QString parseError;
    auto pt1 = MeasurementData::parsePoint3D(point1Var, &parseError);
    if (!pt1) {
        emit errorOccurred(tr("点1数据格式无效: %1").arg(parseError));
        return false;
    }

    // 解析点2
    auto pt2 = MeasurementData::parsePoint3D(point2Var, &parseError);
    if (!pt2) {
        emit errorOccurred(tr("点2数据格式无效: %1").arg(parseError));
        return false;
    }

    // 确定测量维度
    bool is3d = (pt1->z != 0.0 || pt2->z != 0.0);
    QString dimension = is3d ? QStringLiteral("3d") : QStringLiteral("2d");

    // 计算间隙距离
    m_resultGap = calculateGapDistance(pt1->x, pt1->y, pt1->z, pt2->x, pt2->y, pt2->z);
    m_resultDeltaX = pt2->x - pt1->x;
    m_resultDeltaY = pt2->y - pt1->y;
    m_resultDeltaZ = pt2->z - pt1->z;

    // 设置输出数据
    output.setData("gap_distance", m_resultGap);
    output.setData("gap_delta_x", m_resultDeltaX);
    output.setData("gap_delta_y", m_resultDeltaY);
    output.setData("gap_delta_z", m_resultDeltaZ);
    output.setData("measurement_dimension", dimension);

    QString result = QString("间隙: %1, ΔX: %2, ΔY: %3, ΔZ: %4")
                        .arg(m_resultGap, 0, 'f', 3)
                        .arg(m_resultDeltaX, 0, 'f', 3)
                        .arg(m_resultDeltaY, 0, 'f', 3)
                        .arg(m_resultDeltaZ, 0, 'f', 3);
    Logger::instance().debug(result, "MeasureGap");

    return true;
}

double MeasureGapPlugin::calculateGapDistance(double x1, double y1, double z1,
                                              double x2, double y2, double z2)
{
    double dx = x2 - x1;
    double dy = y2 - y1;
    double dz = z2 - z1;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

bool MeasureGapPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* MeasureGapPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("测量两点之间的3D间隙距离")));
    layout->addStretch();
    return widget;
}

IModule* MeasureGapPlugin::cloneImpl() const
{
    MeasureGapPlugin* clone = new MeasureGapPlugin();
    return clone;
}

} // namespace DeepLux
