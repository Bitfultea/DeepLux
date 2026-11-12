#include "core/model/ImageData.h"
#include "plugins/detection/FindCircle/FindCirclePlugin.h"

#include <QCoreApplication>
#include <QtTest>
#include <algorithm>
#include <cmath>

using namespace DeepLux;

class TestFindCircle : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestFindCircle Start ===";
    }

    void testPluginInitialization() {
        FindCirclePlugin plugin;
        QVERIFY(plugin.initialize());
        QVERIFY(plugin.isInitialized());
    }

    void testPluginInfo() {
        FindCirclePlugin plugin;

        QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.findcircle"));
        QCOMPARE(plugin.name(), QString("圆检测"));
        QCOMPARE(plugin.category(), QString("detection"));
        QVERIFY(!plugin.version().isEmpty());
    }

    void testProcessWithNoImage() {
        FindCirclePlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        ImageData output;

        // No image set - should fail
        bool result = plugin.execute(input, output);
        QVERIFY2(!result, "Should fail with no image");
    }

    void testCircleDetection_data() {
        QTest::addColumn<int>("imageSize");
        QTest::addColumn<int>("circleCenterX");
        QTest::addColumn<int>("circleCenterY");
        QTest::addColumn<int>("circleRadius");

        // Test 1: Small image with circle in center
        QTest::newRow("centered_circle_small") << 200 << 100 << 100 << 50;

        // Test 2: Larger image
        QTest::newRow("centered_circle_large") << 400 << 200 << 200 << 80;
    }

    void testCircleDetection() {
        QFETCH(int, imageSize);
        QFETCH(int, circleCenterX);
        QFETCH(int, circleCenterY);
        QFETCH(int, circleRadius);

#ifdef DEEPLUX_HAS_OPENCV
        // Create synthetic image with a white circle on black background
        cv::Mat image(imageSize, imageSize, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::circle(image, cv::Point(circleCenterX, circleCenterY), circleRadius, cv::Scalar(255, 255, 255),
                   -1); // Filled white circle

        FindCirclePlugin plugin;
        plugin.setParams(QJsonObject{{"minRadius", circleRadius / 2},
                                     {"maxRadius", circleRadius * 2},
                                     {"param1", 50.0},
                                     { "param2",
                                       30.0 }});

        QVERIFY(plugin.initialize());

        ImageData input;
        input.setMat(image);
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        provenance.sourceSize = QSize(imageSize * 2, imageSize * 3);
        const ImageCoordinateProvenance::Matrix3 localToSource{{ 1.5, 0.0, 7.0, 0.0, 2.0, 9.0, 0.0, 0.0, 1.0 }};
        QVERIFY(provenance.composeLocalToSource(localToSource));
        input.setCoordinateProvenance(provenance);

        ImageData output;
        bool result = plugin.execute(input, output);
        QVERIFY2(result, "Circle detection must succeed for the synthetic input");

        const double detectedCenterX = output.data("circle_center_x").toDouble();
        const double detectedCenterY = output.data("circle_center_y").toDouble();
        const double detectedRadius = output.data("circle_radius").toDouble();
        const Circle2D circle = output.data("circle").value<Circle2D>();

        QCOMPARE(output.width(), input.width());
        QCOMPARE(output.height(), input.height());
        QVERIFY(output.hasCoordinateProvenance());
        const ImageCoordinateProvenance outputProvenance = output.coordinateProvenance();
        QCOMPARE(outputProvenance.sourceImageId, provenance.sourceImageId);
        QCOMPARE(outputProvenance.sourceSize, provenance.sourceSize);
        QCOMPARE(outputProvenance.localToSourceMatrix(), provenance.localToSourceMatrix());
        for (const QPointF& point : {QPointF(0.0, 0.0), QPointF(imageSize - 1.0, imageSize - 1.0)}) {
            const auto expectedSource = provenance.mapLocalToSource(point);
            const auto actualSource = outputProvenance.mapLocalToSource(point);
            QVERIFY(expectedSource.has_value());
            QVERIFY(actualSource.has_value());
            QCOMPARE(actualSource->x(), expectedSource->x());
            QCOMPARE(actualSource->y(), expectedSource->y());
        }

        QVERIFY(output.data("circle").canConvert<Circle2D>());
        QVERIFY(circle.isValid());
        QCOMPARE(circle.centerX, detectedCenterX);
        QCOMPARE(circle.centerY, detectedCenterY);
        QCOMPARE(circle.radius, detectedRadius);
        const double tolerance = std::max(image.cols, image.rows) * 0.1;
        QVERIFY2(std::abs(detectedCenterX - circleCenterX) < tolerance, "Center X mismatch");
        QVERIFY2(std::abs(detectedCenterY - circleCenterY) < tolerance, "Center Y mismatch");
        QVERIFY2(std::abs(detectedRadius - circleRadius) < tolerance, "Radius mismatch");
        QVERIFY(!output.hasData("circle_radius_source"));
#else
        QSKIP("OpenCV not available");
#endif
    }

    void cleanupTestCase() {
        qDebug() << "=== TestFindCircle End ===";
    }
};

QTEST_MAIN(TestFindCircle)
#include "test_findcircle.moc"
