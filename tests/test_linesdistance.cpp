#include <QtTest>
#include <QCoreApplication>
#include "plugins/geometry/LinesDistance/LinesDistancePlugin.h"
#include "core/model/ImageData.h"

using namespace DeepLux;

class TestLinesDistance : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestLinesDistance Start ===";
    }

    void testPluginInitialization() {
        LinesDistancePlugin plugin;
        QVERIFY(plugin.initialize());
        QVERIFY(plugin.isInitialized());
    }

    void testPluginInfo() {
        LinesDistancePlugin plugin;

        QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.linesdistance"));
        QCOMPARE(plugin.name(), QString("线线距离"));
        QCOMPARE(plugin.category(), QString("geometry"));
        QVERIFY(!plugin.version().isEmpty());
    }

    void testDistanceCalculation_data() {
        QTest::addColumn<double>("line1X1");
        QTest::addColumn<double>("line1Y1");
        QTest::addColumn<double>("line1X2");
        QTest::addColumn<double>("line1Y2");
        QTest::addColumn<double>("line2X1");
        QTest::addColumn<double>("line2Y1");
        QTest::addColumn<double>("line2X2");
        QTest::addColumn<double>("line2Y2");
        QTest::addColumn<double>("expectedDist");

        // Test 1: Two horizontal parallel lines with distance 10
        QTest::newRow("parallel_horizontal") << 0.0 << 0.0 << 100.0 << 0.0
                                              << 0.0 << 10.0 << 100.0 << 10.0 << 10.0;

        // Test 2: Two vertical parallel lines with distance 20
        QTest::newRow("parallel_vertical") << 0.0 << 0.0 << 0.0 << 100.0
                                           << 20.0 << 0.0 << 20.0 << 100.0 << 20.0;

        // Test 3: Two intersecting lines (distance should be 0)
        QTest::newRow("intersecting") << 0.0 << 0.0 << 100.0 << 100.0
                                        << 0.0 << 100.0 << 100.0 << 0.0 << 0.0;
    }

    void testDistanceCalculation() {
        QFETCH(double, line1X1);
        QFETCH(double, line1Y1);
        QFETCH(double, line1X2);
        QFETCH(double, line1Y2);
        QFETCH(double, line2X1);
        QFETCH(double, line2Y1);
        QFETCH(double, line2X2);
        QFETCH(double, line2Y2);
        QFETCH(double, expectedDist);

        LinesDistancePlugin plugin;
        QVERIFY(plugin.initialize());

        // Create input with two lines
        QVector<QPointF> line1Points;
        line1Points << QPointF(line1X1, line1Y1) << QPointF(line1X2, line1Y2);
        QVariant line1Variant = QVariant::fromValue(line1Points);

        QVector<QPointF> line2Points;
        line2Points << QPointF(line2X1, line2Y1) << QPointF(line2X2, line2Y2);
        QVariant line2Variant = QVariant::fromValue(line2Points);

        ImageData input;
        input.setData("line1", line1Variant);
        input.setData("line2", line2Variant);

        ImageData output;
        bool result = plugin.execute(input, output);

        if (result) {
            double distance = output.data("distance").toDouble();
            qDebug() << "Expected distance:" << expectedDist << ", calculated:" << distance;
            QVERIFY2(qAbs(distance - expectedDist) < 0.01,
                     QString("Distance mismatch: expected %1, got %2").arg(expectedDist).arg(distance).toUtf8());
        } else {
            qWarning() << "Lines distance calculation failed";
            QFAIL("Lines distance calculation should succeed");
        }
    }

    void testMissingLine1() {
        LinesDistancePlugin plugin;
        QVERIFY(plugin.initialize());

        QVector<QPointF> line2Points;
        line2Points << QPointF(0, 0) << QPointF(100, 100);
        QVariant line2Variant = QVariant::fromValue(line2Points);

        ImageData input;
        input.setData("line2", line2Variant);

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail with missing line1");
    }

    void testMissingLine2() {
        LinesDistancePlugin plugin;
        QVERIFY(plugin.initialize());

        QVector<QPointF> line1Points;
        line1Points << QPointF(0, 0) << QPointF(100, 100);
        QVariant line1Variant = QVariant::fromValue(line1Points);

        ImageData input;
        input.setData("line1", line1Variant);

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(!result, "Should fail with missing line2");
    }

    void cleanupTestCase() {
        qDebug() << "=== TestLinesDistance End ===";
    }
};

QTEST_MAIN(TestLinesDistance)
#include "test_linesdistance.moc"