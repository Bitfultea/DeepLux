#include "PlyLoader.h"

#include <QDataStream>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <limits>

namespace DeepLux {
namespace {

using ScalarType = PlyLoader::Property::ScalarType;

bool parseScalarType(const QString& token, ScalarType& type) {
    if (token == QLatin1String("float") || token == QLatin1String("float32"))
        type = ScalarType::Float32;
    else if (token == QLatin1String("double") || token == QLatin1String("float64"))
        type = ScalarType::Float64;
    else if (token == QLatin1String("uchar") || token == QLatin1String("uint8"))
        type = ScalarType::UInt8;
    else if (token == QLatin1String("int") || token == QLatin1String("int32"))
        type = ScalarType::Int32;
    else
        return false;
    return true;
}

bool readAsciiScalar(const QString& token, ScalarType type, double& value) {
    bool ok = false;
    switch (type) {
    case ScalarType::Float32:
    case ScalarType::Float64:
        value = token.toDouble(&ok);
        if (!ok || !std::isfinite(value))
            return false;
        if (type == ScalarType::Float32) {
            if (std::abs(value) > std::numeric_limits<float>::max())
                return false;
            value = static_cast<float>(value);
        }
        return std::isfinite(value);
    case ScalarType::UInt8: {
        const uint parsed = token.toUInt(&ok);
        if (!ok || parsed > 255)
            return false;
        value = parsed;
        return true;
    }
    case ScalarType::Int32: {
        const qlonglong parsed = token.toLongLong(&ok);
        if (!ok || parsed < std::numeric_limits<qint32>::min() || parsed > std::numeric_limits<qint32>::max())
            return false;
        value = parsed;
        return true;
    }
    }
    return false;
}

bool readBinaryScalar(QDataStream& stream, ScalarType type, double& value) {
    switch (type) {
    case ScalarType::Float32: {
        stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
        float parsed = 0;
        stream >> parsed;
        value = parsed;
        break;
    }
    case ScalarType::Float64: {
        stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
        double parsed = 0;
        stream >> parsed;
        value = parsed;
        break;
    }
    case ScalarType::UInt8: {
        quint8 parsed = 0;
        stream >> parsed;
        value = parsed;
        break;
    }
    case ScalarType::Int32: {
        qint32 parsed = 0;
        stream >> parsed;
        value = parsed;
        break;
    }
    }
    return stream.status() == QDataStream::Ok && std::isfinite(value);
}

bool isInteger(ScalarType type) {
    return type == ScalarType::UInt8 || type == ScalarType::Int32;
}

qint64 scalarSize(ScalarType type) {
    switch (type) {
    case ScalarType::Float32:
    case ScalarType::Int32:
        return 4;
    case ScalarType::Float64:
        return 8;
    case ScalarType::UInt8:
        return 1;
    }
    return 0;
}

void assignVertexProperty(const QString& propertyName, double value, bool isFloat, Eigen::Vector3d& point,
                          Eigen::Vector3d& normal, Eigen::Vector3d& color, bool& hasNormal, bool& hasColor) {
    const QString name = propertyName.toLower();
    if (name == QLatin1String("x"))
        point.x() = value;
    else if (name == QLatin1String("y"))
        point.y() = value;
    else if (name == QLatin1String("z"))
        point.z() = value;
    else if (name == QLatin1String("nx")) {
        normal.x() = value;
        hasNormal = true;
    } else if (name == QLatin1String("ny")) {
        normal.y() = value;
        hasNormal = true;
    } else if (name == QLatin1String("nz")) {
        normal.z() = value;
        hasNormal = true;
    } else if (name == QLatin1String("red") || name == QLatin1String("r")) {
        color.x() = isFloat ? value : value / 255.0;
        hasColor = true;
    } else if (name == QLatin1String("green") || name == QLatin1String("g")) {
        color.y() = isFloat ? value : value / 255.0;
        hasColor = true;
    } else if (name == QLatin1String("blue") || name == QLatin1String("b")) {
        color.z() = isFloat ? value : value / 255.0;
        hasColor = true;
    }
}

void appendVertex(PointCloudData& out, const Eigen::Vector3d& point, const Eigen::Vector3d& normal,
                  const Eigen::Vector3d& color, bool hasNormal, bool hasColor) {
    out.points.push_back(point);
    if (hasNormal)
        out.normals.push_back(normal);
    if (hasColor)
        out.colors.push_back(color);
}

bool validateAsciiOtherRecord(const QStringList& tokens, const QList<PlyLoader::Property>& properties) {
    int tokenIndex = 0;
    for (const PlyLoader::Property& property : properties) {
        double value = 0;
        if (tokenIndex >= tokens.size() || !readAsciiScalar(tokens[tokenIndex++], property.type, value))
            return false;
        if (property.isList) {
            if (!isInteger(property.type) || value < 0 || value > tokens.size() - tokenIndex)
                return false;
            const int itemCount = static_cast<int>(value);
            for (int i = 0; i < itemCount; ++i) {
                if (!readAsciiScalar(tokens[tokenIndex++], property.itemType, value))
                    return false;
            }
        }
    }
    return tokenIndex == tokens.size();
}

bool readBinaryOtherRecord(QDataStream& stream, QFile& file, const QList<PlyLoader::Property>& properties) {
    for (const PlyLoader::Property& property : properties) {
        double value = 0;
        if (!readBinaryScalar(stream, property.type, value))
            return false;
        if (property.isList) {
            if (!isInteger(property.type) || value < 0 || value > std::numeric_limits<int>::max())
                return false;
            const qint64 itemBytes = scalarSize(property.itemType);
            const qint64 remainingBytes = file.size() - file.pos();
            if (itemBytes <= 0 || value > remainingBytes / itemBytes)
                return false;
            const int itemCount = static_cast<int>(value);
            for (int i = 0; i < itemCount; ++i) {
                if (!readBinaryScalar(stream, property.itemType, value))
                    return false;
            }
        }
    }
    return true;
}

} // namespace

bool PlyLoader::load(const QString& filePath, PointCloudData& outData, QString& errorMsg) {
    outData.clear();
    errorMsg.clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        errorMsg = QString("Cannot open PLY file: %1").arg(filePath);
        return false;
    }

    QTextStream stream(&file);
    if (stream.readLine().trimmed() != QLatin1String("ply")) {
        errorMsg = QStringLiteral("Not a valid PLY file (missing 'ply' header)");
        return false;
    }

    QList<Element> elements;
    bool isBinary = false;
    if (!parseHeader(stream, elements, isBinary, errorMsg))
        return false;

    const qint64 dataStart = stream.pos();
    if (dataStart < 0) {
        errorMsg = QStringLiteral("Cannot locate PLY vertex data");
        return false;
    }

    const bool ok = isBinary ? parseBinary(file, dataStart, elements, outData, errorMsg)
                             : parseAscii(stream, elements, outData, errorMsg);
    if (!ok)
        outData.clear();
    return ok;
}

bool PlyLoader::parseHeader(QTextStream& stream, QList<Element>& elements, bool& isBinary, QString& errorMsg) {
    bool sawFormat = false;
    bool sawEndHeader = false;
    QSet<QString> elementNames;
    QSet<QString> propertyNames;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line == QLatin1String("end_header")) {
            sawEndHeader = true;
            break;
        }
        if (line.startsWith(QLatin1String("comment ")) || line.startsWith(QLatin1String("obj_info ")) || line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("format "))) {
            const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (parts.size() != 3 || parts[2] != QLatin1String("1.0") || sawFormat || !elements.isEmpty()) {
                errorMsg = QStringLiteral("Unsupported or malformed PLY format declaration");
                return false;
            }
            if (parts[1] == QLatin1String("ascii"))
                isBinary = false;
            else if (parts[1] == QLatin1String("binary_little_endian"))
                isBinary = true;
            else {
                errorMsg = QStringLiteral("Only ASCII and little-endian binary PLY are supported");
                return false;
            }
            sawFormat = true;
            continue;
        }
        if (line.startsWith(QLatin1String("element "))) {
            const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            bool ok = false;
            const int count = parts.size() == 3 ? parts[2].toInt(&ok) : -1;
            const QString name = parts.value(1);
            if (parts.size() != 3 || !ok || count < 0 || elementNames.contains(name)) {
                errorMsg = QStringLiteral("Malformed or duplicate PLY element declaration");
                return false;
            }
            if (elements.isEmpty() && name != QLatin1String("vertex")) {
                errorMsg = QStringLiteral("PLY vertex element must be first");
                return false;
            }
            if (name == QLatin1String("vertex") && (count <= 0 || !elements.isEmpty())) {
                errorMsg = QStringLiteral("PLY vertex element must be first and have a positive count");
                return false;
            }
            elements.append(Element{name, count, {}});
            elementNames.insert(name);
            propertyNames.clear();
            continue;
        }
        if (line.startsWith(QLatin1String("property "))) {
            if (elements.isEmpty()) {
                errorMsg = QStringLiteral("PLY property appears before an element declaration");
                return false;
            }
            const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            Property property;
            if (elements.last().name == QLatin1String("vertex")) {
                if (parts.size() != 3 || !parseScalarType(parts[1], property.type)) {
                    errorMsg = QStringLiteral("Unsupported or malformed PLY vertex property");
                    return false;
                }
                property.name = parts[2];
            } else if (parts.size() == 3 && parseScalarType(parts[1], property.type)) {
                property.name = parts[2];
            } else if (parts.size() == 5 && parts[1] == QLatin1String("list") &&
                       parseScalarType(parts[2], property.type) && isInteger(property.type) &&
                       parseScalarType(parts[3], property.itemType)) {
                property.isList = true;
                property.name = parts[4];
            } else {
                errorMsg = QStringLiteral("Unsupported or malformed PLY property declaration");
                return false;
            }
            if (propertyNames.contains(property.name.toLower())) {
                errorMsg = QStringLiteral("Duplicate PLY property declaration");
                return false;
            }
            propertyNames.insert(property.name.toLower());
            elements.last().properties.append(property);
            continue;
        }
        errorMsg = QStringLiteral("Unsupported PLY header declaration");
        return false;
    }

    if (!sawFormat || !sawEndHeader || elements.isEmpty() || elements.first().name != QLatin1String("vertex") ||
        elements.first().properties.isEmpty()) {
        errorMsg = QStringLiteral("PLY header is incomplete");
        return false;
    }
    const QSet<QString> vertexProperties = [&]() {
        QSet<QString> names;
        for (const Property& property : elements.first().properties)
            names.insert(property.name.toLower());
        return names;
    }();
    if (!vertexProperties.contains(QStringLiteral("x")) || !vertexProperties.contains(QStringLiteral("y")) ||
        !vertexProperties.contains(QStringLiteral("z"))) {
        errorMsg = QStringLiteral("PLY vertex properties must include x, y and z");
        return false;
    }
    return true;
}

bool PlyLoader::parseAscii(QTextStream& stream, const QList<Element>& elements, PointCloudData& out,
                           QString& errorMsg) {
    for (const Element& element : elements) {
        for (int i = 0; i < element.count; ++i) {
            QString line;
            do {
                if (stream.atEnd()) {
                    errorMsg = QStringLiteral("PLY element data is truncated");
                    return false;
                }
                line = stream.readLine().trimmed();
            } while (line.isEmpty());

            const QStringList tokens = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (element.name != QLatin1String("vertex")) {
                if (!validateAsciiOtherRecord(tokens, element.properties)) {
                    errorMsg = QStringLiteral("PLY element record is malformed or truncated");
                    return false;
                }
                continue;
            }
            if (tokens.size() != element.properties.size()) {
                errorMsg = QStringLiteral("PLY vertex record has an unexpected number of values");
                return false;
            }
            Eigen::Vector3d point(0, 0, 0), normal(0, 0, 0), color(0.5, 0.5, 0.5);
            bool hasNormal = false, hasColor = false;
            for (int j = 0; j < element.properties.size(); ++j) {
                const Property& property = element.properties[j];
                double value = 0;
                if (property.isList || !readAsciiScalar(tokens[j], property.type, value)) {
                    errorMsg = QStringLiteral("PLY vertex record contains an invalid scalar");
                    return false;
                }
                assignVertexProperty(property.name, value, property.isFloat(), point, normal, color, hasNormal,
                                     hasColor);
            }
            appendVertex(out, point, normal, color, hasNormal, hasColor);
        }
    }
    while (!stream.atEnd()) {
        if (!stream.readLine().trimmed().isEmpty()) {
            errorMsg = QStringLiteral("PLY contains undeclared trailing data");
            return false;
        }
    }
    return true;
}

bool PlyLoader::parseBinary(QFile& file, qint64 dataStart, const QList<Element>& elements, PointCloudData& out,
                            QString& errorMsg) {
    if (!file.seek(dataStart)) {
        errorMsg = QStringLiteral("Cannot seek to PLY vertex data");
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);

    for (const Element& element : elements) {
        for (int i = 0; i < element.count; ++i) {
            if (element.name != QLatin1String("vertex")) {
                if (!readBinaryOtherRecord(stream, file, element.properties)) {
                    errorMsg = QStringLiteral("PLY binary element record is malformed or truncated");
                    return false;
                }
                continue;
            }
            Eigen::Vector3d point(0, 0, 0), normal(0, 0, 0), color(0.5, 0.5, 0.5);
            bool hasNormal = false, hasColor = false;
            for (const Property& property : element.properties) {
                double value = 0;
                if (property.isList || !readBinaryScalar(stream, property.type, value)) {
                    errorMsg = QStringLiteral("PLY binary vertex data is truncated or invalid");
                    return false;
                }
                assignVertexProperty(property.name, value, property.isFloat(), point, normal, color, hasNormal,
                                     hasColor);
            }
            appendVertex(out, point, normal, color, hasNormal, hasColor);
        }
    }
    if (!file.atEnd()) {
        errorMsg = QStringLiteral("PLY contains undeclared trailing binary data");
        return false;
    }
    return true;
}

} // namespace DeepLux
