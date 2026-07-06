#include <QtTest/QtTest>
#include <QTemporaryDir>
#include "core/geometry/MeasurementData.h"
#include "display/DisplayData.h"
#include "plugins/image_processing/LoadPointCloud/LoadPointCloudPlugin.h"

using namespace DeepLux;

class TestLoadPointCloud : public QObject {
    Q_OBJECT
private slots:
    void loadsPlyFromTempFile();
    void missingFileReturnsFalse();
};

void TestLoadPointCloud::loadsPlyFromTempFile() {
    // Create a temporary ASCII PLY file
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString plyPath = dir.filePath("test.ply");
    QFile ply(plyPath);
    QVERIFY(ply.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&ply);
    out << "ply\n";
    out << "format ascii 1.0\n";
    out << "element vertex 2\n";
    out << "property float x\n";
    out << "property float y\n";
    out << "property float z\n";
    out << "end_header\n";
    out << "1.0 2.0 3.0\n";
    out << "4.0 5.0 6.0\n";
    ply.close();

    LoadPointCloudPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParam("filePath", plyPath);

    ImageData input, output;
    QVERIFY(plugin.execute(input, output));

    auto cloud = MeasurementData::pointCloud(output, nullptr);
    QVERIFY(cloud.has_value());
    QCOMPARE(static_cast<int>(cloud->points.size()), 2);
    QCOMPARE(output.data("point_count").toInt(), 2);
    QCOMPARE(output.data("point_cloud_path").toString(), plyPath);
}

void TestLoadPointCloud::missingFileReturnsFalse() {
    LoadPointCloudPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParam("filePath", "/nonexistent/cloud.ply");
    ImageData input, output;
    QVERIFY(!plugin.execute(input, output));
}

QTEST_MAIN(TestLoadPointCloud)
#include "test_loadpointcloud.moc"
