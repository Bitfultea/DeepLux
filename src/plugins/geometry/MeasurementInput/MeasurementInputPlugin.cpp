#include "MeasurementInputPlugin.h"

#include "common/Logger.h"
#include "core/geometry/MeasurementData.h"

#include <QComboBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QRegExp>
#include <QVBoxLayout>

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
    const QStringList parts = text.split(QRegExp("[,\\s]+"), Qt::SkipEmptyParts);
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
                                  {"plane", makeJsonArray({0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0})}};
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
    } else {
        emit errorOccurred(tr("未知的测量输入模式: %1").arg(mode));
        return false;
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
        mode != "custom") {
        error = tr("未知的测量输入模式: %1").arg(mode);
        return false;
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
    modeCombo->addItems({"point_pair", "point_line", "line_pair", "point_plane", "custom"});
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
