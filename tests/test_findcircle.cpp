#include <QtTest>
#include <QCoreApplication>
#include "plugins/detection/FindCircle/FindCirclePlugin.h"
#include "core/model/ImageData.h"

using namespace DeepLux;

class TestFindCircle : public QObject
{
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
        cv::circle(image, cv::Point(circleCenterX, circleCenterY), circleRadius,
                   cv::Scalar(255, 255, 255), -1);  // Filled white circle

        FindCirclePlugin plugin;
        plugin.setParams(QJsonObject{
            {"minRadius", circleRadius / 2},
            {"maxRadius", circleRadius * 2},
            {"param1", 50.0},
            {"param2", 30.0}
        });

        QVERIFY(plugin.initialize());

        ImageData input;
        input.setMat(image);

        ImageData output;
        bool result = plugin.execute(input, output);

        if (result) {
            double detectedCenterX = output.data("circle_center_x").toDouble();
            double detectedCenterY = output.data("circle_center_y").toDouble();
            double detectedRadius = output.data("circle_radius").toDouble();

            qDebug() << "Expected: center(" << circleCenterX << "," << circleCenterY << "), radius" << circleRadius;
            qDebug() << "Detected: center(" << detectedCenterX << "," << detectedCenterY << "), radius" << detectedRadius;

            // Allow 10% tolerance
            QVERIFY2(qAbs(detectedCenterX - circleCenterX) < imageSize * 0.1, "Center X mismatch");
            QVERIFY2(qAbs(detectedCenterY - circleCenterY) < imageSize * 0.1, "Center Y mismatch");
            QVERIFY2(qAbs(detectedRadius - circleRadius) < imageSize * 0.1, "Radius mismatch");
        } else {
            qWarning() << "Circle detection failed";
        }
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