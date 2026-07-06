#include "MeasurementInputPlugin.h"
#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"
#include <QJsonArray>
#include <QVBoxLayout>
#include <QLabel>

namespace DeepLux {

static QJsonArray makeJsonArray(std::initializer_list<double> vals)
{
    QJsonArray arr;
    for (double v : vals) {
        arr.append(v);
    }
    return arr;
}

MeasurementInputPlugin::MeasurementInputPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"mode", "point_pair"},
        {"point1", makeJsonArray({0.0, 0.0})},
        {"point2", makeJsonArray({0.0, 0.0})},
        {"point", makeJsonArray({0.0, 0.0, 0.0})},
        {"line", makeJsonArray({0.0, 0.0, 100.0, 0.0})},
        {"plane", makeJsonArray({0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0})}
    };
    m_params = m_defaultParams;
}

MeasurementInputPlugin::~MeasurementInputPlugin()
{
}

bool MeasurementInputPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "MeasurementInputPlugin initialized";
    return true;
}

void MeasurementInputPlugin::shutdown()
{
    ModuleBase::shutdown();
}

QVariantList MeasurementInputPlugin::jsonArrayToVariantList(const QJsonArray& arr)
{
    QVariantList list;
    for (const auto& val : arr) {
        list.append(val.toDouble());
    }
    return list;
}

bool MeasurementInputPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QJsonObject params = currentParams();
    QString mode = params["mode"].toString();

    QString error;

    if (mode == "point_pair") {
        QJsonArray point1Arr = params["point1"].toArray();
        QJsonArray point2Arr = params["point2"].toArray();
        QVariantList point1List = jsonArrayToVariantList(point1Arr);
        QVariantList point2List = jsonArrayToVariantList(point2Arr);

        auto p1 = MeasurementData::parsePoint2D(point1List, &error);
        if (!p1) {
            emit errorOccurred(tr("点1数据格式无效: %1").arg(error));
            return false;
        }
        auto p2 = MeasurementData::parsePoint2D(point2List, &error);
        if (!p2) {
            emit errorOccurred(tr("点2数据格式无效: %1").arg(error));
            return false;
        }

        output.setData("point1", point1List);
        output.setData("point2", point2List);
        output.setData("measurement_input_mode", mode);

        Logger::instance().debug(QString("测量输入: point_pair mode"), "MeasurementInput");
    }
    else if (mode == "point_line") {
        QJsonArray pointArr = params["point"].toArray();
        QJsonArray lineArr = params["line"].toArray();
        QVariantList pointList = jsonArrayToVariantList(pointArr);
        QVariantList lineList = jsonArrayToVariantList(lineArr);

        auto point = MeasurementData::parsePoint3D(pointList, &error);
        if (!point) {
            emit errorOccurred(tr("点数据格式无效: %1").arg(error));
            return false;
        }
        auto line = MeasurementData::parseLine2D(lineList, &error);
        if (!line) {
            emit errorOccurred(tr("线数据格式无效: %1").arg(error));
            return false;
        }

        output.setData("point", pointList);
        output.setData("line", lineList);
        output.setData("measurement_input_mode", mode);

        Logger::instance().debug(QString("测量输入: point_line mode"), "MeasurementInput");
    }
    else if (mode == "point_plane") {
        QJsonArray pointArr = params["point"].toArray();
        QJsonArray planeArr = params["plane"].toArray();
        QVariantList pointList = jsonArrayToVariantList(pointArr);
        QVariantList planeList = jsonArrayToVariantList(planeArr);

        auto point = MeasurementData::parsePoint3D(pointList, &error);
        if (!point) {
            emit errorOccurred(tr("点数据格式无效: %1").arg(error));
            return false;
        }
        auto plane = MeasurementData::parsePlane3D(planeList, &error);
        if (!plane) {
            emit errorOccurred(tr("平面数据格式无效: %1").arg(error));
            return false;
        }

        output.setData("point", pointList);
        output.setData("plane", planeList);
        output.setData("measurement_input_mode", mode);

        Logger::instance().debug(QString("测量输入: point_plane mode"), "MeasurementInput");
    }
    else if (mode == "custom") {
        // Write all available measurement keys
        bool hasPoint1 = params.contains("point1");
        bool hasPoint2 = params.contains("point2");
        bool hasPoint = params.contains("point");
        bool hasLine = params.contains("line");
        bool hasPlane = params.contains("plane");

        if (hasPoint1) {
            QVariantList point1List = jsonArrayToVariantList(params["point1"].toArray());
            auto p1 = MeasurementData::parsePoint2D(point1List, &error);
            if (!p1) {
                emit errorOccurred(tr("点1数据格式无效: %1").arg(error));
                return false;
            }
            output.setData("point1", point1List);
        }

        if (hasPoint2) {
            QVariantList point2List = jsonArrayToVariantList(params["point2"].toArray());
            auto p2 = MeasurementData::parsePoint2D(point2List, &error);
            if (!p2) {
                emit errorOccurred(tr("点2数据格式无效: %1").arg(error));
                return false;
            }
            output.setData("point2", point2List);
        }

        if (hasPoint) {
            QVariantList pointList = jsonArrayToVariantList(params["point"].toArray());
            auto point = MeasurementData::parsePoint3D(pointList, &error);
            if (!point) {
                emit errorOccurred(tr("点数据格式无效: %1").arg(error));
                return false;
            }
            output.setData("point", pointList);
        }

        if (hasLine) {
            QVariantList lineList = jsonArrayToVariantList(params["line"].toArray());
            auto line = MeasurementData::parseLine2D(lineList, &error);
            if (!line) {
                emit errorOccurred(tr("线数据格式无效: %1").arg(error));
                return false;
            }
            output.setData("line", lineList);
        }

        if (hasPlane) {
            QVariantList planeList = jsonArrayToVariantList(params["plane"].toArray());
            auto plane = MeasurementData::parsePlane3D(planeList, &error);
            if (!plane) {
                emit errorOccurred(tr("平面数据格式无效: %1").arg(error));
                return false;
            }
            output.setData("plane", planeList);
        }

        output.setData("measurement_input_mode", mode);
        Logger::instance().debug(QString("测量输入: custom mode"), "MeasurementInput");
    }
    else {
        emit errorOccurred(tr("未知的测量输入模式: %1").arg(mode));
        return false;
    }

    return true;
}

bool MeasurementInputPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    QString mode = params["mode"].toString();
    if (mode.isEmpty()) {
        error = tr("模式不能为空");
        return false;
    }

    if (mode != "point_pair" && mode != "point_line" && mode != "point_plane" && mode != "custom") {
        error = tr("未知的测量输入模式: %1").arg(mode);
        return false;
    }

    return true;
}

QWidget* MeasurementInputPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("测量输入适配器 - 将配置的测量点/线/面写入管道元数据")));
    layout->addStretch();
    return widget;
}

IModule* MeasurementInputPlugin::cloneImpl() const
{
    MeasurementInputPlugin* clone = new MeasurementInputPlugin();
    return clone;
}

} // namespace DeepLux
