#include <QtTest>
#include "plugins/geometry/FreeformSurface/FreeformSurfacePlugin.h"
#include "core/geometry/MeasurementData.h"

using namespace DeepLux;

class TestFreeformSurface : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qDebug() << "=== TestFreeformSurface Start ===";
    }

    void processesPointCloudFromMeasurementData()
    {
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
        QVERIFY2(output.data("surface_roughness").toDouble() >= 0.0,
                 "surface_roughness must be non-negative");
    }

    void cleanupTestCase()
    {
        qDebug() << "=== TestFreeformSurface End ===";
    }
};

QTEST_MAIN(TestFreeformSurface)
#include "test_freeformsurface.moc"
