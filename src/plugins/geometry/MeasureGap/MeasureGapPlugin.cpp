#include "MeasureGapPlugin.h"
#include "common/Logger.h"
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

    // 解析点1 (支持 x,y,z 或 x,y 格式)
    double x1 = 0, y1 = 0, z1 = 0;
    if (point1Var.canConvert<QVector<QPointF>>()) {
        QVector<QPointF> pts = point1Var.value<QVector<QPointF>>();
        if (pts.size() >= 2) {
            x1 = pts[0].x();
            y1 = pts[0].y();
            z1 = pts.size() > 2 ? pts[2].x() : 0;
        }
    } else if (point1Var.canConvert<QPointF>()) {
        QPointF p = point1Var.toPointF();
        x1 = p.x();
        y1 = p.y();
    } else {
        QList<QVariant> list = point1Var.toList();
        if (list.size() >= 2) {
            x1 = list[0].toDouble();
            y1 = list[1].toDouble();
            z1 = list.size() > 2 ? list[2].toDouble() : 0;
        }
    }

    // 解析点2
    double x2 = 0, y2 = 0, z2 = 0;
    if (point2Var.canConvert<QVector<QPointF>>()) {
        QVector<QPointF> pts = point2Var.value<QVector<QPointF>>();
        if (pts.size() >= 2) {
            x2 = pts[0].x();
            y2 = pts[0].y();
            z2 = pts.size() > 2 ? pts[2].x() : 0;
        }
    } else if (point2Var.canConvert<QPointF>()) {
        QPointF p = point2Var.toPointF();
        x2 = p.x();
        y2 = p.y();
    } else {
        QList<QVariant> list = point2Var.toList();
        if (list.size() >= 2) {
            x2 = list[0].toDouble();
            y2 = list[1].toDouble();
            z2 = list.size() > 2 ? list[2].toDouble() : 0;
        }
    }

    // 计算间隙距离
    m_resultGap = calculateGapDistance(x1, y1, z1, x2, y2, z2);
    m_resultDeltaX = x2 - x1;
    m_resultDeltaY = y2 - y1;
    m_resultDeltaZ = z2 - z1;

    // 设置输出数据
    output.setData("gap_distance", m_resultGap);
    output.setData("gap_delta_x", m_resultDeltaX);
    output.setData("gap_delta_y", m_resultDeltaY);
    output.setData("gap_delta_z", m_resultDeltaZ);

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
