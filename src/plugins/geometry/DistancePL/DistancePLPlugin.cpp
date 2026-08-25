#include "DistancePLPlugin.h"

#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"

#include <QLabel>
#include <QVBoxLayout>
#include <cmath>

namespace DeepLux {

DistancePLPlugin::DistancePLPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{};
    m_params = m_defaultParams;
}

DistancePLPlugin::~DistancePLPlugin() {}

bool DistancePLPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "DistancePLPlugin initialized";
    return true;
}

void DistancePLPlugin::shutdown() {
    ModuleBase::shutdown();
}

bool DistancePLPlugin::process(const ImageData& input, ImageData& output) {
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

    QString parseError;
    auto point = MeasurementData::parsePoint2D(pointVar, &parseError);
    if (!point) {
        emit errorOccurred(tr("点数据格式无效: %1").arg(parseError));
        return false;
    }

    auto line = MeasurementData::parseLine2D(lineVar, &parseError);
    if (!line) {
        emit errorOccurred(tr("直线数据格式无效: %1").arg(parseError));
        return false;
    }

    // 计算点到直线的距离
    m_resultDistance = calculateDistancePointToLine(point->x, point->y, line->p1.x, line->p1.y, line->p2.x, line->p2.y);

    // 计算垂足点
    double dx = line->p2.x - line->p1.x;
    double dy = line->p2.y - line->p1.y;
    double lineLengthSq = dx * dx + dy * dy;

    if (lineLengthSq > 1e-10) {
        double t = ((point->x - line->p1.x) * dx + (point->y - line->p1.y) * dy) / lineLengthSq;
        m_resultFootX = line->p1.x + t * dx;
        m_resultFootY = line->p1.y + t * dy;
    } else {
        m_resultFootX = line->p1.x;
        m_resultFootY = line->p1.y;
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

double DistancePLPlugin::calculateDistancePointToLine(double pointX, double pointY, double lineX1, double lineY1,
                                                      double lineX2, double lineY2) {
    // 点到线段的距离公式
    double dx = lineX2 - lineX1;
    double dy = lineY2 - lineY1;
    double lineLengthSq = dx * dx + dy * dy;

    if (lineLengthSq < 1e-10) {
        // 线段是一个点
        return sqrt((pointX - lineX1) * (pointX - lineX1) + (pointY - lineY1) * (pointY - lineY1));
    }

    // 计算投影参数 t
    double t = ((pointX - lineX1) * dx + (pointY - lineY1) * dy) / lineLengthSq;

    // 如果垂足在线段上
    if (t >= 0 && t <= 1) {
        double footX = lineX1 + t * dx;
        double footY = lineY1 + t * dy;
        return sqrt((pointX - footX) * (pointX - footX) + (pointY - footY) * (pointY - footY));
    }

    // 垂足不在线段上，返回到最近端点的距离
    double distToStart = sqrt((pointX - lineX1) * (pointX - lineX1) + (pointY - lineY1) * (pointY - lineY1));
    double distToEnd = sqrt((pointX - lineX2) * (pointX - lineX2) + (pointY - lineY2) * (pointY - lineY2));

    return qMin(distToStart, distToEnd);
}

bool DistancePLPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* DistancePLPlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("计算点到直线的距离")));
    layout->addStretch();
    return widget;
}

IModule* DistancePLPlugin::cloneImpl() const {
    DistancePLPlugin* clone = new DistancePLPlugin();
    return clone;
}

} // namespace DeepLux
