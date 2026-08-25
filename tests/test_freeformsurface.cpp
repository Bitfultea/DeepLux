#include "core/geometry/MeasurementData.h"
#include "plugins/geometry/FreeformSurface/FreeformSurfacePlugin.h"

#include <QtTest>

using namespace DeepLux;

class TestFreeformSurface : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestFreeformSurface Start ===";
    }

    void processesPointCloudFromMeasurementData() {
        FreeformSurfacePlugin plugin;
        QVERIFY(plugin.initialize());

        // Build a PointCloudData with 4 points
        PointCloudData cloud;
        cloud.points.push_back(Eigen::Vector3d(0.0, 0.0, 0.0));
        cloud.points.push_back(Eigen::Vector3d(1.0, 0.0, 0.5));
        cloud.points.push_back(Eigen::Vector3d(0.0, 1.0, 0.3));
        cloud.points.push_back(Eigen::Vector3d(1.0, 1.0, 0.8));

        ImageData input;
        MeasurementData::setPointCloud(input, cloud);

        ImageData output;
        bool result = plugin.execute(input, output);

        QVERIFY2(result, "FreeformSurface should succeed with valid PointCloudData");
        QCOMPARE(output.data("point_count").toInt(), 4);
        QVERIFY2(output.data("surface_roughness").toDouble() >= 0.0, "surface_roughness must be non-negative");
    }

    // P0-2: 乱序单位矩形的四个角点——面积应为 1，且与输入顺序无关（确定性）
    void areaIsOrderIndependentForShuffledRectangle() {
        FreeformSurfacePlugin plugin;
        QVERIFY(plugin.initialize());

        // 单位正方形四角（z=0 平面，面积应为 1）
        std::vector<Eigen::Vector3d> corners = {
            {0.0, 0.0, 0.0},
            {1.0, 0.0, 0.0},
            {1.0, 1.0, 0.0},
            {0.0, 1.0, 0.0},
        };
        // 两种不同顺序（含乱序）
        std::vector<std::vector<int>> orders = {
            {0, 1, 2, 3}, // 顺序
            {2, 0, 3, 1}, // 乱序
        };

        double firstArea = -1.0;
        for (const auto& order : orders) {
            PointCloudData cloud;
            for (int idx : order)
                cloud.points.push_back(corners[idx]);
            ImageData input;
            MeasurementData::setPointCloud(input, cloud);
            ImageData output;
            QVERIFY(plugin.execute(input, output));
            double area = output.data("surface_area").toDouble();
            qDebug() << "order area:" << area;
            QVERIFY2(qAbs(area - 1.0) < 0.01, QString("unit square area should be 1, got %1").arg(area).toUtf8());
            if (firstArea < 0)
                firstArea = area;
            else
                QVERIFY2(qAbs(area - firstArea) < 0.01, "area must be order-independent");
        }
    }

    // P0-2: 共线点凸包不足 3 点，面积应为 0
    void collinearPointsYieldZeroArea() {
        FreeformSurfacePlugin plugin;
        QVERIFY(plugin.initialize());

        PointCloudData cloud;
        for (int i = 0; i < 5; ++i)
            cloud.points.push_back(Eigen::Vector3d(static_cast<double>(i), 0.0, 0.0));

        ImageData input;
        MeasurementData::setPointCloud(input, cloud);
        ImageData output;
        QVERIFY(plugin.execute(input, output));
        QCOMPARE(output.data("point_count").toInt(), 5);
        QVERIFY2(output.data("surface_area").toDouble() < 0.01, "collinear points must yield ~0 area");
    }

    // P0-2: 重复执行结果一致（无状态残留）
    void repeatedExecutionIsConsistent() {
        FreeformSurfacePlugin plugin;
        QVERIFY(plugin.initialize());

        PointCloudData cloud;
        cloud.points.push_back(Eigen::Vector3d(0.0, 0.0, 0.0));
        cloud.points.push_back(Eigen::Vector3d(2.0, 0.0, 0.0));
        cloud.points.push_back(Eigen::Vector3d(2.0, 3.0, 0.0));
        cloud.points.push_back(Eigen::Vector3d(0.0, 3.0, 0.0));

        double prevArea = -1.0;
        for (int i = 0; i < 3; ++i) {
            ImageData input;
            MeasurementData::setPointCloud(input, cloud);
            ImageData output;
            QVERIFY(plugin.execute(input, output));
            double area = output.data("surface_area").toDouble();
            if (prevArea >= 0)
                QCOMPARE(area, prevArea);
            prevArea = area;
        }
        // 2x3 矩形面积应为 6
        QVERIFY2(qAbs(prevArea - 6.0) < 0.01, QString("2x3 rectangle area should be 6, got %1").arg(prevArea).toUtf8());
    }

    // P0-2: 点数不足 3 时结果重置为 0，不沿用上次
    void fewerThanThreePointsResetsResults() {
        FreeformSurfacePlugin plugin;
        QVERIFY(plugin.initialize());

        // 先跑一次有效矩形，得到非零面积
        PointCloudData good;
        good.points.push_back(Eigen::Vector3d(0.0, 0.0, 0.0));
        good.points.push_back(Eigen::Vector3d(1.0, 0.0, 0.0));
        good.points.push_back(Eigen::Vector3d(1.0, 1.0, 0.0));
        good.points.push_back(Eigen::Vector3d(0.0, 1.0, 0.0));
        ImageData in1;
        MeasurementData::setPointCloud(in1, good);
        ImageData out1;
        QVERIFY(plugin.execute(in1, out1));
        QVERIFY(out1.data("surface_area").toDouble() > 0.5);

        // 再跑一次只有 2 个点的点云——面积应重置为 0，不沿用上次
        PointCloudData few;
        few.points.push_back(Eigen::Vector3d(0.0, 0.0, 0.0));
        few.points.push_back(Eigen::Vector3d(1.0, 1.0, 1.0));
        ImageData in2;
        MeasurementData::setPointCloud(in2, few);
        ImageData out2;
        QVERIFY(plugin.execute(in2, out2));
        QCOMPARE(out2.data("point_count").toInt(), 2);
        QCOMPARE(out2.data("surface_area").toDouble(), 0.0);
        QCOMPARE(out2.data("surface_roughness").toDouble(), 0.0);
    }

    void cleanupTestCase() {
        qDebug() << "=== TestFreeformSurface End ===";
    }
};

QTEST_MAIN(TestFreeformSurface)
#include "test_freeformsurface.moc"
