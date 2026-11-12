#include "plugins/geometry/MeasureGap/MeasureGapPlugin.h"

#include <QtTest>
#include <limits>

using namespace DeepLux;

class TestMeasureGap : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestMeasureGap Start ===";
    }

    void calculates3DGap() {
        MeasureGapPlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        input.setData("point1", QVariantList{0.0, 0.0, 0.0});
        input.setData("point2", QVariantList{3.0, 4.0, 12.0});

        ImageData output;
        QVERIFY(plugin.execute(input, output));

        QCOMPARE(output.data("gap_distance").toDouble(), 13.0);
        QCOMPARE(output.data("gap").toDouble(), 13.0);
        QCOMPARE(output.data("gap_delta_z").toDouble(), 12.0);
        QCOMPARE(output.data("measurement_dimension").toString(), QString("3d"));
    }

    void treats2DPointAsZ0() {
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

    void rejectsNonFiniteCoordinates() {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (int coordinate = 0; coordinate < 6; ++coordinate) {
            const double invalid = coordinate % 2 == 0 ? nan : inf;
            QVariantList point1{0.0, 0.0, 0.0};
            QVariantList point2{1.0, 1.0, 1.0};
            if (coordinate < 3)
                point1[coordinate] = invalid;
            else
                point2[coordinate - 3] = invalid;

            MeasureGapPlugin plugin;
            QVERIFY(plugin.initialize());
            ImageData input;
            input.setData("point1", point1);
            input.setData("point2", point2);

            ImageData output;
            QVERIFY(!plugin.execute(input, output));
            QVERIFY(!output.data("gap_distance").isValid());
            QVERIFY(!output.data("gap").isValid());
        }
    }

    void rejectsMalformedPointShape() {
        MeasureGapPlugin plugin;
        QVERIFY(plugin.initialize());
        ImageData input;
        input.setData("point1", QVariantList{0.0, 0.0, 0.0, 1.0});
        input.setData("point2", QVariantList{1.0, 1.0});

        ImageData output;
        QVERIFY(!plugin.execute(input, output));
        QVERIFY(!output.data("gap_distance").isValid());
    }

    void cleanupTestCase() {
        qDebug() << "=== TestMeasureGap End ===";
    }
};

QTEST_MAIN(TestMeasureGap)
#include "test_measuregap.moc"
