#include <QtTest>
#include <QCoreApplication>
#include "plugins/geometry/DistancePL/DistancePLPlugin.h"
#include "core/model/ImageData.h"

using namespace DeepLux;

class TestDistancePL : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestDistancePL Start ===";
    }

    void testPluginInitialization() {
        DistancePLPlugin plugin;
        QVERIFY(plugin.initialize());
        QVERIFY(plugin.isInitialized());
    }

    void testPluginInfo() {
        DistancePLPlugin plugin;

        QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.distancepl"));
        QCOMPARE(plugin.name(), QString("点线距离"));
        QCOMPARE(plugin.category(), QString("geometry"));
        QVERIFY(!plugin.version().isEmpty());
    }

    void testDistanceCalculation_data() {
        QTest::addColumn<double>("pointX");
        QTest::addColumn<double>("pointY");
        QTest::addColumn<double>("lineX1");
        QTest::addColumn<double>("lineY1");
        QTest::addColumn<double>("lineX2");
        QTest::addColumn<double>("lineY2");
        QTest::addColumn<double>("expectedDist");

        // Test 1: Point (0,0) to horizontal line y=10
        QTest::newRow("point_to_horizontal") << 0.0 << 0.0 << 0.0 << 10.0 << 100.0 << 10.0 << 10.0;

        // Test 2: Point (0,0) to vertical line x=10
        QTest::newRow("point_to_vertical") << 0.0 << 0.0 << 10.0 << 0.0 << 10.0 << 100.0 << 10.0;

        // Test 3: Point on the line
        QTest::newRow("point_on_line") << 0.0 << 10.0 << 0.0 << 10.0 << 100.0 << 10.0 << 0.0;

        // Test 4: Point to diagonal line
        QTest::newRow("point_to_diagonal") << 0.0 << 0.0 << 0.0 << 0.0 << 100.0 << 100.0 << 0.0;
    }

    void testDistanceCalculation() {
        QFETCH(double, pointX);
        QFETCH(double, pointY);
        QFETCH(double, lineX1);
        QFETCH(double, lineY1);
        QFETCH(double, lineX2);
        QFETCH(double, lineY2);
        QFETCH(double, expectedDist);

        DistancePLPlugin plugin;
        QVERIFY(plugin.initialize());

        // Create input with point and line
        QVariant pointVariant = QVariant::fromValue(QPointF(pointX, pointY));
        QVector<QPointF> linePoints;
        linePoints << QPointF(lineX1, lineY1) << QPointF(lineX2, lineY2);
        QVariant lineVariant = QVariant::fromValue(linePoints);

        ImageData input;
        input.setData("point", pointVariant);
        input.setData("line", lineVariant);

        ImageData output;
        bool result = plugin.execute(input, output);

        if (result) {
            double distance = output.data("distance").toDouble();
            qDebug() << "Expected distance:" << expectedDist << ", calculated:" << distance;
            QVERIFY2(qAbs(distance - expectedDist) < 0.01,
                     QString("Distance mismatch: expected %1, got %2").arg(expectedDist).arg(distance).toUtf8());
        } else {
            qWarning() << "Distance calculation failed";
            QFAIL("Distance calculation should succeed");
        }
    }

    void testMissingPoint() {
        DistancePLPlugin plugin;
        QVERIFY(plugin.initialize());

        QVector<QPointF> linePoints;
        linePoints << QPointF(0, 0) << QPointF(100, 100);
        QVariant lineVariant = QVariant::fromValue(linePoints);

        ImageData input;
        input.setData("line", lineVariant);

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail with missing point");
    }

    void testMissingLine() {
        DistancePLPlugin plugin;
        QVERIFY(plugin.initialize());

        QVariant pointVariant = QVariant::fromValue(QPointF(0, 0));

        ImageData input;
        input.setData("point", pointVariant);

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail with missing line");
    }

    void testRejectsMalformedCoordinates() {
        DistancePLPlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        input.setData("point", QVariantList{QString("not-a-number"), 0.0});
        input.setData("line", QVariantList{0.0, 10.0, 100.0, 10.0});

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail instead of silently converting invalid point coordinate to zero");
        QVERIFY(!output.data("distance").isValid());

        input = ImageData();
        output = ImageData();
        input.setData("point", QVariant::fromValue(QPointF(0.0, 0.0)));
        input.setData("line", QVariantList{0.0, 10.0, QString("not-a-number"), 10.0});

        result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail instead of silently converting invalid line coordinate to zero");
        QVERIFY(!output.data("distance").isValid());
    }

    void cleanupTestCase() {
        qDebug() << "=== TestDistancePL End ===";
    }
};

QTEST_MAIN(TestDistancePL)
#include "test_distancepl.moc"
