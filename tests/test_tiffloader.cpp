#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/io/TiffLoader.h>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#endif

using namespace DeepLux;

class TestTiffLoader : public QObject {
    Q_OBJECT

private slots:
    void testRgb16TiffKeepsColorDepth();
};

void TestTiffLoader::testRgb16TiffKeepsColorDepth() {
#ifndef DEEPLUX_HAS_OPENCV
    QSKIP("OpenCV not available");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath("rgb16.tiff");
    cv::Mat img(1, 1, CV_16UC3);
    img.at<cv::Vec<unsigned short, 3>>(0, 0) = cv::Vec<unsigned short, 3>(0, 32768, 65535);
    QVERIFY(cv::imwrite(path.toStdString(), img));

    PointCloudData data;
    QString error;
    QVERIFY2(TiffLoader::load(path, data, error), qPrintable(error));

    QCOMPARE(static_cast<int>(data.points.size()), 1);
    QVERIFY(data.hasColors());
    QCOMPARE(static_cast<int>(data.colors.size()), 1);

    QVERIFY(qAbs(data.colors[0].x() - 1.0) < 1e-6);
    QVERIFY(qAbs(data.colors[0].y() - (32768.0 / 65535.0)) < 1e-6);
    QVERIFY(qAbs(data.colors[0].z()) < 1e-6);
#endif
}

QTEST_MAIN(TestTiffLoader)
#include "test_tiffloader.moc"
