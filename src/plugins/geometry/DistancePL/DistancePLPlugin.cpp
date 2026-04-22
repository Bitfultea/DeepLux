#include "DistancePLPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>

namespace DeepLux {

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

    // 解析点 (x, y)
    QPointF point;
    if (pointVar.canConvert<QPointF>()) {
        point = pointVar.toPointF();
    } else {
        QList<QVariant> pointList = pointVar.toList();
        if (pointList.size() >= 2) {
            point = QPointF(pointList[0].toDouble(), pointList[1].toDouble());
        } else {
            emit errorOccurred(tr("点数据格式无效"));
            return false;
        }
    }

    // 解析直线 (x1, y1, x2, y2)
    double lineX1, lineY1, lineX2, lineY2;
    if (lineVar.canConvert<QVector<QPointF>>()) {
        QVector<QPointF> linePoints = lineVar.value<QVector<QPointF>>();
        if (linePoints.size() < 2) {
            emit errorOccurred(tr("直线需要至少2个点"));
            return false;
        }
        lineX1 = linePoints[0].x();
        lineY1 = linePoints[0].y();
        lineX2 = linePoints[1].x();
        lineY2 = linePoints[1].y();
    } else {
        QList<QVariant> lineList = lineVar.toList();
        if (lineList.size() >= 4) {
            lineX1 = lineList[0].toDouble();
            lineY1 = lineList[1].toDouble();
            lineX2 = lineList[2].toDouble();
            lineY2 = lineList[3].toDouble();
        } else {
            emit errorOccurred(tr("直线数据格式无效"));
            return false;
        }
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
