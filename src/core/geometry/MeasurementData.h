#pragma once

#include "display/DisplayData.h"
#include "model/ImageData.h"

#include <QPointF>
#include <QVariant>
#include <optional>

namespace DeepLux {

struct MeasurementPoint2D {
    double x = 0.0;
    double y = 0.0;
};

struct MeasurementPoint3D {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct MeasurementLine2D {
    MeasurementPoint2D p1;
    MeasurementPoint2D p2;
};

struct MeasurementPlane3D {
    MeasurementPoint3D p1;
    MeasurementPoint3D p2;
    MeasurementPoint3D p3;
};

namespace MeasurementKeys {
inline constexpr const char* Point = "point";
inline constexpr const char* Point1 = "point1";
inline constexpr const char* Point2 = "point2";
inline constexpr const char* Line = "line";
inline constexpr const char* Line1 = "line1";
inline constexpr const char* Line2 = "line2";
inline constexpr const char* Plane = "plane";
inline constexpr const char* PointCloud = "point_cloud";
inline constexpr const char* Dimension = "measurement_dimension";
}

class MeasurementData {
public:
    static std::optional<MeasurementPoint2D> parsePoint2D(const QVariant& value, QString* error);
    static std::optional<MeasurementPoint3D> parsePoint3D(const QVariant& value, QString* error);
    static std::optional<MeasurementLine2D> parseLine2D(const QVariant& value, QString* error);
    static std::optional<MeasurementPlane3D> parsePlane3D(const QVariant& value, QString* error);

    static void setPointCloud(ImageData& image, const PointCloudData& cloud);
    static std::optional<PointCloudData> pointCloud(const ImageData& image, QString* error);
};

} // namespace DeepLux
