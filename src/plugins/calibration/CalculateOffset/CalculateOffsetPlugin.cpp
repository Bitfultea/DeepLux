#include "CalculateOffsetPlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"
#include "core/geometry/MeasurementData.h"

#include <cmath>

namespace DeepLux {

namespace {
// 角度差归一化到 (-180, 180]（与全代码库度制口径一致）
double normalizeAngle180(double deg) {
    double d = deg;
    while (d > 180.0) {
        d -= 360.0;
    }
    while (d <= -180.0) {
        d += 360.0;
    }
    return d;
}
} // namespace

CalculateOffsetPlugin::CalculateOffsetPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"currentX", 0.0}, {"currentY", 0.0}, {"currentAngle", 0.0},
                                  {"targetX", 0.0},  {"targetY", 0.0},  {"targetAngle", 0.0}};
    m_params = m_defaultParams;
}

CalculateOffsetPlugin::~CalculateOffsetPlugin() {}

bool CalculateOffsetPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "CalculateOffsetPlugin initialized";
    return true;
}

void CalculateOffsetPlugin::shutdown() {
    ModuleBase::shutdown();
}

bool CalculateOffsetPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();
    auto num = [&params, &error](const char* key, double lo, double hi, const QString& msg) {
        const QJsonValue v = params[QLatin1String(key)];
        if (!v.isDouble()) {
            error = msg;
            return false;
        }
        const double d = v.toDouble();
        if (!std::isfinite(d) || d < lo || d > hi) {
            error = msg;
            return false;
        }
        return true;
    };
    if (!num("currentX", -1e6, 1e6, tr("当前坐标X必须为[-1e6,1e6]有限数")))
        return false;
    if (!num("currentY", -1e6, 1e6, tr("当前坐标Y必须为[-1e6,1e6]有限数")))
        return false;
    if (!num("currentAngle", -360.0, 360.0, tr("当前角度必须为[-360,360]有限数")))
        return false;
    if (!num("targetX", -1e6, 1e6, tr("基准坐标X必须为[-1e6,1e6]有限数")))
        return false;
    if (!num("targetY", -1e6, 1e6, tr("基准坐标Y必须为[-1e6,1e6]有限数")))
        return false;
    if (!num("targetAngle", -360.0, 360.0, tr("基准角度必须为[-360,360]有限数")))
        return false;
    return true;
}

bool CalculateOffsetPlugin::process(const ImageData& input, ImageData& output) {
    output = input;
    const QJsonObject params = currentParams();
    QString perr;
    if (!doValidateParams(params, perr)) {
        emit errorOccurred(perr);
        return false;
    }
    double curX = params["currentX"].toDouble();
    double curY = params["currentY"].toDouble();
    const double curA = params["currentAngle"].toDouble();
    const double tgtX = params["targetX"].toDouble();
    const double tgtY = params["targetY"].toDouble();
    const double tgtA = params["targetAngle"].toDouble();

    // 可选 Point3D 端口覆盖当前坐标（取 x,y，忽略 z）：先经核心契约严格门禁
    // （批1结论：插件判定必须与 portValueMatchesType 一致），再走核心解析
    const QVariant ptVar = input.data("current_point");
    if (ptVar.isValid()) {
        if (!portValueMatchesType(ptVar, DataType::Point3D)) {
            emit errorOccurred(tr("当前坐标点格式非法（须为 Point3D：3 数值列表）"));
            return false;
        }
        QString parseError;
        const auto pt = MeasurementData::parsePoint3D(ptVar, &parseError);
        if (!pt) {
            emit errorOccurred(tr("当前坐标点解析失败: %1").arg(parseError));
            return false;
        }
        if (!std::isfinite(pt->x) || !std::isfinite(pt->y)) {
            emit errorOccurred(tr("当前坐标点含非有限值"));
            return false;
        }
        curX = pt->x;
        curY = pt->y;
    }

    const double offsetX = tgtX - curX;
    const double offsetY = tgtY - curY;
    const double offsetA = normalizeAngle180(tgtA - curA);

    output.setData("offset_x", offsetX);
    output.setData("offset_y", offsetY);
    output.setData("offset_a", offsetA);
    Logger::instance().debug(
        QString("对位偏移: dX=%1 dY=%2 dA=%3").arg(offsetX, 0, 'f', 3).arg(offsetY, 0, 'f', 3).arg(offsetA, 0, 'f', 3),
        "CalculateOffset");
    return true;
}

IModule* CalculateOffsetPlugin::cloneImpl() const {
    CalculateOffsetPlugin* clone = new CalculateOffsetPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
