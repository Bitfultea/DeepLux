#include <QtTest/QtTest>
#include <QJsonArray>
#include "core/geometry/MeasurementData.h"
#include "plugins/geometry/MeasurementInput/MeasurementInputPlugin.h"

using namespace DeepLux;

static QJsonArray arr(std::initializer_list<double> vals)
{
    QJsonArray a;
    for (double v : vals) {
        a.append(v);
    }
    return a;
}

class TestMeasurementInput : public QObject {
    Q_OBJECT
private slots:
    void writesPointPair();
    void writesPointLine();
    void writesPointPlane();
    void writesCustomMode();
    void invalidPointValuesFail();
};

void TestMeasurementInput::writesPointPair() {
    MeasurementInputPlugin plugin;
    plugin.initialize();
    plugin.setParam("mode", QString("point_pair"));
    plugin.setParam("point1", arr({10.0, 20.0}));
    plugin.setParam("point2", arr({40.0, 60.0}));
    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    auto p1 = MeasurementData::parsePoint2D(output.data("point1"), nullptr);
    auto p2 = MeasurementData::parsePoint2D(output.data("point2"), nullptr);
    QVERIFY(p1.has_value());
    QVERIFY(p2.has_value());
    QCOMPARE(p1->x, 10.0);
    QCOMPARE(p2->y, 60.0);
    QCOMPARE(output.data("measurement_input_mode").toString(), QString("point_pair"));
}

void TestMeasurementInput::writesPointLine() {
    MeasurementInputPlugin plugin;
    plugin.initialize();
    plugin.setParam("mode", QString("point_line"));
    plugin.setParam("point", arr({5.0, 10.0, 15.0}));
    plugin.setParam("line", arr({0.0, 0.0, 100.0, 0.0}));
    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    auto point = MeasurementData::parsePoint3D(output.data("point"), nullptr);
    QVERIFY(point.has_value());
    QCOMPARE(point->x, 5.0);
    QCOMPARE(point->y, 10.0);
    QCOMPARE(point->z, 15.0);
    auto line = MeasurementData::parseLine2D(output.data("line"), nullptr);
    QVERIFY(line.has_value());
    QCOMPARE(line->p1.x, 0.0);
    QCOMPARE(line->p2.x, 100.0);
    QCOMPARE(output.data("measurement_input_mode").toString(), QString("point_line"));
}

void TestMeasurementInput::writesPointPlane() {
    MeasurementInputPlugin plugin;
    plugin.initialize();
    plugin.setParam("mode", QString("point_plane"));
    plugin.setParam("point", arr({1.0, 2.0, 3.0}));
    plugin.setParam("plane", arr({0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0}));
    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    auto point = MeasurementData::parsePoint3D(output.data("point"), nullptr);
    QVERIFY(point.has_value());
    QCOMPARE(point->z, 3.0);
    auto plane = MeasurementData::parsePlane3D(output.data("plane"), nullptr);
    QVERIFY(plane.has_value());
    QCOMPARE(output.data("measurement_input_mode").toString(), QString("point_plane"));
}

void TestMeasurementInput::writesCustomMode() {
    MeasurementInputPlugin plugin;
    plugin.initialize();
    plugin.setParam("mode", QString("custom"));
    plugin.setParam("point1", arr({1.0, 2.0}));
    plugin.setParam("point2", arr({3.0, 4.0}));
    plugin.setParam("point", arr({5.0, 6.0, 7.0}));
    plugin.setParam("line", arr({8.0, 9.0, 10.0, 11.0}));
    plugin.setParam("plane", arr({0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0}));
    ImageData input, output;
    QVERIFY(plugin.execute(input, output));

    auto p1 = MeasurementData::parsePoint2D(output.data("point1"), nullptr);
    QVERIFY(p1.has_value());
    QCOMPARE(p1->x, 1.0);
    QCOMPARE(p1->y, 2.0);

    auto p2 = MeasurementData::parsePoint2D(output.data("point2"), nullptr);
    QVERIFY(p2.has_value());
    QCOMPARE(p2->x, 3.0);
    QCOMPARE(p2->y, 4.0);

    auto point = MeasurementData::parsePoint3D(output.data("point"), nullptr);
    QVERIFY(point.has_value());
    QCOMPARE(point->z, 7.0);

    auto line = MeasurementData::parseLine2D(output.data("line"), nullptr);
    QVERIFY(line.has_value());
    QCOMPARE(line->p1.x, 8.0);

    auto plane = MeasurementData::parsePlane3D(output.data("plane"), nullptr);
    QVERIFY(plane.has_value());

    QCOMPARE(output.data("measurement_input_mode").toString(), QString("custom"));
}

void TestMeasurementInput::invalidPointValuesFail() {
    MeasurementInputPlugin plugin;
    plugin.initialize();
    // point_pair needs valid point1 (2 values) and point2 (2 values)
    plugin.setParam("mode", QString("point_pair"));
    plugin.setParam("point1", arr({0.0})); // invalid - need 2 values
    plugin.setParam("point2", arr({0.0, 0.0}));
    ImageData input, output;
    QVERIFY(!plugin.execute(input, output));
}

QTEST_MAIN(TestMeasurementInput)
#include "test_measurementinput.moc"
