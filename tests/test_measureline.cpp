#include <QtTest>
#include <QCoreApplication>
#include "plugins/detection/MeasureLine/MeasureLinePlugin.h"
#include "core/model/ImageData.h"

using namespace DeepLux;

class TestMeasureLine : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestMeasureLine Start ===";
    }

    void testPluginInitialization() {
        MeasureLinePlugin plugin;
        QVERIFY(plugin.initialize());
        QVERIFY(plugin.isInitialized());
    }

    void testPluginInfo() {
        MeasureLinePlugin plugin;

        QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.measureline"));
        QCOMPARE(plugin.name(), QString("线条测量"));
        QCOMPARE(plugin.category(), QString("detection"));
        QVERIFY(!plugin.version().isEmpty());
    }

    void testProcessWithNoImage() {
        MeasureLinePlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        ImageData output;

        // No image set - should fail
        bool result = plugin.execute(input, output);
        QVERIFY2(!result, "Should fail with no image");
    }

    void testValidateRejectsInvalidParams() {
        MeasureLinePlugin plugin;
        QString error;

        QJsonObject params = plugin.defaultParams();
        params["threshold"] = 0.0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject non-positive threshold");
        QVERIFY(!error.isEmpty());

        error.clear();
        params = plugin.defaultParams();
        params["minLength"] = 0.0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject non-positive minimum length");
        QVERIFY(!error.isEmpty());

        error.clear();
        params = plugin.defaultParams();
        params["minLength"] = 100.0;
        params["maxLength"] = 100.0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject length range without positive span");
        QVERIFY(!error.isEmpty());

        error.clear();
        params = plugin.defaultParams();
        params["minAngle"] = 45.0;
        params["maxAngle"] = 30.0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject inverted angle range");
        QVERIFY(!error.isEmpty());

        error.clear();
        params = plugin.defaultParams();
        params["minAngle"] = -1.0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject angle outside 0-180 degrees");
        QVERIFY(!error.isEmpty());
    }

    void testLineDetection_data() {
        QTest::addColumn<int>("imageSize");
        QTest::addColumn<int>("x1");
        QTest::addColumn<int>("y1");
        QTest::addColumn<int>("x2");
        QTest::addColumn<int>("y2");

        // Test 1: Horizontal line
        QTest::newRow("horizontal_line") << 200 << 20 << 100 << 180 << 100;

        // Test 2: Vertical line
        QTest::newRow("vertical_line") << 200 << 100 << 20 << 100 << 180;

        // Test 3: Diagonal line
        QTest::newRow("diagonal_line") << 200 << 20 << 20 << 180 << 180;
    }

    void testLineDetection() {
        QFETCH(int, imageSize);
        QFETCH(int, x1);
        QFETCH(int, y1);
        QFETCH(int, x2);
        QFETCH(int, y2);

    #ifdef DEEPLUX_HAS_OPENCV
        // Create synthetic image with a white line on black background
        cv::Mat image(imageSize, imageSize, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::line(image, cv::Point(x1, y1), cv::Point(x2, y2),
                 cv::Scalar(255, 255, 255), 2);  // White line

        MeasureLinePlugin plugin;
        plugin.setParams(QJsonObject{
            {"minLength", 10.0},
            {"maxLength", 500.0},
            {"threshold", 30.0}
        });

        QVERIFY(plugin.initialize());

        ImageData input;
        input.setMat(image);

        ImageData output;
        bool result = plugin.execute(input, output);

        if (result) {
            double detectedLength = output.data("line_length").toDouble();
            double detectedAngle = output.data("line_angle").toDouble();
            int lineCount = output.data("line_count").toInt();

            // Calculate expected length
            double expectedLength = sqrt(pow(x2 - x1, 2) + pow(y2 - y1, 2));

            qDebug() << "Expected: length=" << expectedLength << ", endpoints: (" << x1 << "," << y1 << ") - (" << x2 << "," << y2 << ")";
            qDebug() << "Detected: length=" << detectedLength << ", angle=" << detectedAngle << ", count=" << lineCount;

            // Allow 15% tolerance
            QVERIFY2(qAbs(detectedLength - expectedLength) < imageSize * 0.15, "Length mismatch");
            QVERIFY(lineCount >= 1);
        } else {
            qWarning() << "Line detection failed";
        }
    #else
        QSKIP("OpenCV not available");
    #endif
    }

    void cleanupTestCase() {
        qDebug() << "=== TestMeasureLine End ===";
    }
};

QTEST_MAIN(TestMeasureLine)
#include "test_measureline.moc"
