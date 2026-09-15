#include "RegionStatsPlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"

#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace DeepLux {

namespace {

// map 形态 polygon 解析：元素为 QPointF 或 2 数值列表（与 DataContract isRegion2D 口径一致）
bool parsePolygon(const QVariant& polyVar, QList<QPointF>& out) {
    if (polyVar.type() != QVariant::List)
        return false;
    const QVariantList list = polyVar.toList();
    for (const QVariant& item : list) {
        if (item.type() == QVariant::PointF) {
            out.append(item.toPointF());
            continue;
        }
        if (item.type() != QVariant::List)
            return false;
        const QVariantList xy = item.toList();
        if (xy.size() != 2)
            return false;
        bool okX = false;
        bool okY = false;
        const double x = xy[0].toDouble(&okX);
        const double y = xy[1].toDouble(&okY);
        if (!okX || !okY)
            return false;
        out.append(QPointF(x, y));
    }
    return true;
}

// shoelace：2× 带符号面积（顶点序闭合，逆/顺时针均正确，符号在质心公式中自消）
double shoelaceArea2(const QList<QPointF>& poly) {
    double area2 = 0.0;
    const int n = poly.size();
    for (int i = 0; i < n; ++i) {
        const QPointF& p = poly[i];
        const QPointF& q = poly[(i + 1) % n];
        area2 += p.x() * q.y() - q.x() * p.y();
    }
    return area2;
}

// 面积加权质心（仅 |area2| > 0 时有定义）
QPointF polygonCentroid(const QList<QPointF>& poly, double area2) {
    double cx = 0.0;
    double cy = 0.0;
    const int n = poly.size();
    for (int i = 0; i < n; ++i) {
        const QPointF& p = poly[i];
        const QPointF& q = poly[(i + 1) % n];
        const double cross = p.x() * q.y() - q.x() * p.y();
        cx += (p.x() + q.x()) * cross;
        cy += (p.y() + q.y()) * cross;
    }
    return QPointF(cx / (3.0 * area2), cy / (3.0 * area2));
}

} // namespace

RegionStatsPlugin::RegionStatsPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{};
    m_params = m_defaultParams;
}

RegionStatsPlugin::~RegionStatsPlugin() {}

bool RegionStatsPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "RegionStatsPlugin initialized";
    return true;
}

void RegionStatsPlugin::shutdown() {
    ModuleBase::shutdown();
}

bool RegionStatsPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    Q_UNUSED(params);
    error.clear();
    return true; // 无可配置参数
}

bool RegionStatsPlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    const QVariant regionVar = input.data(QStringLiteral("region"));
    if (!regionVar.isValid()) {
        emit errorOccurred(tr("未提供 region 输入"));
        return false;
    }
    if (!portValueMatchesType(regionVar, DataType::Region2D)) {
        emit errorOccurred(tr("region 输入不符合 Region2D 契约（强类型或 polygon+bbox map）"));
        return false;
    }

    Region2D region;
    if (regionVar.canConvert<Region2D>()) {
        region = regionVar.value<Region2D>();
    } else {
        const QVariantMap m = regionVar.toMap();
        if (!parsePolygon(m.value(QStringLiteral("polygon")), region.polygon)) {
            emit errorOccurred(tr("region.polygon 解析失败"));
            return false;
        }
        const QVariantList bb = m.value(QStringLiteral("bbox")).toList();
        region.bbox = QRectF(bb[0].toDouble(), bb[1].toDouble(), bb[2].toDouble(), bb[3].toDouble());
        const QVariant score = m.value(QStringLiteral("score"));
        region.score = score.isValid() ? score.toDouble() : 0.0;
        region.label = m.value(QStringLiteral("label")).toString();
        // maskRle 不参与统计，原样随 output 透传
    }

    // 外接矩形：优先载荷自带 bbox；polygon-only（强类型允许）时由多边形推导
    QRectF bbox = region.bbox;
    if (!bbox.isValid()) {
        if (region.polygon.size() < 3) {
            // 契约保证 polygon<3 时 bbox 有效；此处防御直调 process 绕过桥接校验
            emit errorOccurred(tr("Region2D polygon 与 bbox 均无效"));
            return false;
        }
        double minX = region.polygon.first().x();
        double maxX = minX;
        double minY = region.polygon.first().y();
        double maxY = minY;
        for (const QPointF& p : region.polygon) {
            minX = std::min(minX, p.x());
            maxX = std::max(maxX, p.x());
            minY = std::min(minY, p.y());
            maxY = std::max(maxY, p.y());
        }
        bbox = QRectF(minX, minY, maxX - minX, maxY - minY);
    }

    double area = 0.0;
    QPointF centroid;
    if (region.polygon.size() >= 3) {
        const double area2 = shoelaceArea2(region.polygon);
        area = std::fabs(area2) / 2.0;
        // 退化多边形（共线/重合点，面积≈0）质心回退外接矩形中心
        centroid = (std::fabs(area2) > 1e-12) ? polygonCentroid(region.polygon, area2) : bbox.center();
    } else {
        // polygon<3：契约保证 bbox w,h>0，回退 bbox 面积
        area = bbox.width() * bbox.height();
        centroid = bbox.center();
    }

    output.setData(QStringLiteral("region_area"), area);
    output.setData(QStringLiteral("bbox_x"), bbox.x());
    output.setData(QStringLiteral("bbox_y"), bbox.y());
    output.setData(QStringLiteral("bbox_w"), bbox.width());
    output.setData(QStringLiteral("bbox_h"), bbox.height());
    output.setData(QStringLiteral("point_count"), region.polygon.size());
    output.setData(QStringLiteral("centroid_x"), centroid.x());
    output.setData(QStringLiteral("centroid_y"), centroid.y());
    output.setData(QStringLiteral("score"), region.score);
    output.setData(QStringLiteral("label"), region.label);

    Logger::instance().debug(QString("区域统计: label=%1 area=%2 points=%3 score=%4")
                                 .arg(region.label)
                                 .arg(area, 0, 'f', 2)
                                 .arg(region.polygon.size())
                                 .arg(region.score, 0, 'f', 3),
                             "RegionStats");
    return true;
}

IModule* RegionStatsPlugin::cloneImpl() const {
    RegionStatsPlugin* clone = new RegionStatsPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
