#include "core/geometry/MeasurementData.h"
#include "plugins/geometry/MeasurementInput/MeasurementInputPlugin.h"

#include <QComboBox>
#include <QJsonArray>
#include <QLineEdit>
#include <QtTest/QtTest>
#include <limits>
#include <memory>

using namespace DeepLux;

static QJsonArray arr(std::initializer_list<double> vals) {
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
    void writesWorldPointPair();
    void rejectsInvalidPointPairDimensionsAndValues();
    void writesPointLine();
    void pointLineCoordinateFrameControlsProvenance();
    void linePairCoordinateFrameControlsProvenance();
    void writesLinePair();
    void writesPointPlane();
    void writesCustomMode();
    void invalidPointValuesFail();
    void configWidgetEditsParams();
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
    QCOMPARE(output.data("coordinate_frame").toString(), QString("ImageLocalPixel"));
}

void TestMeasurementInput::writesWorldPointPair() {
    MeasurementInputPlugin plugin;
    plugin.initialize();
    plugin.setParam("mode", QString("point_pair"));
    plugin.setParam("point1", arr({16777217.125, 12.5, -3.75}));
    plugin.setParam("point2", arr({16777220.125, 16.5, -0.75}));
    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("point1").toList(), QVariantList({16777217.125, 12.5, -3.75}));
    QCOMPARE(output.data("point2").toList(), QVariantList({16777220.125, 16.5, -0.75}));
    QCOMPARE(output.data("coordinate_frame").toString(), QString("World3D"));
    QVERIFY(!output.data("source_coordinate_available").toBool());
}

void TestMeasurementInput::rejectsInvalidPointPairDimensionsAndValues() {
    const QList<QVariant> invalidPoints{arr({1.0, 2.0, 3.0, 4.0}), QJsonArray{1.0, QStringLiteral("2")},
                                        QJsonArray{1.0, true},
                                        QJsonArray{1.0, std::numeric_limits<double>::quiet_NaN()}};
    for (const QVariant& point : invalidPoints) {
        MeasurementInputPlugin plugin;
        plugin.initialize();
        plugin.setParam("mode", QString("point_pair"));
        plugin.setParam("point1", point);
        plugin.setParam("point2", arr({3.0, 4.0}));
        ImageData input, output;
        QVERIFY(!plugin.execute(input, output));
    }

    MeasurementInputPlugin mixedDimensions;
    mixedDimensions.initialize();
    mixedDimensions.setParam("mode", QString("point_pair"));
    mixedDimensions.setParam("point1", arr({1.0, 2.0}));
    mixedDimensions.setParam("point2", arr({3.0, 4.0, 5.0}));
    ImageData input, output;
    QVERIFY(!mixedDimensions.execute(input, output));
}

void TestMeasurementInput::writesPointLine() {
    MeasurementInputPlugin plugin;
    plugin.initialize();
    plugin.setParam("mode", QString("point_line"));
    plugin.setParam("point", arr({5.0, 10.0, 15.0}));
    plugin.setParam("line", arr({0.0, 0.0, 100.0, 0.0}));
    plugin.setParam("pick_coordinate_frame", QStringLiteral("ImageLocalPixel"));
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
    QCOMPARE(output.data("coordinate_frame").toString(), QString("ImageLocalPixel"));
}

void TestMeasurementInput::pointLineCoordinateFrameControlsProvenance() {
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
    provenance.sourceSize = QSize(200, 150);
    provenance.hasLocalToSourceTransform = true;
    provenance.localToSource = {1.0, 0.0, 50.0, 0.0, 1.0, 40.0, 0.0, 0.0, 1.0};
    ImageData input(QImage(100, 80, QImage::Format_RGB32));
    input.setCoordinateProvenance(provenance);

    const auto execute = [&input](const QVariant& frame, ImageData& output) {
        MeasurementInputPlugin plugin;
        plugin.initialize();
        plugin.setParam("mode", QStringLiteral("point_line"));
        plugin.setParam("point", arr({5.0, 10.0, 15.0}));
        plugin.setParam("line", arr({1.0, 2.0, 30.0, 40.0}));
        plugin.setParam("pick_coordinate_frame", frame);
        return plugin.execute(input, output);
    };

    ImageData imageOutput;
    QVERIFY(execute(QStringLiteral("ImageLocalPixel"), imageOutput));
    QCOMPARE(imageOutput.data("coordinate_frame").toString(), QStringLiteral("ImageLocalPixel"));
    QVERIFY(imageOutput.data("source_coordinate_available").toBool());
    QCOMPARE(imageOutput.data("point_source").toList(), QVariantList({55.0, 50.0, 15.0}));
    QCOMPARE(imageOutput.data("line_source").toList(), QVariantList({51.0, 42.0, 80.0, 80.0}));

    for (const QVariant& invalidFrame :
         {QVariant(QStringLiteral("Unspecified")), QVariant(QStringLiteral("future-frame")),
          QVariant(QVariantList{QStringLiteral("ImageLocalPixel")})}) {
        ImageData worldOutput;
        QVERIFY(execute(invalidFrame, worldOutput));
        QCOMPARE(worldOutput.data("coordinate_frame").toString(), QStringLiteral("Unspecified"));
        QVERIFY(!worldOutput.data("source_coordinate_available").toBool());
        QVERIFY(!worldOutput.hasData(QStringLiteral("source_image_id")));
        QVERIFY(!worldOutput.hasData(QStringLiteral("source_image_size")));
        QVERIFY(!worldOutput.hasData(QStringLiteral("source_coordinate_origin")));
        QVERIFY(!worldOutput.hasData(QStringLiteral("point_source")));
        QVERIFY(!worldOutput.hasData(QStringLiteral("line_source")));
        QCOMPARE(worldOutput.data("point").toList(), QVariantList({5.0, 10.0, 15.0}));
        QCOMPARE(worldOutput.data("line").toList(), QVariantList({1.0, 2.0, 30.0, 40.0}));
    }
}

void TestMeasurementInput::linePairCoordinateFrameControlsProvenance() {
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
    provenance.sourceSize = QSize(200, 150);
    provenance.hasLocalToSourceTransform = true;
    provenance.localToSource = {1.0, 0.0, 50.0, 0.0, 1.0, 40.0, 0.0, 0.0, 1.0};
    ImageData input(QImage(100, 80, QImage::Format_RGB32));
    input.setCoordinateProvenance(provenance);

    const auto execute = [&input](const QVariant& frame, ImageData& output) {
        MeasurementInputPlugin plugin;
        plugin.initialize();
        plugin.setParam("mode", QStringLiteral("line_pair"));
        plugin.setParam("line1", arr({1.0, 2.0, 30.0, 40.0}));
        plugin.setParam("line2", arr({5.0, 6.0, 70.0, 80.0}));
        plugin.setParam("pick_coordinate_frame", frame);
        return plugin.execute(input, output);
    };

    ImageData imageOutput;
    QVERIFY(execute(QStringLiteral("ImageLocalPixel"), imageOutput));
    QCOMPARE(imageOutput.data("coordinate_frame").toString(), QStringLiteral("ImageLocalPixel"));
    QVERIFY(imageOutput.data("source_coordinate_available").toBool());
    QCOMPARE(imageOutput.data("line1_source").toList(), QVariantList({51.0, 42.0, 80.0, 80.0}));
    QCOMPARE(imageOutput.data("line2_source").toList(), QVariantList({55.0, 46.0, 120.0, 120.0}));

    for (const QVariant& invalidFrame :
         {QVariant(QStringLiteral("Unspecified")), QVariant(QStringLiteral("future-frame")),
          QVariant(QVariantList{QStringLiteral("ImageLocalPixel")})}) {
        ImageData unspecifiedOutput;
        QVERIFY(execute(invalidFrame, unspecifiedOutput));
        QCOMPARE(unspecifiedOutput.data("coordinate_frame").toString(), QStringLiteral("Unspecified"));
        QVERIFY(!unspecifiedOutput.data("source_coordinate_available").toBool());
        QVERIFY(!unspecifiedOutput.hasData(QStringLiteral("source_image_id")));
        QVERIFY(!unspecifiedOutput.hasData(QStringLiteral("source_image_size")));
        QVERIFY(!unspecifiedOutput.hasData(QStringLiteral("source_coordinate_origin")));
        QVERIFY(!unspecifiedOutput.hasData(QStringLiteral("line1_source")));
        QVERIFY(!unspecifiedOutput.hasData(QStringLiteral("line2_source")));
        QCOMPARE(unspecifiedOutput.data("line1").toList(), QVariantList({1.0, 2.0, 30.0, 40.0}));
        QCOMPARE(unspecifiedOutput.data("line2").toList(), QVariantList({5.0, 6.0, 70.0, 80.0}));
    }
}

void TestMeasurementInput::writesLinePair() {
    MeasurementInputPlugin plugin;
    plugin.initialize();
    plugin.setParam("mode", QString("line_pair"));
    plugin.setParam("line1", arr({0.0, 0.0, 10.0, 0.0}));
    plugin.setParam("line2", arr({0.0, 5.0, 10.0, 5.0}));
    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    auto line1 = MeasurementData::parseLine2D(output.data("line1"), nullptr);
    auto line2 = MeasurementData::parseLine2D(output.data("line2"), nullptr);
    QVERIFY(line1.has_value());
    QVERIFY(line2.has_value());
    QCOMPARE(line1->p2.x, 10.0);
    QCOMPARE(line2->p1.y, 5.0);
    QCOMPARE(output.data("measurement_input_mode").toString(), QString("line_pair"));
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

void TestMeasurementInput::configWidgetEditsParams() {
    MeasurementInputPlugin plugin;
    plugin.initialize();
    std::unique_ptr<QWidget> widget(plugin.createConfigWidget());
    QVERIFY(widget != nullptr);

    QComboBox* mode = widget->findChild<QComboBox*>("MeasurementInputModeCombo");
    QLineEdit* point = widget->findChild<QLineEdit*>("MeasurementInputPointEdit");
    QLineEdit* line1 = widget->findChild<QLineEdit*>("MeasurementInputLine1Edit");
    QLineEdit* plane = widget->findChild<QLineEdit*>("MeasurementInputPlaneEdit");
    QVERIFY(mode != nullptr);
    QVERIFY(point != nullptr);
    QVERIFY(line1 != nullptr);
    QVERIFY(plane != nullptr);

    mode->setCurrentText(QStringLiteral("point_plane"));
    point->setText(QStringLiteral("1,2,3"));
    plane->setText(QStringLiteral("0,0,0,1,0,0,0,1,0"));
    QCoreApplication::processEvents();

    QJsonObject params = plugin.currentParams();
    QCOMPARE(params["mode"].toString(), QStringLiteral("point_plane"));
    QCOMPARE(params["point"].toArray().at(2).toDouble(), 3.0);
    QCOMPARE(params["plane"].toArray().size(), 9);
}

QTEST_MAIN(TestMeasurementInput)
#include "test_measurementinput.moc"
