#include "LinesDistancePlugin.h"
#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"
#include <QVBoxLayout>
#include <QLabel>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

LinesDistancePlugin::LinesDistancePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
    };
    m_params = m_defaultParams;
}

LinesDistancePlugin::~LinesDistancePlugin()
{
}

bool LinesDistancePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "LinesDistancePlugin initialized";
    return true;
}

void LinesDistancePlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool LinesDistancePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    // 获取输入数据
    QVariant line1Var = input.data("line1");
    QVariant line2Var = input.data("line2");

    if (!line1Var.isValid()) {
        emit errorOccurred(tr("未提供第一条直线"));
        return false;
    }

    if (!line2Var.isValid()) {
        emit errorOccurred(tr("未提供第二条直线"));
        return false;
    }

    // 解析直线1
    QString parseError;
    auto line1 = MeasurementData::parseLine2D(line1Var, &parseError);
    if (!line1) {
        emit errorOccurred(tr("直线1数据格式无效: %1").arg(parseError));
        return false;
    }

    // 解析直线2
    auto line2 = MeasurementData::parseLine2D(line2Var, &parseError);
    if (!line2) {
        emit errorOccurred(tr("直线2数据格式无效: %1").arg(parseError));
        return false;
    }

    // 计算两直线间的距离
    m_resultDistance = calculateLinesDistance(line1->p1.x, line1->p1.y, line1->p2.x, line1->p2.y,
                                              line2->p1.x, line2->p1.y, line2->p2.x, line2->p2.y);

    // 设置输出数据
    output.setData("distance", m_resultDistance);

    QString result = QString("线线距离: %1").arg(m_resultDistance, 0, 'f', 2);
    Logger::instance().debug(result, "LinesDistance");

    return true;
}

double LinesDistancePlugin::calculateLinesDistance(double line1X1, double line1Y1, double line1X2, double line1Y2,
                                                 double line2X1, double line2Y1, double line2X2, double line2Y2)
{
    // 将两条直线转换为标准式: Ax + By + C = 0
    // 计算直线1的参数
    double A1 = line1Y2 - line1Y1;
    double B1 = line1X1 - line1X2;
    double C1 = line1X2 * line1Y1 - line1X1 * line1Y2;

    // 计算直线2的参数
    double A2 = line2Y2 - line2Y1;
    double B2 = line2X1 - line2X2;
    double C2 = line2X2 * line2Y1 - line2X1 * line2Y2;

    // 计算两条直线是否平行
    double crossProduct = A1 * B2 - A2 * B1;

    if (qAbs(crossProduct) < 1e-10) {
        // 两条直线平行，使用点到直线的距离公式
        // 选择直线1上的一个点，计算到直线2的距离
        double dist = qAbs(A2 * line1X1 + B2 * line1Y1 + C2) / sqrt(A2 * A2 + B2 * B2);
        return dist;
    } else {
        // 两条直线相交，距离为0
        return 0.0;
    }
}

bool LinesDistancePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* LinesDistancePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("计算两条直线之间的距离")));
    layout->addStretch();
    return widget;
}

IModule* LinesDistancePlugin::cloneImpl() const
{
    LinesDistancePlugin* clone = new LinesDistancePlugin();
    return clone;
}

} // namespace DeepLux
