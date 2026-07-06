#include "DistancePPPlugin.h"
#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"
#include <QVBoxLayout>
#include <QLabel>
#include <cmath>

namespace DeepLux {

DistancePPPlugin::DistancePPPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
    };
    m_params = m_defaultParams;
}

DistancePPPlugin::~DistancePPPlugin()
{
}

bool DistancePPPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "DistancePPPlugin initialized";
    return true;
}

void DistancePPPlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool DistancePPPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    // 获取输入数据
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

    QString parseError;
    auto pt1 = MeasurementData::parsePoint2D(point1Var, &parseError);
    if (!pt1) {
        emit errorOccurred(tr("点1数据格式无效: %1").arg(parseError));
        return false;
    }

    auto pt2 = MeasurementData::parsePoint2D(point2Var, &parseError);
    if (!pt2) {
        emit errorOccurred(tr("点2数据格式无效: %1").arg(parseError));
        return false;
    }

    // 计算距离
    m_resultDistance = calculateDistance(pt1->x, pt1->y, pt2->x, pt2->y);
    m_resultDeltaX = pt2->x - pt1->x;
    m_resultDeltaY = pt2->y - pt1->y;

    // 设置输出数据
    output.setData("distance", m_resultDistance);
    output.setData("delta_x", m_resultDeltaX);
    output.setData("delta_y", m_resultDeltaY);

    QString result = QString("点点距离: %1, ΔX: %2, ΔY: %3")
                        .arg(m_resultDistance, 0, 'f', 2)
                        .arg(m_resultDeltaX, 0, 'f', 2)
                        .arg(m_resultDeltaY, 0, 'f', 2);
    Logger::instance().debug(result, "DistancePP");

    return true;
}

double DistancePPPlugin::calculateDistance(double x1, double y1, double x2, double y2)
{
    return sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
}

bool DistancePPPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* DistancePPPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("计算两点之间的距离")));
    layout->addStretch();
    return widget;
}

IModule* DistancePPPlugin::cloneImpl() const
{
    DistancePPPlugin* clone = new DistancePPPlugin();
    return clone;
}

} // namespace DeepLux
