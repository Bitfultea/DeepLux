#include "core/model/ImageData.h"
#include "plugins/geometry/LinesDistance/LinesDistancePlugin.h"

#include <QCoreApplication>
#include <QtTest>

using namespace DeepLux;

// 辅助：运行 LinesDistance 并返回最近点间距（应≈distance）
static double runNearestGap(QVector<QPointF> l1, QVector<QPointF> l2, double& dist) {
    LinesDistancePlugin plugin;
    plugin.initialize();
    ImageData input;
    input.setData("line1", QVariant::fromValue(l1));
    input.setData("line2", QVariant::fromValue(l2));
    ImageData output;
    if (!plugin.execute(input, output))
        return -1;
    dist = output.data("distance").toDouble();
    const double dx = output.data("nearest_x2").toDouble() - output.data("nearest_x1").toDouble();
    const double dy = output.data("nearest_y2").toDouble() - output.data("nearest_y1").toDouble();
    return std::sqrt(dx * dx + dy * dy);
}

class TestLinesDistance : public QObject {
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
        QTest::newRow("parallel_horizontal") << 0.0 << 0.0 << 100.0 << 0.0 << 0.0 << 10.0 << 100.0 << 10.0 << 10.0;

        // Test 2: Two vertical parallel lines with distance 20
        QTest::newRow("parallel_vertical") << 0.0 << 0.0 << 0.0 << 100.0 << 20.0 << 0.0 << 20.0 << 100.0 << 20.0;

        // Test 3: Two intersecting lines (distance should be 0)
        QTest::newRow("intersecting") << 0.0 << 0.0 << 100.0 << 100.0 << 0.0 << 100.0 << 100.0 << 0.0 << 0.0;

        // P0-1 Test 4: 延长线相交但线段本身不相交——旧实现误判为 0，正确应为 2
        // 线段A [(0,0)-(1,0)]，线段B [(3,-1)-(3,1)]；无限直线 y=0 与 x=3 相交，但线段不达交点
        QTest::newRow("extensions_intersect_segments_not")
            << 0.0 << 0.0 << 1.0 << 0.0 << 3.0 << -1.0 << 3.0 << 1.0 << 2.0;

        // P0-1 Test 5: 退化线段（零长度，退化为点 (5,5)）到线段 B [(0,0)-(0,10)]，距离 5
        QTest::newRow("degenerate_point_segment") << 5.0 << 5.0 << 5.0 << 5.0 << 0.0 << 0.0 << 0.0 << 10.0 << 5.0;

        // P0-1 Test 6: 两条分离的共线线段（同一直线但不重叠），端点间距 1
        // 线段A [(0,0)-(1,0)]，线段B [(2,0)-(3,0)]
        QTest::newRow("collinear_disjoint") << 0.0 << 0.0 << 1.0 << 0.0 << 2.0 << 0.0 << 3.0 << 0.0 << 1.0;
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

    void testNearestPointsOutput() {
        // P0-1: 延长线相交但线段不相交，验证最近点对输出
        // 线段A [(0,0)-(1,0)]，线段B [(3,-1)-(3,1)]，距离 2，最近点 (1,0) 与 (3,0)
        LinesDistancePlugin plugin;
        QVERIFY(plugin.initialize());

        QVector<QPointF> line1Points;
        line1Points << QPointF(0, 0) << QPointF(1, 0);
        QVector<QPointF> line2Points;
        line2Points << QPointF(3, -1) << QPointF(3, 1);

        ImageData input;
        input.setData("line1", QVariant::fromValue(line1Points));
        input.setData("line2", QVariant::fromValue(line2Points));

        ImageData output;
        QVERIFY(plugin.execute(input, output));

        QCOMPARE(output.data("distance").toDouble(), 2.0);
        QVERIFY(output.hasData("nearest_x1"));
        QVERIFY(output.hasData("nearest_y1"));
        QVERIFY(output.hasData("nearest_x2"));
        QVERIFY(output.hasData("nearest_y2"));
        // 最近点应为 (1,0) 与 (3,0)
        QCOMPARE(output.data("nearest_x1").toDouble(), 1.0);
        QCOMPARE(output.data("nearest_y1").toDouble(), 0.0);
        QCOMPARE(output.data("nearest_x2").toDouble(), 3.0);
        QCOMPARE(output.data("nearest_y2").toDouble(), 0.0);
    }

    void testIntersectingNearestPointsCoincide() {
        // P1-2: 交叉线段距离 0，最近点应为真实交点（两点重合）
        // A [(0,0)-(4,4)]，B [(0,4)-(4,0)]，交点 (2,2)
        double dist = 0;
        const double gap = runNearestGap({QPointF(0, 0), QPointF(4, 4)}, {QPointF(0, 4), QPointF(4, 0)}, dist);
        QVERIFY(gap >= 0);
        QCOMPARE(dist, 0.0);
        QVERIFY2(qAbs(gap - dist) < 1e-6,
                 qPrintable(QString("intersect nearest points must coincide; gap=%1").arg(gap)));
    }

    void testEndpointTouchingNearestPointsCoincide() {
        // P1-2: 端点接触距离 0，最近点重合于接触点 (1,0)
        double dist = 0;
        const double gap = runNearestGap({QPointF(0, 0), QPointF(1, 0)}, {QPointF(1, 0), QPointF(2, 1)}, dist);
        QVERIFY(gap >= 0);
        QCOMPARE(dist, 0.0);
        QVERIFY2(qAbs(gap) < 1e-6, "touching endpoints must coincide");
    }

    void testCollinearOverlapNearestPointsCoincide() {
        // P1-2: 共线重叠距离 0，最近点重合
        double dist = 0;
        const double gap = runNearestGap({QPointF(0, 0), QPointF(2, 0)}, {QPointF(1, 0), QPointF(3, 0)}, dist);
        QVERIFY(gap >= 0);
        QCOMPARE(dist, 0.0);
        QVERIFY2(qAbs(gap) < 1e-6, "collinear overlap nearest points must coincide");
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