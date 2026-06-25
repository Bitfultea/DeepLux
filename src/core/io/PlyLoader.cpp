#include "PlyLoader.h"

#include <QDebug>
#include <QFile>
#include <QTextStream>

namespace DeepLux {

bool PlyLoader::load(const QString& filePath, PointCloudData& outData, QString& errorMsg) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        errorMsg = QString("Cannot open PLY file: %1").arg(filePath);
        return false;
    }

    QTextStream stream(&file);
    QString line = stream.readLine();
    if (line.trimmed() != "ply") {
        errorMsg = "Not a valid PLY file (missing 'ply' header)";
        return false;
    }

    int vertexCount = 0;
    QList<Property> properties;

    bool isBinary = false;
    if (!parseHeader(stream, vertexCount, properties, isBinary, errorMsg)) {
        return false;
    }

    if (vertexCount <= 0) {
        errorMsg = "PLY file has no vertices";
        return false;
    }

    qint64 dataStart = file.pos();

    outData.clear();
    outData.points.reserve(vertexCount);

    if (isBinary) {
        file.close(); // reopen in binary mode below
        return parseBinary(file, dataStart, vertexCount, properties, outData);
    } else {
        return parseAscii(stream, vertexCount, properties, outData);
    }
}

bool PlyLoader::parseHeader(QTextStream& stream, int& vertexCount, QList<Property>& props, bool& isBinary,
                            QString& errorMsg) {
    bool inVertex = false;
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line == "end_header")
            break;
        if (line.startsWith("format ")) {
            isBinary = line.contains("binary_little_endian");
            continue;
        }
        if (line.startsWith("element vertex ")) {
            vertexCount = line.mid(15).toInt();
            inVertex = true;
        } else if (line.startsWith("element ")) {
            inVertex = false;
        } else if (inVertex && line.startsWith("property ")) {
            QString rest = line.mid(9).trimmed();
            Property prop;
            if (rest.startsWith("float") || rest.startsWith("float32")) {
                prop.isFloat = true;
                prop.name = rest.split(' ').last();
            } else if (rest.startsWith("double") || rest.startsWith("float64")) {
                prop.isFloat = true;
                prop.name = rest.split(' ').last();
            } else if (rest.startsWith("uchar") || rest.startsWith("uint8")) {
                prop.isFloat = false;
                prop.name = rest.split(' ').last();
            } else if (rest.startsWith("int")) {
                prop.isFloat = true;
                prop.name = rest.split(' ').last();
            } else {
                // skip unknown property type
                prop.name = rest.split(' ').last();
                prop.isFloat = true;
            }
            props.append(prop);
        }
    }
    return vertexCount > 0;
}

bool PlyLoader::parseAscii(QTextStream& stream, int vertexCount, const QList<Property>& props, PointCloudData& out) {
    for (int i = 0; i < vertexCount && !stream.atEnd(); ++i) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty())
            continue;

        QStringList tokens = line.split(' ', Qt::SkipEmptyParts);
        Eigen::Vector3d pt(0, 0, 0);
        Eigen::Vector3d normal(0, 0, 0);
        Eigen::Vector3d color(0.5, 0.5, 0.5);
        bool hasNormal = false, hasColor = false;

        for (int j = 0; j < props.size() && j < tokens.size(); ++j) {
            double val = tokens[j].toDouble();
            const QString& name = props[j].name.toLower();

            if (name == "x")
                pt.x() = val;
            else if (name == "y")
                pt.y() = val;
            else if (name == "z")
                pt.z() = val;
            else if (name == "nx") {
                normal.x() = val;
                hasNormal = true;
            } else if (name == "ny") {
                normal.y() = val;
                hasNormal = true;
            } else if (name == "nz") {
                normal.z() = val;
                hasNormal = true;
            } else if (name == "red" || name == "r") {
                color.x() = props[j].isFloat ? val : val / 255.0;
                hasColor = true;
            } else if (name == "green" || name == "g") {
                color.y() = props[j].isFloat ? val : val / 255.0;
                hasColor = true;
            } else if (name == "blue" || name == "b") {
                color.z() = props[j].isFloat ? val : val / 255.0;
                hasColor = true;
            }
        }

        out.points.push_back(pt);
        if (hasNormal)
            out.normals.push_back(normal);
        if (hasColor)
            out.colors.push_back(color);
    }
    return !out.points.empty();
}

bool PlyLoader::parseBinary(QFile& file, qint64 dataStart, int vertexCount, const QList<Property>& props,
                            PointCloudData& out) {
    if (!file.open(QIODevice::ReadOnly))
        return false;
    file.seek(dataStart);

    QDataStream ds(&file);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);

    for (int i = 0; i < vertexCount; ++i) {
        Eigen::Vector3d pt(0, 0, 0);
        Eigen::Vector3d normal(0, 0, 0);
        Eigen::Vector3d color(0.5, 0.5, 0.5);
        bool hasNormal = false, hasColor = false;

        for (const Property& p : props) {
            float fval = 0;
            quint8 uval = 0;
            if (p.isFloat) {
                ds >> fval;
            } else {
                ds >> uval;
            }
            double val = p.isFloat ? static_cast<double>(fval) : static_cast<double>(uval);
            const QString& name = p.name.toLower();

            if (name == "x")
                pt.x() = val;
            else if (name == "y")
                pt.y() = val;
            else if (name == "z")
                pt.z() = val;
            else if (name == "nx") {
                normal.x() = val;
                hasNormal = true;
            } else if (name == "ny") {
                normal.y() = val;
                hasNormal = true;
            } else if (name == "nz") {
                normal.z() = val;
                hasNormal = true;
            } else if (name == "red" || name == "r") {
                color.x() = p.isFloat ? val : val / 255.0;
                hasColor = true;
            } else if (name == "green" || name == "g") {
                color.y() = p.isFloat ? val : val / 255.0;
                hasColor = true;
            } else if (name == "blue" || name == "b") {
                color.z() = p.isFloat ? val : val / 255.0;
                hasColor = true;
            }
        }

        out.points.push_back(pt);
        if (hasNormal)
            out.normals.push_back(normal);
        if (hasColor)
            out.colors.push_back(color);
    }
    return !out.points.empty();
}

} // namespace DeepLux
