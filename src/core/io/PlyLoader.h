#pragma once

#include "core/display/DisplayData.h"

#include <QFile>
#include <QString>
#include <QTextStream>

namespace DeepLux {

class PlyLoader {
public:
    struct Property {
        enum class ScalarType { Float32, Float64, UInt8, Int32 };
        QString name;
        ScalarType type = ScalarType::Float32;
        bool isList = false;
        ScalarType itemType = ScalarType::Float32;
        bool isFloat() const {
            return type == ScalarType::Float32 || type == ScalarType::Float64;
        }
    };
    struct Element {
        QString name;
        int count = 0;
        QList<Property> properties;
    };

    static bool load(const QString& filePath, PointCloudData& outData, QString& errorMsg);

private:
    static bool parseHeader(QTextStream& stream, QList<Element>& elements, bool& isBinary, QString& errorMsg);
    static bool parseAscii(QTextStream& stream, const QList<Element>& elements, PointCloudData& out, QString& errorMsg);
    static bool parseBinary(QFile& file, qint64 dataStart, const QList<Element>& elements, PointCloudData& out,
                            QString& errorMsg);
};

} // namespace DeepLux
