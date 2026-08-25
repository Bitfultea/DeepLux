#include "LinesDistancePlugin.h"

#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"

#include <QLabel>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

LinesDistancePlugin::LinesDistancePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{};
    m_params = m_defaultParams;
}

LinesDistancePlugin::~LinesDistancePlugin() {}

bool LinesDistancePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "LinesDistancePlugin initialized";
    return true;
}

void LinesDistancePlugin::shutdown() {
    ModuleBase::shutdown();
}

bool LinesDistancePlugin::process(const ImageData& input, ImageData& output) {
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

    // P0-1: 计算两"线段"间的最短距离（有限线段，非无限直线），并输出最近点对
    m_resultDistance =
        calculateSegmentDistance(line1->p1.x, line1->p1.y, line1->p2.x, line1->p2.y, line2->p1.x, line2->p1.y,
                                 line2->p2.x, line2->p2.y, m_nearAx, m_nearAy, m_nearBx, m_nearBy);

    // 设置输出数据
    output.setData("distance", m_resultDistance);
    output.setData("nearest_x1", m_nearAx);
    output.setData("nearest_y1", m_nearAy);
    output.setData("nearest_x2", m_nearBx);
    output.setData("nearest_y2", m_nearBy);

    QString result = QString("线段距离: %1").arg(m_resultDistance, 0, 'f', 2);
    Logger::instance().debug(result, "LinesDistance");

    return true;
}

double LinesDistancePlugin::pointSegmentDistance(double px, double py, double ax, double ay, double bx, double by,
                                                 double& nearestX, double& nearestY) {
    const double dx = bx - ax;
    const double dy = by - ay;
    const double lenSq = dx * dx + dy * dy;

    // 退化线段（零长度）：端点即最近点
    if (lenSq < 1e-12) {
        nearestX = ax;
        nearestY = ay;
        return std::hypot(px - ax, py - ay);
    }

    // 投影参数 t ∈ [0,1]
    double t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
    t = qBound(0.0, t, 1.0);
    nearestX = ax + t * dx;
    nearestY = ay + t * dy;
    return std::hypot(px - nearestX, py - nearestY);
}

double LinesDistancePlugin::calculateSegmentDistance(double ax1, double ay1, double ax2, double ay2, double bx1,
                                                     double by1, double bx2, double by2, double& nearAx, double& nearAy,
                                                     double& nearBx, double& nearBy) {
    // 阶1: 统一浮点容差
    constexpr double kEps = 1e-9;

    const double dax = ax2 - ax1, day = ay2 - ay1;
    const double dbx = bx2 - bx1, dby = by2 - by1;
    const double lenA = std::hypot(dax, day);
    const double denom = dax * dby - day * dbx;

    // 阶1-交叉: 两直线不平行且交点同时落在两线段内 → 距离 0，最近点=交点。
    if (qAbs(denom) > kEps * qMax(1.0, lenA * std::hypot(dbx, dby))) {
        const double t = ((bx1 - ax1) * dby - (by1 - ay1) * dbx) / denom;
        const double s = ((bx1 - ax1) * day - (by1 - ay1) * dax) / denom;
        if (t >= 0.0 && t <= 1.0 && s >= 0.0 && s <= 1.0) {
            const double ix = ax1 + t * dax;
            const double iy = ay1 + t * day;
            nearAx = ix;
            nearAy = iy;
            nearBx = ix;
            nearBy = iy;
            return 0.0;
        }
    }

    // 阶1-共线: 方向平行且 B 的端点在 A 直线上 → 计算重叠区间，返回"确定的重叠起点"。
    const double crossAB = (bx1 - ax1) * day - (by1 - ay1) * dax; // (B1-A1) x dirA
    if (lenA > kEps && qAbs(denom) <= kEps * qMax(1.0, lenA * std::hypot(dbx, dby)) && qAbs(crossAB) <= kEps * lenA) {
        // 沿 A 方向参数化（长度量纲）
        const double tB1 = (bx1 - ax1) * dax + (by1 - ay1) * day;
        const double tB2 = (bx2 - ax1) * dax + (by2 - ay1) * day;
        const double lo = qMax(0.0, qMin(tB1, tB2));
        const double hi = qMin(lenA * lenA, qMax(tB1, tB2));
        if (lo <= hi + kEps) {
            // 重叠起点（沿 A 参数 lo 归一化）
            const double px = ax1 + (lo / (lenA * lenA)) * dax;
            const double py = ay1 + (lo / (lenA * lenA)) * day;
            nearAx = px;
            nearAy = py;
            nearBx = px;
            nearBy = py;
            return 0.0;
        }
    }

    // 不相交（含端点接触的退化情形）：最近距离为某端点到另一线段的最短距离。
    double best = std::numeric_limits<double>::max();
    double nx = 0, ny = 0, mx = 0, my = 0;

    // 线段 A 的端点到线段 B
    double d = pointSegmentDistance(ax1, ay1, bx1, by1, bx2, by2, nx, ny);
    if (d < best) {
        best = d;
        nearAx = ax1;
        nearAy = ay1;
        nearBx = nx;
        nearBy = ny;
    }
    d = pointSegmentDistance(ax2, ay2, bx1, by1, bx2, by2, nx, ny);
    if (d < best) {
        best = d;
        nearAx = ax2;
        nearAy = ay2;
        nearBx = nx;
        nearBy = ny;
    }
    // 线段 B 的端点到线段 A
    d = pointSegmentDistance(bx1, by1, ax1, ay1, ax2, ay2, mx, my);
    if (d < best) {
        best = d;
        nearAx = mx;
        nearAy = my;
        nearBx = bx1;
        nearBy = by1;
    }
    d = pointSegmentDistance(bx2, by2, ax1, ay1, ax2, ay2, mx, my);
    if (d < best) {
        best = d;
        nearAx = mx;
        nearAy = my;
        nearBx = bx2;
        nearBy = by2;
    }
    return best;
}

bool LinesDistancePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* LinesDistancePlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("计算两条线段之间的最短距离")));
    layout->addStretch();
    return widget;
}

IModule* LinesDistancePlugin::cloneImpl() const {
    LinesDistancePlugin* clone = new LinesDistancePlugin();
    return clone;
}

} // namespace DeepLux
