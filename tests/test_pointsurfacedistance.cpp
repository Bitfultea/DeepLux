#include <QtTest>
#include "plugins/geometry/PointSurfaceDistance/PointSurfaceDistancePlugin.h"

using namespace DeepLux;

class TestPointSurfaceDistance : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qDebug() << "=== TestPointSurfaceDistance Start ===";
    }

    void pointAbovePlaneReturnsCorrectDistance()
    {
        PointSurfaceDistancePlugin plugin;
        QVERIFY(plugin.initialize());

        // point [0,0,5], plane [0,0,0, 1,0,0, 0,1,0] (xy-plane at z=0)
        ImageData input;
        input.setData("point", QVariantList{0.0, 0.0, 5.0});
        input.setData("plane", QVariantList{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0});

        ImageData output;
        QVERIFY(plugin.execute(input, output));

        QCOMPARE(output.data("distance").toDouble(), 5.0);
        QCOMPARE(output.data("foot_z").toDouble(), 0.0);
    }

    void pointAboveXyPlaneReturnsDistance3()
    {
        PointSurfaceDistancePlugin plugin;
        QVERIFY(plugin.initialize());

        // point [1,2,3], plane [0,0,0, 1,0,0, 0,1,0] (xy-plane at z=0)
        ImageData input;
        input.setData("point", QVariantList{1.0, 2.0, 3.0});
        input.setData("plane", QVariantList{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0});

        ImageData output;
        QVERIFY(plugin.execute(input, output));

        QVERIFY2(qAbs(output.data("distance").toDouble() - 3.0) < 0.001,
                 "Point at [1,2,3] above xy-plane should have distance 3");
        QVERIFY2(qAbs(output.data("foot_x").toDouble() - 1.0) < 0.001,
                 "Foot x should be 1.0");
        QVERIFY2(qAbs(output.data("foot_y").toDouble() - 2.0) < 0.001,
                 "Foot y should be 2.0");
        QVERIFY2(qAbs(output.data("foot_z").toDouble() - 0.0) < 0.001,
                 "Foot z should be 0.0");
    }

    void degeneratePlaneReturnsFalse()
    {
        PointSurfaceDistancePlugin plugin;
        QVERIFY(plugin.initialize());

        // Collinear points: [0,0,0, 1,1,1, 2,2,2]
        ImageData input;
        input.setData("point", QVariantList{0.0, 0.0, 0.0});
        input.setData("plane", QVariantList{0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 2.0, 2.0, 2.0});

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(!result, "Degenerate plane (collinear points) must be rejected");
    }

    void cleanupTestCase()
    {
        qDebug() << "=== TestPointSurfaceDistance End ===";
    }
};

QTEST_MAIN(TestPointSurfaceDistance)
#include "test_pointsurfacedistance.moc"
