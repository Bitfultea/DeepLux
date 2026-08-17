#include "DataContract.h"

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>

#include "core/common/CancellationToken.h"
#include "core/display/DisplayData.h"
#include "core/model/ImageData.h"

namespace DeepLux {

bool ExecutionContext::isCancelled() const {
    return cancellationToken && cancellationToken->isCancelledFast();
}

QString dataTypeName(DataType type) {
    switch (type) {
    case DataType::Image2D: return QStringLiteral("Image2D");
    case DataType::HeightMap2D: return QStringLiteral("HeightMap2D");
    case DataType::PointCloud3D: return QStringLiteral("PointCloud3D");
    case DataType::Mask2D: return QStringLiteral("Mask2D");
    case DataType::Region2D: return QStringLiteral("Region2D");
    case DataType::Point2D: return QStringLiteral("Point2D");
    case DataType::Point3D: return QStringLiteral("Point3D");
    case DataType::PointSet2D: return QStringLiteral("PointSet2D");
    case DataType::Line2D: return QStringLiteral("Line2D");
    case DataType::Circle2D: return QStringLiteral("Circle2D");
    case DataType::Ellipse2D: return QStringLiteral("Ellipse2D");
    case DataType::Plane3D: return QStringLiteral("Plane3D");
    case DataType::Transform2D: return QStringLiteral("Transform2D");
    case DataType::DetectionList: return QStringLiteral("DetectionList");
    case DataType::ClassScores: return QStringLiteral("ClassScores");
    case DataType::Number: return QStringLiteral("Number");
    case DataType::Integer: return QStringLiteral("Integer");
    case DataType::Boolean: return QStringLiteral("Boolean");
    case DataType::String: return QStringLiteral("String");
    case DataType::Binary: return QStringLiteral("Binary");
    case DataType::Table: return QStringLiteral("Table");
    case DataType::Any: return QStringLiteral("Any");
    }
    return QStringLiteral("Unknown");
}

bool dataTypeFromString(const QString& name, DataType& out) {
    static const QHash<QString, DataType> map = {
        {QStringLiteral("Image2D"), DataType::Image2D},
        {QStringLiteral("HeightMap2D"), DataType::HeightMap2D},
        {QStringLiteral("PointCloud3D"), DataType::PointCloud3D},
        {QStringLiteral("Mask2D"), DataType::Mask2D},
        {QStringLiteral("Region2D"), DataType::Region2D},
        {QStringLiteral("Point2D"), DataType::Point2D},
        {QStringLiteral("Point3D"), DataType::Point3D},
        {QStringLiteral("PointSet2D"), DataType::PointSet2D},
        {QStringLiteral("Line2D"), DataType::Line2D},
        {QStringLiteral("Circle2D"), DataType::Circle2D},
        {QStringLiteral("Ellipse2D"), DataType::Ellipse2D},
        {QStringLiteral("Plane3D"), DataType::Plane3D},
        {QStringLiteral("Transform2D"), DataType::Transform2D},
        {QStringLiteral("DetectionList"), DataType::DetectionList},
        {QStringLiteral("ClassScores"), DataType::ClassScores},
        {QStringLiteral("Number"), DataType::Number},
        {QStringLiteral("Integer"), DataType::Integer},
        {QStringLiteral("Boolean"), DataType::Boolean},
        {QStringLiteral("String"), DataType::String},
        {QStringLiteral("Binary"), DataType::Binary},
        {QStringLiteral("Table"), DataType::Table},
        {QStringLiteral("Any"), DataType::Any},
    };
    auto it = map.constFind(name);
    if (it == map.constEnd())
        return false;
    out = it.value();
    return true;
}

namespace {

bool isNumeric(const QVariant& value) {
    switch (value.type()) {
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
    case QVariant::Double:
        return true;
    default:
        return false;
    }
}

bool isNumericList(const QVariant& value, int expectedSize) {
    if (value.type() != QVariant::List)
        return false;
    const QVariantList values = value.toList();
    if (values.size() != expectedSize)
        return false;
    for (const QVariant& item : values) {
        if (!isNumeric(item))
            return false;
    }
    return true;
}

} // namespace

bool portValueMatchesType(const QVariant& value, DataType type) {
    if (!value.isValid())
        return false;

    if (type == DataType::Any)
        return true;

    switch (type) {
    case DataType::Image2D:
    case DataType::HeightMap2D:
        return value.canConvert<ImageData>();
    case DataType::PointCloud3D:
        if (value.canConvert<PointCloudData>())
            return true;
        return value.canConvert<ImageData>() && value.value<ImageData>().hasData(QStringLiteral("point_cloud"));
    case DataType::Point2D:
        return value.type() == QVariant::PointF || isNumericList(value, 2);
    case DataType::Point3D:
        return isNumericList(value, 3);
    case DataType::PointSet2D:
        return value.canConvert<QVector<QPointF>>() || value.type() == QVariant::List;
    case DataType::Line2D:
        return isNumericList(value, 4);
    case DataType::Plane3D:
        return isNumericList(value, 9);
    case DataType::Number:
        return isNumeric(value);
    case DataType::Integer:
        return value.type() == QVariant::Int || value.type() == QVariant::UInt || value.type() == QVariant::LongLong ||
               value.type() == QVariant::ULongLong;
    case DataType::Boolean:
        return value.type() == QVariant::Bool;
    case DataType::String:
        return value.type() == QVariant::String;
    case DataType::Binary:
        return value.type() == QVariant::ByteArray;
    case DataType::Table:
        return value.type() == QVariant::Map || value.type() == QVariant::List;
    case DataType::Mask2D:
    case DataType::Region2D:
    case DataType::Circle2D:
    case DataType::Ellipse2D:
    case DataType::Transform2D:
    case DataType::DetectionList:
    case DataType::ClassScores:
        return true; // Dedicated payload types are introduced with their producing plugins.
    case DataType::Any:
        return true;
    }
    return false;
}

void registerDataContractMetaTypes() {
    static bool registered = false;
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    if (registered)
        return;
    registered = true;

    qRegisterMetaType<DataType>("DeepLux::DataType");
    qRegisterMetaType<PortSpec>("DeepLux::PortSpec");
    qRegisterMetaType<PortValueMap>("DeepLux::PortValueMap");
    qRegisterMetaType<ExecutionResult>("DeepLux::ExecutionResult");
}

} // namespace DeepLux
