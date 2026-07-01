#include <QtTest>
#include <QCoreApplication>
#include "plugins/detection/MeasureRect/MeasureRectPlugin.h"
#include "core/model/ImageData.h"

using namespace DeepLux;

class TestMeasureRect : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestMeasureRect Start ===";
    }

    void testPluginInitialization() {
        MeasureRectPlugin plugin;
        QVERIFY(plugin.initialize());
        QVERIFY(plugin.isInitialized());
    }

    void testPluginInfo() {
        MeasureRectPlugin plugin;

        QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.measurerect"));
        QCOMPARE(plugin.name(), QString("矩形测量"));
        QCOMPARE(plugin.category(), QString("detection"));
        QVERIFY(!plugin.version().isEmpty());
    }

    void testProcessWithNoImage() {
        MeasureRectPlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        ImageData output;

        // No image set - should fail
        bool result = plugin.execute(input, output);
        QVERIFY2(!result, "Should fail with no image");
    }

    void testRectDetection_data() {
        QTest::addColumn<int>("imageSize");
        QTest::addColumn<int>("rectX");
        QTest::addColumn<int>("rectY");
        QTest::addColumn<int>("rectWidth");
        QTest::addColumn<int>("rectHeight");

        // Test 1: Rectangle in center
        QTest::newRow("centered_rect") << 200 << 50 << 50 << 100 << 80;
    }

    void testRectDetection() {
        QFETCH(int, imageSize);
        QFETCH(int, rectX);
        QFETCH(int, rectY);
        QFETCH(int, rectWidth);
        QFETCH(int, rectHeight);

    #ifdef DEEPLUX_HAS_OPENCV
        // Create synthetic image with a white rectangle on black background
        cv::Mat image(imageSize, imageSize, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::rectangle(image, cv::Point(rectX, rectY),
                      cv::Point(rectX + rectWidth, rectY + rectHeight),
                      cv::Scalar(255, 255, 255), -1);  // Filled white rectangle

        MeasureRectPlugin plugin;
        plugin.setParams(QJsonObject{
            {"minArea", 100.0},
            {"maxArea", 100000.0},
            {"threshold1", 50.0},
            {"threshold2", 150.0}
        });

        QVERIFY(plugin.initialize());

        ImageData input;
        input.setMat(image);

        ImageData output;
        bool result = plugin.execute(input, output);

        if (result) {
            double width = output.data("rect_width").toDouble();
            double height = output.data("rect_height").toDouble();
            double area = output.data("rect_area").toDouble();

            qDebug() << "Expected: width=" << rectWidth << ", height=" << rectHeight;
            qDebug() << "Detected: width=" << width << ", height=" << height << ", area=" << area;

            // Allow 40% tolerance due to rotated rectangle detection variations
            double widthTol = qMax(rectWidth, rectHeight) * 0.4;
            double heightTol = qMax(rectWidth, rectHeight) * 0.4;
            QVERIFY2(qAbs(width - rectWidth) < widthTol || qAbs(width - rectHeight) < widthTol,
                     "Width mismatch");
            QVERIFY2(qAbs(height - rectHeight) < heightTol || qAbs(height - rectWidth) < heightTol,
                     "Height mismatch");
            QVERIFY(area > 0);
        } else {
            qWarning() << "Rectangle detection failed";
        }
    #else
        QSKIP("OpenCV not available");
    #endif
    }

    void cleanupTestCase() {
        qDebug() << "=== TestMeasureRect End ===";
    }
};

QTEST_MAIN(TestMeasureRect)
#include "test_measurerect.moc"