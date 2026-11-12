#include "MeasurementInputPlugin.h"

#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"

#include <QComboBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPointF>
#include <QPointer>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace DeepLux {

static QJsonArray makeJsonArray(std::initializer_list<double> vals) {
    QJsonArray arr;
    for (double v : vals) {
        arr.append(v);
    }
    return arr;
}

static QString arrayText(const QJsonArray& arr) {
    QStringList parts;
    for (const QJsonValue& value : arr) {
        parts.append(QString::number(value.toDouble(), 'g', 12));
    }
    return parts.join(',');
}

static bool parseArrayText(const QString& text, QJsonArray& out) {
    const QStringList parts = text.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }

    QJsonArray arr;
    for (const QString& part : parts) {
        bool ok = false;
        const double value = part.toDouble(&ok);
        if (!ok) {
            return false;
        }
        arr.append(value);
    }
    out = arr;
    return true;
}

MeasurementInputPlugin::MeasurementInputPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"mode", "point_pair"},
                                  {"point1", makeJsonArray({0.0, 0.0})},
                                  {"point2", makeJsonArray({0.0, 0.0})},
                                  {"point", makeJsonArray({0.0, 0.0, 0.0})},
                                  {"line", makeJsonArray({0.0, 0.0, 100.0, 0.0})},
                                  {"line1", makeJsonArray({0.0, 0.0, 100.0, 0.0})},
                                  {"line2", makeJsonArray({0.0, 100.0, 100.0, 100.0})},
                                  {"plane", makeJsonArray({0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0})},
                                  {"points", QJsonArray()},
                                  {"pick_coordinate_frame", QStringLiteral("Unspecified")}};
    m_params = m_defaultParams;
}

MeasurementInputPlugin::~MeasurementInputPlugin() {}

bool MeasurementInputPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "MeasurementInputPlugin initialized";
    return true;
}

void MeasurementInputPlugin::shutdown() {
    ModuleBase::shutdown();
}

QVariantList MeasurementInputPlugin::jsonArrayToVariantList(const QJsonArray& arr) {
    QVariantList list;
    for (const auto& val : arr) {
        list.append(val.toDouble());
    }
    return list;
}

bool MeasurementInputPlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    QJsonObject params = currentParams();
    QString mode = params["mode"].toString();

    QString error;

    if (mode == "point_pair") {
        QJsonArray point1Arr = params["point1"].toArray();
        QJsonArray point2Arr = params["point2"].toArray();
        const auto isFinitePoint = [](const QJsonArray& point) {
            if (point.size() != 2 && point.size() != 3)
                return false;
            return std::all_of(point.cbegin(), point.cend(), [](const QJsonValue& coordinate) {
                return coordinate.isDouble() && std::isfinite(coordinate.toDouble());
            });
        };
        if (!isFinitePoint(point1Arr) || !isFinitePoint(point2Arr)) {
            emit errorOccurred(tr("两点必须分别包含两个或三个有限数值"));
            return false;
        }
        if (point1Arr.size() != point2Arr.size()) {
            emit errorOccurred(tr("两点坐标维数必须一致"));
            return false;
        }
        QVariantList point1List = jsonArrayToVariantList(point1Arr);
        QVariantList point2List = jsonArrayToVariantList(point2Arr);

        auto p1 = MeasurementData::parsePoint3D(point1List, &error);
        if (!p1) {
            emit errorOccurred(tr("点1数据格式无效: %1").arg(error));
            return false;
        }
        auto p2 = MeasurementData::parsePoint3D(point2List, &error);
        if (!p2) {
            emit errorOccurred(tr("点2数据格式无效: %1").arg(error));
            return false;
        }

        output.setData("point1", point1List);
        output.setData("point2", point2List);
        output.setData("measurement_input_mode", mode);

        Logger::instance().debug(QString("测量输入: point_pair mode"), "MeasurementInput");
    } else if (mode == "point_line") {
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
    } else if (mode == "line_pair") {
        QJsonArray line1Arr = params["line1"].toArray();
        QJsonArray line2Arr = params["line2"].toArray();
        QVariantList line1List = jsonArrayToVariantList(line1Arr);
        QVariantList line2List = jsonArrayToVariantList(line2Arr);

        auto line1 = MeasurementData::parseLine2D(line1List, &error);
        if (!line1) {
            emit errorOccurred(tr("线1数据格式无效: %1").arg(error));
            return false;
        }
        auto line2 = MeasurementData::parseLine2D(line2List, &error);
        if (!line2) {
            emit errorOccurred(tr("线2数据格式无效: %1").arg(error));
            return false;
        }

        output.setData("line1", line1List);
        output.setData("line2", line2List);
        output.setData("measurement_input_mode", mode);

        Logger::instance().debug(QString("测量输入: line_pair mode"), "MeasurementInput");
    } else if (mode == "point_plane") {
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
    } else if (mode == "custom") {
        // Write all available measurement keys
        bool hasPoint1 = params.contains("point1");
        bool hasPoint2 = params.contains("point2");
        bool hasPoint = params.contains("point");
        bool hasLine = params.contains("line");
        bool hasPlane = params.contains("plane");
        bool hasLine1 = params.contains("line1");
        bool hasLine2 = params.contains("line2");

        if (hasPoint1) {
            QVariantList point1List = jsonArrayToVariantList(params["point1"].toArray());
            auto p1 = MeasurementData::parsePoint3D(point1List, &error);
            if (!p1) {
                emit errorOccurred(tr("点1数据格式无效: %1").arg(error));
                return false;
            }
            output.setData("point1", point1List);
        }

        if (hasPoint2) {
            QVariantList point2List = jsonArrayToVariantList(params["point2"].toArray());
            auto p2 = MeasurementData::parsePoint3D(point2List, &error);
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

        if (hasLine1) {
            QVariantList line1List = jsonArrayToVariantList(params["line1"].toArray());
            auto line1 = MeasurementData::parseLine2D(line1List, &error);
            if (!line1) {
                emit errorOccurred(tr("线1数据格式无效: %1").arg(error));
                return false;
            }
            output.setData("line1", line1List);
        }

        if (hasLine2) {
            QVariantList line2List = jsonArrayToVariantList(params["line2"].toArray());
            auto line2 = MeasurementData::parseLine2D(line2List, &error);
            if (!line2) {
                emit errorOccurred(tr("线2数据格式无效: %1").arg(error));
                return false;
            }
            output.setData("line2", line2List);
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
    } else if (mode == "point_set") {
        // 点集输入：points 为 [[x,y],[x,y],...] 数组，输出 fit_points 供拟合插件使用
        QJsonArray pointsArr = params["points"].toArray();
        if (pointsArr.isEmpty()) {
            emit errorOccurred(tr("point_set 模式需要提供非空 points 数组"));
            return false;
        }
        // 输出 QVector<QPointF>，与拟合插件 canConvert<QVector<QPointF>> 分支对齐
        QVector<QPointF> pointSet;
        for (const auto& ptVal : pointsArr) {
            QJsonArray ptArr = ptVal.toArray();
            if (ptArr.size() < 2) {
                emit errorOccurred(tr("points 中每个点至少需要 [x, y] 两个坐标"));
                return false;
            }
            pointSet.append(QPointF(ptArr[0].toDouble(), ptArr[1].toDouble()));
        }
        output.setData("fit_points", QVariant::fromValue(pointSet));
        output.setData("measurement_input_mode", mode);
        Logger::instance().debug(QString("测量输入: point_set mode, %1 points").arg(pointSet.size()),
                                 "MeasurementInput");
    } else {
        emit errorOccurred(tr("未知的测量输入模式: %1").arg(mode));
        return false;
    }

    const bool pointPair2D = mode == QStringLiteral("point_pair") && params["point1"].toArray().size() == 2;
    const bool pointPair3D = mode == QStringLiteral("point_pair") && params["point1"].toArray().size() == 3;
    const QJsonValue pickCoordinateFrame = params.value(QStringLiteral("pick_coordinate_frame"));
    const bool pointLineImageCoordinates = mode == QStringLiteral("point_line") && pickCoordinateFrame.isString() &&
                                           pickCoordinateFrame.toString() == QStringLiteral("ImageLocalPixel");
    const bool linePairImageCoordinates = mode == QStringLiteral("line_pair") && pickCoordinateFrame.isString() &&
                                          pickCoordinateFrame.toString() == QStringLiteral("ImageLocalPixel");
    const bool imageCoordinateMode =
        pointPair2D || pointLineImageCoordinates || linePairImageCoordinates || mode == QStringLiteral("point_set");
    output.setData(QStringLiteral("coordinate_frame"), imageCoordinateMode ? QStringLiteral("ImageLocalPixel")
                                                       : (pointPair3D || mode == QStringLiteral("point_plane"))
                                                           ? QStringLiteral("World3D")
                                                           : QStringLiteral("Unspecified"));
    const bool sourceAvailable = imageCoordinateMode && input.hasCoordinateProvenance();
    output.setData(QStringLiteral("source_coordinate_available"), sourceAvailable);
    if (sourceAvailable) {
        const ImageCoordinateProvenance provenance = input.coordinateProvenance();
        output.setData(QStringLiteral("source_image_id"), provenance.sourceImageId);
        output.setData(QStringLiteral("source_image_size"),
                       QVariantList{provenance.sourceSize.width(), provenance.sourceSize.height()});
        if (const auto origin = provenance.mapLocalToSource(QPointF()))
            output.setData(QStringLiteral("source_coordinate_origin"), QVariantList{origin->x(), origin->y()});
        const QStringList pointKeys{QStringLiteral("point1"), QStringLiteral("point2"), QStringLiteral("point")};
        for (const QString& key : pointKeys) {
            const QJsonArray point = params.value(key).toArray();
            if (point.size() >= 2) {
                if (const auto mapped =
                        provenance.mapLocalToSource(QPointF(point[0].toDouble(), point[1].toDouble()))) {
                    QVariantList sourcePoint{mapped->x(), mapped->y()};
                    for (qsizetype i = 2; i < point.size(); ++i)
                        sourcePoint.append(point[i].toVariant());
                    output.setData(key + QStringLiteral("_source"), sourcePoint);
                }
            }
        }
        const QStringList lineKeys{QStringLiteral("line"), QStringLiteral("line1"), QStringLiteral("line2")};
        for (const QString& key : lineKeys) {
            const QJsonArray line = params.value(key).toArray();
            if (line.size() == 4) {
                const auto first = provenance.mapLocalToSource(QPointF(line[0].toDouble(), line[1].toDouble()));
                const auto second = provenance.mapLocalToSource(QPointF(line[2].toDouble(), line[3].toDouble()));
                if (first && second) {
                    output.setData(key + QStringLiteral("_source"),
                                   QVariantList{first->x(), first->y(), second->x(), second->y()});
                }
            }
        }
        const QJsonArray pointSet = params.value(QStringLiteral("points")).toArray();
        if (!pointSet.isEmpty()) {
            QVariantList sourcePoints;
            for (const QJsonValue& pointValue : pointSet) {
                const QJsonArray point = pointValue.toArray();
                if (point.size() != 2) {
                    sourcePoints.append(QVariant());
                    continue;
                }
                const auto mapped = provenance.mapLocalToSource(QPointF(point[0].toDouble(), point[1].toDouble()));
                sourcePoints.append(mapped ? QVariant(QVariantList{mapped->x(), mapped->y()}) : QVariant());
            }
            output.setData(QStringLiteral("fit_points_source"), sourcePoints);
        }
    }

    return true;
}

bool MeasurementInputPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    QString mode = params["mode"].toString();
    if (mode.isEmpty()) {
        error = tr("模式不能为空");
        return false;
    }

    if (mode != "point_pair" && mode != "point_line" && mode != "line_pair" && mode != "point_plane" &&
        mode != "custom" && mode != "point_set") {
        error = tr("未知的测量输入模式: %1").arg(mode);
        return false;
    }

    const auto numericArray = [&params, &error](const QString& key, int minSize, int maxSize) {
        const QJsonValue value = params.value(key);
        if (!value.isArray() || value.toArray().size() < minSize || value.toArray().size() > maxSize) {
            error = QString("%1 must contain %2..%3 numeric coordinates").arg(key).arg(minSize).arg(maxSize);
            return false;
        }
        for (const QJsonValue& coordinate : value.toArray()) {
            if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble())) {
                error = QString("%1 coordinates must be finite numbers").arg(key);
                return false;
            }
        }
        return true;
    };
    if (!numericArray("point1", 2, 3) || !numericArray("point2", 2, 3) || !numericArray("point", 2, 3) ||
        !numericArray("line", 4, 4) || !numericArray("line1", 4, 4) || !numericArray("line2", 4, 4) ||
        !numericArray("plane", 9, 9))
        return false;
    const QJsonValue pointsValue = params.value(QStringLiteral("points"));
    if (!pointsValue.isArray() || pointsValue.toArray().size() > 1024) {
        error = QStringLiteral("points must be an array of at most 1024 items");
        return false;
    }
    for (const QJsonValue& pointValue : pointsValue.toArray()) {
        const QJsonArray point = pointValue.toArray();
        if (!pointValue.isArray() || point.size() != 2 || !point[0].isDouble() || !point[1].isDouble() ||
            !std::isfinite(point[0].toDouble()) || !std::isfinite(point[1].toDouble())) {
            error = QStringLiteral("points items must be finite [x,y] pairs");
            return false;
        }
    }
    if (mode == QStringLiteral("point_plane")) {
        QString planeError;
        if (!MeasurementData::parsePlane3D(jsonArrayToVariantList(params.value("plane").toArray()), &planeError)) {
            error = planeError;
            return false;
        }
    }

    return true;
}

QWidget* MeasurementInputPlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    layout->addWidget(new QLabel(tr("测量输入适配器 - 将配置的测量点/线/面写入管道元数据")));

    QFormLayout* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    QComboBox* modeCombo = new QComboBox(widget);
    modeCombo->setObjectName("MeasurementInputModeCombo");
    modeCombo->addItems({"point_pair", "point_line", "line_pair", "point_plane", "custom", "point_set"});
    modeCombo->setCurrentText(m_params["mode"].toString("point_pair"));
    form->addRow(tr("模式"), modeCombo);

    auto makeEdit = [widget](const QString& objectName, const QJsonArray& value) {
        QLineEdit* edit = new QLineEdit(arrayText(value), widget);
        edit->setObjectName(objectName);
        edit->setPlaceholderText(QStringLiteral("1,2 或 1,2,3"));
        return edit;
    };

    QLineEdit* point1Edit = makeEdit("MeasurementInputPoint1Edit", m_params["point1"].toArray());
    QLineEdit* point2Edit = makeEdit("MeasurementInputPoint2Edit", m_params["point2"].toArray());
    QLineEdit* pointEdit = makeEdit("MeasurementInputPointEdit", m_params["point"].toArray());
    QLineEdit* lineEdit = makeEdit("MeasurementInputLineEdit", m_params["line"].toArray());
    QLineEdit* line1Edit = makeEdit("MeasurementInputLine1Edit", m_params["line1"].toArray());
    QLineEdit* line2Edit = makeEdit("MeasurementInputLine2Edit", m_params["line2"].toArray());
    QLineEdit* planeEdit = makeEdit("MeasurementInputPlaneEdit", m_params["plane"].toArray());

    form->addRow(tr("点 1"), point1Edit);
    form->addRow(tr("点 2"), point2Edit);
    form->addRow(tr("点"), pointEdit);
    form->addRow(tr("线"), lineEdit);
    form->addRow(tr("线 1"), line1Edit);
    form->addRow(tr("线 2"), line2Edit);
    form->addRow(tr("平面"), planeEdit);
    layout->addLayout(form);

    QPointer<MeasurementInputPlugin> pluginPtr(this);
    connect(modeCombo, &QComboBox::currentTextChanged, this, [pluginPtr](const QString& mode) {
        if (pluginPtr) {
            pluginPtr->setParam("mode", mode);
        }
    });

    auto bindArrayEdit = [this, pluginPtr](QLineEdit* edit, const QString& key) {
        connect(edit, &QLineEdit::textChanged, this, [pluginPtr, key](const QString& text) {
            if (!pluginPtr)
                return;
            QJsonArray arr;
            if (parseArrayText(text, arr)) {
                pluginPtr->setParam(key, arr);
            }
        });
    };

    bindArrayEdit(point1Edit, "point1");
    bindArrayEdit(point2Edit, "point2");
    bindArrayEdit(pointEdit, "point");
    bindArrayEdit(lineEdit, "line");
    bindArrayEdit(line1Edit, "line1");
    bindArrayEdit(line2Edit, "line2");
    bindArrayEdit(planeEdit, "plane");

    layout->addStretch();
    return widget;
}

IModule* MeasurementInputPlugin::cloneImpl() const {
    MeasurementInputPlugin* clone = new MeasurementInputPlugin();
    return clone;
}

} // namespace DeepLux
