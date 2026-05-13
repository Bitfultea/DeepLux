#pragma once

#include "core/display/DisplayData.h"
#include <QString>
#include <QFile>
#include <QTextStream>

namespace DeepLux {

class PlyLoader {
public:
    struct Property {
        QString name;
        bool isFloat = false;
    };

    static bool load(const QString& filePath, PointCloudData& outData, QString& errorMsg);

private:
    static bool parseHeader(QTextStream& stream, int& vertexCount, QList<Property>& props, bool& isBinary, QString& errorMsg);
    static bool parseAscii(QTextStream& stream, int vertexCount, const QList<Property>& props, PointCloudData& out);
    static bool parseBinary(QFile& file, qint64 dataStart, int vertexCount, const QList<Property>& props, PointCloudData& out);
};

} // namespace DeepLux
