#include <QDataStream>
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
    void testTruncatedAsciiFailsWithoutPartialCloud();
    void testTruncatedBinaryFailsWithoutPartialCloud();
    void testCompleteBinaryLoads();
    void testMalformedNonVertexElementIsRejected();
    void testFloat32OverflowIsRejected();
    void testTrailingElementPayloadMustBeComplete();
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

void TestPlyLoader::testTruncatedAsciiFailsWithoutPartialCloud() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("truncated-ascii.ply"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\n"
               "end_header\n0 1 2\n");
    file.close();

    PointCloudData data;
    data.points.push_back(Eigen::Vector3d(99, 99, 99));
    QString error;
    QVERIFY(!PlyLoader::load(path, data, error));
    QVERIFY(error.contains(QStringLiteral("truncated")));
    QVERIFY(data.points.empty());
}

void TestPlyLoader::testTruncatedBinaryFailsWithoutPartialCloud() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("truncated-binary.ply"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("ply\nformat binary_little_endian 1.0\nelement vertex 2\nproperty float x\nproperty float y\n"
               "property float z\nend_header\n");
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << 1.0f << 2.0f << 3.0f << 4.0f;
    file.close();

    PointCloudData data;
    data.points.push_back(Eigen::Vector3d(99, 99, 99));
    QString error;
    QVERIFY(!PlyLoader::load(path, data, error));
    QVERIFY(error.contains(QStringLiteral("truncated")));
    QVERIFY(data.points.empty());
}

void TestPlyLoader::testCompleteBinaryLoads() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("complete-binary.ply"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("ply\nformat binary_little_endian 1.0\nelement vertex 2\nproperty float x\nproperty float y\n"
               "property float z\nend_header\n");
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << 1.0f << 2.0f << 3.0f << 4.0f << 5.0f << 6.0f;
    file.close();

    PointCloudData data;
    QString error;
    QVERIFY2(PlyLoader::load(path, data, error), qPrintable(error));
    QCOMPARE(static_cast<int>(data.points.size()), 2);
    QCOMPARE(data.points[0].x(), 1.0);
    QCOMPARE(data.points[1].z(), 6.0);
}

void TestPlyLoader::testMalformedNonVertexElementIsRejected() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("malformed-element.ply"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\n"
               "element face nope\nproperty list uchar int vertex_indices\nend_header\n0 1 2\n");
    file.close();

    PointCloudData data;
    QString error;
    QVERIFY(!PlyLoader::load(path, data, error));
    QVERIFY(error.contains(QStringLiteral("element")));
    QVERIFY(data.points.empty());
}

void TestPlyLoader::testFloat32OverflowIsRejected() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("float32-overflow.ply"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float32 x\nproperty float y\nproperty float z\n"
               "end_header\n1e300 1 2\n");
    file.close();

    PointCloudData data;
    QString error;
    QVERIFY(!PlyLoader::load(path, data, error));
    QVERIFY(error.contains(QStringLiteral("invalid scalar")));
    QVERIFY(data.points.empty());
}

void TestPlyLoader::testTrailingElementPayloadMustBeComplete() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray header = QByteArrayLiteral(
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_indices\nend_header\n");
    const QString validPath = dir.filePath(QStringLiteral("complete-face.ply"));
    QFile validFile(validPath);
    QVERIFY(validFile.open(QIODevice::WriteOnly | QIODevice::Text));
    validFile.write(header + QByteArrayLiteral("0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n"));
    validFile.close();

    PointCloudData data;
    QString error;
    QVERIFY2(PlyLoader::load(validPath, data, error), qPrintable(error));
    QCOMPARE(static_cast<int>(data.points.size()), 3);

    const QString truncatedPath = dir.filePath(QStringLiteral("truncated-face.ply"));
    QFile truncatedFile(truncatedPath);
    QVERIFY(truncatedFile.open(QIODevice::WriteOnly | QIODevice::Text));
    truncatedFile.write(header + QByteArrayLiteral("0 0 0\n1 0 0\n0 1 0\n"));
    truncatedFile.close();

    QVERIFY(!PlyLoader::load(truncatedPath, data, error));
    QVERIFY(error.contains(QStringLiteral("truncated")) || error.contains(QStringLiteral("malformed")));
    QVERIFY(data.points.empty());
}

QTEST_MAIN(TestPlyLoader)
#include "test_plyloader.moc"
