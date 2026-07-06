#include <QtTest>
#include "plugins/geometry/MeasureGap/MeasureGapPlugin.h"

using namespace DeepLux;

class TestMeasureGap : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qDebug() << "=== TestMeasureGap Start ===";
    }

    void calculates3DGap()
    {
        MeasureGapPlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        input.setData("point1", QVariantList{0.0, 0.0, 0.0});
        input.setData("point2", QVariantList{3.0, 4.0, 12.0});

        ImageData output;
        QVERIFY(plugin.execute(input, output));

        QCOMPARE(output.data("gap_distance").toDouble(), 13.0);
        QCOMPARE(output.data("gap_delta_z").toDouble(), 12.0);
        QCOMPARE(output.data("measurement_dimension").toString(), QString("3d"));
    }

    void treats2DPointAsZ0()
    {
        MeasureGapPlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        input.setData("point1", QVariantList{0.0, 0.0});
        input.setData("point2", QVariantList{3.0, 4.0});

        ImageData output;
        QVERIFY(plugin.execute(input, output));

        QCOMPARE(output.data("gap_distance").toDouble(), 5.0);
        QCOMPARE(output.data("gap_delta_z").toDouble(), 0.0);
        QCOMPARE(output.data("measurement_dimension").toString(), QString("2d"));
    }

    void cleanupTestCase()
    {
        qDebug() << "=== TestMeasureGap End ===";
    }
};

QTEST_MAIN(TestMeasureGap)
#include "test_measuregap.moc"
