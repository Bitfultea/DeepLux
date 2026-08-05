#include "core/geometry/MeasurementData.h"

#include <QPointF>
#include <QVariant>
#include <QVariantList>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace DeepLux {

namespace {

MeasurementPoint3D subtract(const MeasurementPoint3D& left, const MeasurementPoint3D& right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

MeasurementPoint3D addScaled(const MeasurementPoint3D& point, const MeasurementPoint3D& direction, double scale) {
    return {point.x + direction.x * scale, point.y + direction.y * scale, point.z + direction.z * scale};
}

double dot(const MeasurementPoint3D& left, const MeasurementPoint3D& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

MeasurementPoint3D closestPointOnSegment(const MeasurementPoint3D& point, const MeasurementPoint3D& start,
                                         const MeasurementPoint3D& end) {
    const MeasurementPoint3D direction = subtract(end, start);
    const double lengthSquared = dot(direction, direction);
    if (lengthSquared <= 1e-12) {
        return start;
    }
    const double t = std::clamp(dot(subtract(point, start), direction) / lengthSquared, 0.0, 1.0);
    return addScaled(start, direction, t);
}

MeasurementSegmentDistance3D makeSegmentDistance(const MeasurementPoint3D& first, const MeasurementPoint3D& second) {
    const MeasurementPoint3D delta = subtract(first, second);
    return {first, second, std::sqrt(dot(delta, delta))};
}

} // namespace

// ---------------------------------------------------------------------------
// parsePoint2D
// Accepts: QPointF, or QVariantList with exactly 2 numeric values.
// Rejects: QVariantList with 3 values (users should use 3D measurement).
// ---------------------------------------------------------------------------
std::optional<MeasurementPoint2D> MeasurementData::parsePoint2D(const QVariant& value, QString* error) {
    // Accept QPointF directly
    if (value.canConvert<QPointF>()) {
        QPointF pt = value.toPointF();
        return MeasurementPoint2D{pt.x(), pt.y()};
    }

    if (value.type() == QVariant::List) {
        QVariantList list = value.toList();

        if (static_cast<int>(list.size()) == 3) {
            if (error)
                *error = QStringLiteral("2D measurement requires exactly 2 values (x, y). "
                                        "Use a 3D measurement for (x, y, z).");
            return std::nullopt;
        }

        if (static_cast<int>(list.size()) == 2) {
            bool ok1 = false, ok2 = false;
            double x = list[0].toDouble(&ok1);
            double y = list[1].toDouble(&ok2);
            if (ok1 && ok2)
                return MeasurementPoint2D{x, y};
        }

        if (error)
            *error = QStringLiteral("2D point: expected [x, y] list, got %1 values").arg(list.size());
        return std::nullopt;
    }

    if (error)
        *error = QStringLiteral("2D point: expected QPointF or [x, y] list");
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// parsePoint3D
// Accepts: [x, y] (z = 0) or [x, y, z].
// ---------------------------------------------------------------------------
std::optional<MeasurementPoint3D> MeasurementData::parsePoint3D(const QVariant& value, QString* error) {
    if (value.type() != QVariant::List) {
        if (error)
            *error = QStringLiteral("3D point: expected [x, y] or [x, y, z] list");
        return std::nullopt;
    }

    QVariantList list = value.toList();
    int n = static_cast<int>(list.size());

    if (n == 2) {
        bool ok1 = false, ok2 = false;
        double x = list[0].toDouble(&ok1);
        double y = list[1].toDouble(&ok2);
        if (ok1 && ok2)
            return MeasurementPoint3D{x, y, 0.0};
        if (error)
            *error = QStringLiteral("3D point: values in list must be numeric");
        return std::nullopt;
    }

    if (n == 3) {
        bool ok1 = false, ok2 = false, ok3 = false;
        double x = list[0].toDouble(&ok1);
        double y = list[1].toDouble(&ok2);
        double z = list[2].toDouble(&ok3);
        if (ok1 && ok2 && ok3)
            return MeasurementPoint3D{x, y, z};
        if (error)
            *error = QStringLiteral("3D point: values in list must be numeric");
        return std::nullopt;
    }

    if (error)
        *error = QStringLiteral("3D point: expected [x, y] or [x, y, z] list, got %1 values").arg(n);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// parseLine2D
// Accepts: QVector<QPointF> with 2 points, or QVariantList [x1,y1,x2,y2].
// ---------------------------------------------------------------------------
std::optional<MeasurementLine2D> MeasurementData::parseLine2D(const QVariant& value, QString* error) {
    // Accept flat list of 4 numbers [x1, y1, x2, y2]
    if (value.type() == QVariant::List) {
        QVariantList list = value.toList();
        if (static_cast<int>(list.size()) == 4) {
            bool ok[4] = {};
            double vals[4];
            for (int i = 0; i < 4; ++i)
                vals[i] = list[i].toDouble(&ok[i]);
            if (ok[0] && ok[1] && ok[2] && ok[3]) {
                return MeasurementLine2D{{vals[0], vals[1]}, {vals[2], vals[3]}};
            }
        }
    }

    // Accept QVector<QPointF> with exactly 2 points
    if (value.canConvert<QVector<QPointF>>()) {
        QVector<QPointF> pts = value.value<QVector<QPointF>>();
        if (pts.size() >= 2) {
            return MeasurementLine2D{{pts[0].x(), pts[0].y()}, {pts[1].x(), pts[1].y()}};
        }
    }

    // Accept QVector<QPointF> or list-of-points with exactly 2 points
    if (value.canConvert<QVariantList>()) {
        QVariantList list = value.toList();
        if (static_cast<int>(list.size()) == 2) {
            QPointF p1, p2;

            // Try to convert each element to QPointF
            if (list[0].canConvert<QPointF>() && list[1].canConvert<QPointF>()) {
                p1 = list[0].toPointF();
                p2 = list[1].toPointF();
                return MeasurementLine2D{{p1.x(), p1.y()}, {p2.x(), p2.y()}};
            }

            // Try to interpret each element as a 2-element list [x, y]
            if (list[0].type() == QVariant::List && list[1].type() == QVariant::List) {
                QVariantList sub1 = list[0].toList();
                QVariantList sub2 = list[1].toList();
                if (sub1.size() >= 2 && sub2.size() >= 2) {
                    bool ok[4] = {};
                    p1.setX(sub1[0].toDouble(&ok[0]));
                    p1.setY(sub1[1].toDouble(&ok[1]));
                    p2.setX(sub2[0].toDouble(&ok[2]));
                    p2.setY(sub2[1].toDouble(&ok[3]));
                    if (ok[0] && ok[1] && ok[2] && ok[3])
                        return MeasurementLine2D{{p1.x(), p1.y()}, {p2.x(), p2.y()}};
                }
            }
        }
    }

    if (error)
        *error = QStringLiteral("2D line: expected two QPointF values or [x1,y1,x2,y2] list");
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// parsePlane3D
// Accepts: exactly 9 numeric values representing 3 points.
// Rejects: collinear points (normal length < 1e-10).
// ---------------------------------------------------------------------------
std::optional<MeasurementPlane3D> MeasurementData::parsePlane3D(const QVariant& value, QString* error) {
    if (value.type() != QVariant::List) {
        if (error)
            *error = QStringLiteral("3D plane: expected list of 9 numeric values [x1,y1,z1, x2,y2,z2, x3,y3,z3]");
        return std::nullopt;
    }

    QVariantList list = value.toList();
    if (static_cast<int>(list.size()) != 9) {
        if (error)
            *error = QStringLiteral("3D plane: expected 9 values, got %1").arg(list.size());
        return std::nullopt;
    }

    // Parse 9 values
    double v[9];
    for (int i = 0; i < 9; ++i) {
        bool ok = false;
        v[i] = list[i].toDouble(&ok);
        if (!ok) {
            if (error)
                *error = QStringLiteral("3D plane: value at index %1 is not numeric").arg(i);
            return std::nullopt;
        }
    }

    MeasurementPoint3D p1{v[0], v[1], v[2]};
    MeasurementPoint3D p2{v[3], v[4], v[5]};
    MeasurementPoint3D p3{v[6], v[7], v[8]};

    // Check for degenerate (collinear) points via cross-product
    double ux = p2.x - p1.x;
    double uy = p2.y - p1.y;
    double uz = p2.z - p1.z;

    double vx = p3.x - p1.x;
    double vy = p3.y - p1.y;
    double vz = p3.z - p1.z;

    // Cross product: u x v
    double nx = uy * vz - uz * vy;
    double ny = uz * vx - ux * vz;
    double nz = ux * vy - uy * vx;

    double normalLen = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (normalLen < 1e-10) {
        if (error)
            *error = QStringLiteral("3D plane: points are collinear, cannot define a plane");
        return std::nullopt;
    }

    return MeasurementPlane3D{p1, p2, p3};
}

MeasurementSegmentDistance3D MeasurementData::closestPointsBetweenSegments(const MeasurementPoint3D& firstStart,
                                                                           const MeasurementPoint3D& firstEnd,
                                                                           const MeasurementPoint3D& secondStart,
                                                                           const MeasurementPoint3D& secondEnd) {
    const MeasurementPoint3D firstDirection = subtract(firstEnd, firstStart);
    const MeasurementPoint3D secondDirection = subtract(secondEnd, secondStart);
    const MeasurementPoint3D betweenStarts = subtract(firstStart, secondStart);
    const double firstLengthSquared = dot(firstDirection, firstDirection);
    const double secondLengthSquared = dot(secondDirection, secondDirection);

    if (firstLengthSquared <= 1e-12 && secondLengthSquared <= 1e-12) {
        return makeSegmentDistance(firstStart, secondStart);
    }
    if (firstLengthSquared <= 1e-12) {
        return makeSegmentDistance(firstStart, closestPointOnSegment(firstStart, secondStart, secondEnd));
    }
    if (secondLengthSquared <= 1e-12) {
        return makeSegmentDistance(closestPointOnSegment(secondStart, firstStart, firstEnd), secondStart);
    }

    const double directionDot = dot(firstDirection, secondDirection);
    const double firstStartDot = dot(firstDirection, betweenStarts);
    const double secondStartDot = dot(secondDirection, betweenStarts);
    const double denominator = firstLengthSquared * secondLengthSquared - directionDot * directionDot;

    if (denominator <= 1e-12 * firstLengthSquared * secondLengthSquared) {
        MeasurementSegmentDistance3D closest =
            makeSegmentDistance(firstStart, closestPointOnSegment(firstStart, secondStart, secondEnd));
        const MeasurementSegmentDistance3D candidates[] = {
            makeSegmentDistance(firstEnd, closestPointOnSegment(firstEnd, secondStart, secondEnd)),
            makeSegmentDistance(closestPointOnSegment(secondStart, firstStart, firstEnd), secondStart),
            makeSegmentDistance(closestPointOnSegment(secondEnd, firstStart, firstEnd), secondEnd),
        };
        for (const MeasurementSegmentDistance3D& candidate : candidates) {
            if (candidate.distance < closest.distance) {
                closest = candidate;
            }
        }
        return closest;
    }

    double firstT =
        std::clamp((directionDot * secondStartDot - firstStartDot * secondLengthSquared) / denominator, 0.0, 1.0);
    double secondT = (directionDot * firstT + secondStartDot) / secondLengthSquared;
    if (secondT <= 0.0) {
        secondT = 0.0;
        firstT = std::clamp(-firstStartDot / firstLengthSquared, 0.0, 1.0);
    } else if (secondT >= 1.0) {
        secondT = 1.0;
        firstT = std::clamp((directionDot - firstStartDot) / firstLengthSquared, 0.0, 1.0);
    }

    return makeSegmentDistance(addScaled(firstStart, firstDirection, firstT),
                               addScaled(secondStart, secondDirection, secondT));
}

// ---------------------------------------------------------------------------
// setPointCloud
// Stores PointCloudData into ImageData metadata under MeasurementKeys::PointCloud.
// ---------------------------------------------------------------------------
void MeasurementData::setPointCloud(ImageData& image, const PointCloudData& cloud) {
    image.setData(QString::fromLatin1(MeasurementKeys::PointCloud), QVariant::fromValue(cloud));
}

// ---------------------------------------------------------------------------
// pointCloud
// Retrieves PointCloudData from ImageData metadata.
// ---------------------------------------------------------------------------
std::optional<PointCloudData> MeasurementData::pointCloud(const ImageData& image, QString* error) {
    QVariant var = image.data(QString::fromLatin1(MeasurementKeys::PointCloud));

    if (!var.isValid()) {
        if (error)
            *error = QStringLiteral("point_cloud key not found in ImageData metadata");
        return std::nullopt;
    }

    if (var.canConvert<PointCloudData>()) {
        return var.value<PointCloudData>();
    }

    if (error)
        *error = QStringLiteral("point_cloud value is not a PointCloudData");
    return std::nullopt;
}

} // namespace DeepLux
