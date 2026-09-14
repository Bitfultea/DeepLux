#include "CalculateOffsetPlugin.h"

#include "common/Logger.h"
#include "core/deeplux/DataContract.h"
#include "core/geometry/MeasurementData.h"

#include <cmath>

namespace DeepLux {

namespace {
// 角度差归一化到 (-180, 180]（与全代码库度制口径一致）。
// 阶7 批4复核三轮（P0-1）：fmod 常数时间归一——循环逐次减/加 360 对超大
// 有限角度（如 current_angle 端口传入 1e300）因 d-360==d 浮点吸收而永不
// 终止，流程永久卡死
double normalizeAngle180(double deg) {
    double d = std::fmod(deg, 360.0); // (-360, 360)
    if (d > 180.0) {
        d -= 360.0;
    } else if (d <= -180.0) {
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
    double curA = params["currentAngle"].toDouble();
    const double tgtX = params["targetX"].toDouble();
    const double tgtY = params["targetY"].toDouble();
    const double tgtA = params["targetAngle"].toDouble();

    // 阶7 批4复核（P1-4）：current_angle Number 端口逐帧覆盖当前角度（旧版
    // DegLink 运行时链接对应物）；键存在但类型错误或非有限失败关闭
    const QVariant angVar = input.data("current_angle");
    if (angVar.isValid()) {
        if (!portValueMatchesType(angVar, DataType::Number)) {
            emit errorOccurred(tr("current_angle 端口类型非法（须为数值）"));
            return false;
        }
        const double a = angVar.toDouble();
        if (!std::isfinite(a)) {
            emit errorOccurred(tr("current_angle 端口值非有限"));
            return false;
        }
        curA = a;
    }

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

    // 阶7 批4复核（P1-1）：方向与旧版一致——offset = 实测(current) − 参考(target)。
    // 旧版 OffsetX = -(RealXRef - RealFindX) = Find - Ref（MathCoord 实测链接值减
    // ModeCoord 参考值）；Hommat2DTrans 仿射与 EnableRotateCenter 旋转中心补正
    // 未实现（partial），坐标按参数单位直接作差
    const double offsetX = curX - tgtX;
    const double offsetY = curY - tgtY;
    const double offsetA = normalizeAngle180(curA - tgtA);

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
