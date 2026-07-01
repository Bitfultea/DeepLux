#include <QtTest>
#include <QCoreApplication>
#include "plugins/geometry/DistancePP/DistancePPPlugin.h"
#include "core/model/ImageData.h"

using namespace DeepLux;

class TestDistancePP : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestDistancePP Start ===";
    }

    void testPluginInitialization() {
        DistancePPPlugin plugin;
        QVERIFY(plugin.initialize());
        QVERIFY(plugin.isInitialized());
    }

    void testPluginInfo() {
        DistancePPPlugin plugin;

        QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.distancepp"));
        QCOMPARE(plugin.name(), QString("点点距离"));
        QCOMPARE(plugin.category(), QString("geometry"));
        QVERIFY(!plugin.version().isEmpty());
    }

    void testDistanceCalculation_data() {
        QTest::addColumn<double>("x1");
        QTest::addColumn<double>("y1");
        QTest::addColumn<double>("x2");
        QTest::addColumn<double>("y2");
        QTest::addColumn<double>("expectedDist");
        QTest::addColumn<double>("expectedDeltaX");
        QTest::addColumn<double>("expectedDeltaY");

        // Test 1: Same point
        QTest::newRow("same_point") << 0.0 << 0.0 << 0.0 << 0.0 << 0.0 << 0.0 << 0.0;

        // Test 2: Horizontal distance
        QTest::newRow("horizontal") << 0.0 << 0.0 << 100.0 << 0.0 << 100.0 << 100.0 << 0.0;

        // Test 3: Vertical distance
        QTest::newRow("vertical") << 0.0 << 0.0 << 0.0 << 100.0 << 100.0 << 0.0 << 100.0;

        // Test 4: Diagonal (3-4-5 triangle)
        QTest::newRow("diagonal") << 0.0 << 0.0 << 30.0 << 40.0 << 50.0 << 30.0 << 40.0;
    }

    void testDistanceCalculation() {
        QFETCH(double, x1);
        QFETCH(double, y1);
        QFETCH(double, x2);
        QFETCH(double, y2);
        QFETCH(double, expectedDist);
        QFETCH(double, expectedDeltaX);
        QFETCH(double, expectedDeltaY);

        DistancePPPlugin plugin;
        QVERIFY(plugin.initialize());

        // Create input with two points
        QVariant point1Variant = QVariant::fromValue(QPointF(x1, y1));
        QVariant point2Variant = QVariant::fromValue(QPointF(x2, y2));

        ImageData input;
        input.setData("point1", point1Variant);
        input.setData("point2", point2Variant);

        ImageData output;
        bool result = plugin.execute(input, output);

        if (result) {
            double distance = output.data("distance").toDouble();
            double deltaX = output.data("delta_x").toDouble();
            double deltaY = output.data("delta_y").toDouble();

            qDebug() << "Expected: dist=" << expectedDist << ", dx=" << expectedDeltaX << ", dy=" << expectedDeltaY;
            qDebug() << "Got: dist=" << distance << ", dx=" << deltaX << ", dy=" << deltaY;

            QVERIFY2(qAbs(distance - expectedDist) < 0.01,
                     QString("Distance mismatch: expected %1, got %2").arg(expectedDist).arg(distance).toUtf8());
            QVERIFY2(qAbs(deltaX - expectedDeltaX) < 0.01,
                     QString("DeltaX mismatch: expected %1, got %2").arg(expectedDeltaX).arg(deltaX).toUtf8());
            QVERIFY2(qAbs(deltaY - expectedDeltaY) < 0.01,
                     QString("DeltaY mismatch: expected %1, got %2").arg(expectedDeltaY).arg(deltaY).toUtf8());
        } else {
            qWarning() << "Distance calculation failed";
            QFAIL("Distance calculation should succeed");
        }
    }

    void testMissingPoint1() {
        DistancePPPlugin plugin;
        QVERIFY(plugin.initialize());

        QVariant point2Variant = QVariant::fromValue(QPointF(100, 100));

        ImageData input;
        input.setData("point2", point2Variant);

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail with missing point1");
    }

    void testMissingPoint2() {
        DistancePPPlugin plugin;
        QVERIFY(plugin.initialize());

        QVariant point1Variant = QVariant::fromValue(QPointF(0, 0));

        ImageData input;
        input.setData("point1", point1Variant);

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail with missing point2");
    }

    void testRejectsMalformedPointCoordinates() {
        DistancePPPlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        input.setData("point1", QVariantList{QString("not-a-number"), 0.0});
        input.setData("point2", QVariant::fromValue(QPointF(100.0, 100.0)));

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail instead of silently converting invalid point1 coordinate to zero");
        QVERIFY(!output.data("distance").isValid());

        input = ImageData();
        output = ImageData();
        input.setData("point1", QVariant::fromValue(QPointF(0.0, 0.0)));
        input.setData("point2", QVariantList{100.0, QString("not-a-number")});

        result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail instead of silently converting invalid point2 coordinate to zero");
        QVERIFY(!output.data("distance").isValid());
    }

    void cleanupTestCase() {
        qDebug() << "=== TestDistancePP End ===";
    }
};

QTEST_MAIN(TestDistancePP)
#include "test_distancepp.moc"
