#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest/QtTest>
#include <core/io/PlyLoader.h>

using namespace DeepLux;

class TestPlyLoader : public QObject {
    Q_OBJECT

private slots:
    void testAsciiRgbPropertiesPopulateColors();
};

void TestPlyLoader::testAsciiRgbPropertiesPopulateColors() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath("rgb.ply");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));

    QTextStream out(&file);
    out << "ply\n"
        << "format ascii 1.0\n"
        << "element vertex 2\n"
        << "property float x\n"
        << "property float y\n"
        << "property float z\n"
        << "property uchar red\n"
        << "property uchar green\n"
        << "property uchar blue\n"
        << "end_header\n"
        << "0 1 2 255 128 0\n"
        << "3 4 5 0 64 255\n";
    file.close();

    PointCloudData data;
    QString error;
    QVERIFY2(PlyLoader::load(path, data, error), qPrintable(error));

    QCOMPARE(static_cast<int>(data.points.size()), 2);
    QVERIFY(data.hasColors());
    QCOMPARE(static_cast<int>(data.colors.size()), 2);

    QVERIFY(qAbs(data.colors[0].x() - 1.0) < 1e-6);
    QVERIFY(qAbs(data.colors[0].y() - (128.0 / 255.0)) < 1e-6);
    QVERIFY(qAbs(data.colors[0].z()) < 1e-6);
    QVERIFY(qAbs(data.colors[1].x()) < 1e-6);
    QVERIFY(qAbs(data.colors[1].y() - (64.0 / 255.0)) < 1e-6);
    QVERIFY(qAbs(data.colors[1].z() - 1.0) < 1e-6);
}

QTEST_MAIN(TestPlyLoader)
#include "test_plyloader.moc"
