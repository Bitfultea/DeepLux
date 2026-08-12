#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/io/TiffLoader.h>
#include <limits>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#endif

using namespace DeepLux;

class TestTiffLoader : public QObject {
    Q_OBJECT

private slots:
    void testRgb16TiffKeepsColorDepth();
    void testFloatTiffFiltersNoDataWithoutChangingGeometry();
    void testNegativeAndFlatHeightsRemainValid();
    void testExplicitValidRange();
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

void TestTiffLoader::testFloatTiffFiltersNoDataWithoutChangingGeometry() {
#ifndef DEEPLUX_HAS_OPENCV
    QSKIP("OpenCV not available");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath("height_with_nodata.tiff");
    cv::Mat img(5, 5, CV_32F);
    const float noData = -21474836.0f;
    const float values[] = {noData, noData, noData, noData, noData,
                            noData, noData, noData, noData, noData,
                            noData, noData, noData, noData, noData,
                            noData, noData, 1.0f,   2.0f,   3.0f,
                            4.0f,   5.0f,   6.0f,   7.0f,   std::numeric_limits<float>::quiet_NaN()};
    std::copy(std::begin(values), std::end(values), img.ptr<float>());
    QVERIFY(cv::imwrite(path.toStdString(), img));

    TiffLoader::Config config;
    config.scaleX = 2.0f;
    config.scaleY = 3.0f;
    config.scaleZ = 2.0f;
    config.offsetZ = 10.0f;

    PointCloudData data;
    QString error;
    QVERIFY2(TiffLoader::load(path, data, error, config), qPrintable(error));
    QCOMPARE(static_cast<int>(data.points.size()), 7);
    QCOMPARE(data.points.front(), Eigen::Vector3d(4.0, 9.0, 12.0));
    QCOMPARE(data.points.back(), Eigen::Vector3d(6.0, 12.0, 24.0));
#endif
}

void TestTiffLoader::testNegativeAndFlatHeightsRemainValid() {
#ifndef DEEPLUX_HAS_OPENCV
    QSKIP("OpenCV not available");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString negativePath = dir.filePath("negative.tiff");
    cv::Mat negative(1, 4, CV_32F);
    const float negativeValues[] = {-4.0f, -3.0f, -2.0f, -1.0f};
    std::copy(std::begin(negativeValues), std::end(negativeValues), negative.ptr<float>());
    QVERIFY(cv::imwrite(negativePath.toStdString(), negative));

    PointCloudData data;
    QString error;
    QVERIFY2(TiffLoader::load(negativePath, data, error), qPrintable(error));
    QCOMPARE(static_cast<int>(data.points.size()), 4);
    QCOMPARE(data.points.front().z(), -4.0);
    QCOMPARE(data.points.back().z(), -1.0);

    const QString flatPath = dir.filePath("flat.tiff");
    cv::Mat flat(2, 2, CV_32F, cv::Scalar(-7.5f));
    QVERIFY(cv::imwrite(flatPath.toStdString(), flat));
    QVERIFY2(TiffLoader::load(flatPath, data, error), qPrintable(error));
    QCOMPARE(static_cast<int>(data.points.size()), 4);
    QCOMPARE(data.points.front().z(), -7.5);

    const QString sparseExtremePath = dir.filePath("sparse_extreme.tiff");
    cv::Mat sparseExtreme(1, 20, CV_32F);
    for (int x = 0; x < sparseExtreme.cols; ++x) {
        sparseExtreme.at<float>(0, x) = static_cast<float>(x);
    }
    sparseExtreme.at<float>(0, 0) = -100000000.0f;
    QVERIFY(cv::imwrite(sparseExtremePath.toStdString(), sparseExtreme));
    QVERIFY2(TiffLoader::load(sparseExtremePath, data, error), qPrintable(error));
    QCOMPARE(static_cast<int>(data.points.size()), 20);
    QCOMPARE(data.points.front().z(), -100000000.0);
#endif
}

void TestTiffLoader::testExplicitValidRange() {
#ifndef DEEPLUX_HAS_OPENCV
    QSKIP("OpenCV not available");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath("valid_range.tiff");
    cv::Mat img(1, 5, CV_16U);
    const unsigned short values[] = {0, 10, 20, 30, 65535};
    std::copy(std::begin(values), std::end(values), img.ptr<unsigned short>());
    QVERIFY(cv::imwrite(path.toStdString(), img));

    TiffLoader::Config config;
    config.validMin = 10.0;
    config.validMax = 30.0;

    PointCloudData data;
    QString error;
    QVERIFY2(TiffLoader::load(path, data, error, config), qPrintable(error));
    QCOMPARE(static_cast<int>(data.points.size()), 3);
    QCOMPARE(data.points.front().z(), 10.0);
    QCOMPARE(data.points.back().z(), 30.0);
#endif
}

QTEST_MAIN(TestTiffLoader)
#include "test_tiffloader.moc"
