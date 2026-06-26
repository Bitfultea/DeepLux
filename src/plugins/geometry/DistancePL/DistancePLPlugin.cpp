#include "DistancePLPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <cmath>

namespace DeepLux {

namespace {

bool toFiniteDouble(const QVariant& value, double& number)
{
    bool ok = false;
    number = value.toDouble(&ok);
    return ok && std::isfinite(number);
}

bool parsePoint(const QVariant& value, QPointF& point)
{
    if (value.canConvert<QPointF>()) {
        point = value.toPointF();
        return std::isfinite(point.x()) && std::isfinite(point.y());
    }

    QList<QVariant> pointList = value.toList();
    if (pointList.size() < 2) {
        return false;
    }

    double x = 0.0;
    double y = 0.0;
    if (!toFiniteDouble(pointList[0], x) || !toFiniteDouble(pointList[1], y)) {
        return false;
    }

    point = QPointF(x, y);
    return true;
}

bool parseLineCoordinates(const QVariant& value, double& x1, double& y1, double& x2, double& y2)
{
    if (value.canConvert<QVector<QPointF>>()) {
        QVector<QPointF> linePoints = value.value<QVector<QPointF>>();
        if (linePoints.size() < 2) {
            return false;
        }

        x1 = linePoints[0].x();
        y1 = linePoints[0].y();
        x2 = linePoints[1].x();
        y2 = linePoints[1].y();
        return std::isfinite(x1) && std::isfinite(y1) && std::isfinite(x2) && std::isfinite(y2);
    }

    QList<QVariant> lineList = value.toList();
    if (lineList.size() < 4) {
        return false;
    }

    return toFiniteDouble(lineList[0], x1)
        && toFiniteDouble(lineList[1], y1)
        && toFiniteDouble(lineList[2], x2)
        && toFiniteDouble(lineList[3], y2);
}

} // namespace

DistancePLPlugin::DistancePLPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
    };
    m_params = m_defaultParams;
}

DistancePLPlugin::~DistancePLPlugin()
{
}

bool DistancePLPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "DistancePLPlugin initialized";
    return true;
}

void DistancePLPlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool DistancePLPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    // 获取输入数据
    QVariant pointVar = input.data("point");
    QVariant lineVar = input.data("line");

    if (!pointVar.isValid()) {
        emit errorOccurred(tr("未提供输入点"));
        return false;
    }

    if (!lineVar.isValid()) {
        emit errorOccurred(tr("未提供输入直线"));
        return false;
    }

    QPointF point;
    if (!parsePoint(pointVar, point)) {
        emit errorOccurred(tr("点数据格式无效"));
        return false;
    }

    double lineX1, lineY1, lineX2, lineY2;
    if (!parseLineCoordinates(lineVar, lineX1, lineY1, lineX2, lineY2)) {
        emit errorOccurred(tr("直线数据格式无效"));
        return false;
    }

    // 计算点到直线的距离
    m_resultDistance = calculateDistancePointToLine(point.x(), point.y(),
                                                    lineX1, lineY1, lineX2, lineY2);

    // 计算垂足点
    double dx = lineX2 - lineX1;
    double dy = lineY2 - lineY1;
    double lineLengthSq = dx * dx + dy * dy;

    if (lineLengthSq > 1e-10) {
        double t = ((point.x() - lineX1) * dx + (point.y() - lineY1) * dy) / lineLengthSq;
        m_resultFootX = lineX1 + t * dx;
        m_resultFootY = lineY1 + t * dy;
    } else {
        m_resultFootX = lineX1;
        m_resultFootY = lineY1;
    }

    // 设置输出数据
    output.setData("distance", m_resultDistance);
    output.setData("foot_x", m_resultFootX);
    output.setData("foot_y", m_resultFootY);

    QString result = QString("点线距离: %1, 垂足: (%2, %3)")
                        .arg(m_resultDistance, 0, 'f', 2)
                        .arg(m_resultFootX, 0, 'f', 2)
                        .arg(m_resultFootY, 0, 'f', 2);
    Logger::instance().debug(result, "DistancePL");

    return true;
}

double DistancePLPlugin::calculateDistancePointToLine(double pointX, double pointY,
                                                      double lineX1, double lineY1,
                                                      double lineX2, double lineY2)
{
    // 点到线段的距离公式
    double dx = lineX2 - lineX1;
    double dy = lineY2 - lineY1;
    double lineLengthSq = dx * dx + dy * dy;

    if (lineLengthSq < 1e-10) {
        // 线段是一个点
        return sqrt((pointX - lineX1) * (pointX - lineX1) +
                     (pointY - lineY1) * (pointY - lineY1));
    }

    // 计算投影参数 t
    double t = ((pointX - lineX1) * dx + (pointY - lineY1) * dy) / lineLengthSq;

    // 如果垂足在线段上
    if (t >= 0 && t <= 1) {
        double footX = lineX1 + t * dx;
        double footY = lineY1 + t * dy;
        return sqrt((pointX - footX) * (pointX - footX) +
                     (pointY - footY) * (pointY - footY));
    }

    // 垂足不在线段上，返回到最近端点的距离
    double distToStart = sqrt((pointX - lineX1) * (pointX - lineX1) +
                              (pointY - lineY1) * (pointY - lineY1));
    double distToEnd = sqrt((pointX - lineX2) * (pointX - lineX2) +
                           (pointY - lineY2) * (pointY - lineY2));

    return qMin(distToStart, distToEnd);
}

bool DistancePLPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* DistancePLPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("计算点到直线的距离")));
    layout->addStretch();
    return widget;
}

IModule* DistancePLPlugin::cloneImpl() const
{
    DistancePLPlugin* clone = new DistancePLPlugin();
    return clone;
}

} // namespace DeepLux
