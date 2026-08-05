#include "core/geometry/MeasurementData.h"

#include <QtTest/QtTest>

using namespace DeepLux;

class TestMeasurementData : public QObject {
    Q_OBJECT

private slots:
    void parsePoint2DRejects3DList();
    void parsePoint3DAccepts2DListAsZ0();
    void parseLine2DAcceptsFourValueList();
    void parsePlane3DRejectsDegeneratePlane();
    void closestSegmentPointsUsesTrueNearestPair();
    void pointCloudRoundTripThroughImageData();
};

void TestMeasurementData::parsePoint2DRejects3DList() {
    QString error;
    QVERIFY(!MeasurementData::parsePoint2D(QVariantList{1.0, 2.0, 3.0}, &error).has_value());
    QVERIFY(error.contains("2D"));
}

void TestMeasurementData::parsePoint3DAccepts2DListAsZ0() {
    auto point = MeasurementData::parsePoint3D(QVariantList{1.0, 2.0}, nullptr);
    QVERIFY(point.has_value());
    QCOMPARE(point->x, 1.0);
    QCOMPARE(point->y, 2.0);
    QCOMPARE(point->z, 0.0);
}

void TestMeasurementData::parseLine2DAcceptsFourValueList() {
    auto line = MeasurementData::parseLine2D(QVariantList{1.0, 2.0, 3.0, 4.0}, nullptr);
    QVERIFY(line.has_value());
    QCOMPARE(line->p1.x, 1.0);
    QCOMPARE(line->p2.y, 4.0);
}

void TestMeasurementData::parsePlane3DRejectsDegeneratePlane() {
    QString error;
    auto plane = MeasurementData::parsePlane3D(QVariantList{0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 2.0, 2.0, 2.0}, &error);
    QVERIFY(!plane.has_value());
    QVERIFY(error.contains("plane"));
}

void TestMeasurementData::closestSegmentPointsUsesTrueNearestPair() {
    const MeasurementSegmentDistance3D parallel = MeasurementData::closestPointsBetweenSegments(
        {0.0, 0.0, 0.0}, {100.0, 0.0, 0.0}, {50.0, 10.0, 0.0}, {150.0, 10.0, 0.0});
    QCOMPARE(parallel.distance, 10.0);
    QCOMPARE(parallel.pointOnFirst.y, 0.0);
    QCOMPARE(parallel.pointOnSecond.y, 10.0);

    const MeasurementSegmentDistance3D skew = MeasurementData::closestPointsBetweenSegments(
        {-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {0.0, -10.0, 5.0}, {0.0, 10.0, 5.0});
    QCOMPARE(skew.distance, 5.0);
    QCOMPARE(skew.pointOnFirst.x, 0.0);
    QCOMPARE(skew.pointOnSecond.y, 0.0);
}

void TestMeasurementData::pointCloudRoundTripThroughImageData() {
    PointCloudData cloud;
    cloud.points.push_back(Eigen::Vector3d(1.0, 2.0, 3.0));

    ImageData image;
    MeasurementData::setPointCloud(image, cloud);

    auto parsed = MeasurementData::pointCloud(image, nullptr);
    QVERIFY(parsed.has_value());
    QCOMPARE(static_cast<int>(parsed->points.size()), 1);
    QCOMPARE(parsed->points[0].z(), 3.0);
}

QTEST_MAIN(TestMeasurementData)
#include "test_measurementdata.moc"
