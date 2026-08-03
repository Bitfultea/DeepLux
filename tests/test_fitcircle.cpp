#include "core/model/ImageData.h"
#include "plugins/geometry/FitCircle/FitCirclePlugin.h"

#include <QCoreApplication>
#include <QtTest>

using namespace DeepLux;

class TestFitCircle : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestFitCircle Start ===";
    }

    void testCircleFitting_data() {
        QTest::addColumn<QVector<QPointF>>("points");
        QTest::addColumn<double>("expectedRadius");
        QTest::addColumn<double>("tolerance");

        // Test 1: Circle with radius 50 centered at (100, 100)
        QVector<QPointF> circlePoints1;
        for (int i = 0; i < 8; ++i) {
            double angle = i * CV_PI / 4;
            circlePoints1 << QPointF(100 + 50 * cos(angle), 100 + 50 * sin(angle));
        }
        QTest::newRow("circle_r50") << circlePoints1 << 50.0 << 5.0;

        // Test 2: Circle with radius 100 centered at (200, 200)
        QVector<QPointF> circlePoints2;
        for (int i = 0; i < 12; ++i) {
            double angle = i * CV_PI / 6;
            circlePoints2 << QPointF(200 + 100 * cos(angle), 200 + 100 * sin(angle));
        }
        QTest::newRow("circle_r100") << circlePoints2 << 100.0 << 10.0;
    }

    void testCircleFitting() {
        QFETCH(QVector<QPointF>, points);
        QFETCH(double, expectedRadius);
        QFETCH(double, tolerance);

        FitCirclePlugin plugin;
        plugin.setParams(QJsonObject{{"minRadius", 1.0}, {"maxRadius", 500.0}, {"iterations", 100}});

        QVERIFY(plugin.initialize());

        ImageData input;
        input.setData("fit_points", QVariant::fromValue(points));

        ImageData output;
        bool result = plugin.execute(input, output);

        if (result) {
            double centerX = output.data("circle_center_x").toDouble();
            double centerY = output.data("circle_center_y").toDouble();
            double radius = output.data("circle_radius").toDouble();

            qDebug() << "Input points count:" << points.size();
            qDebug() << "Result - center: (" << centerX << "," << centerY << "), radius:" << radius;
            qDebug() << "Expected radius:" << expectedRadius << ", tolerance:" << tolerance;

            // Check radius is within tolerance
            QVERIFY2(qAbs(radius - expectedRadius) < tolerance,
                     QString("Radius %1 not within tolerance of %2").arg(radius).arg(expectedRadius).toUtf8());
        } else {
            qWarning() << "Circle fitting failed";
            QFAIL("Circle fitting should succeed with valid points");
        }
    }

    void testInsufficientPoints() {
        FitCirclePlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        QVector<QPointF> onlyTwoPoints;
        onlyTwoPoints << QPointF(100, 100) << QPointF(150, 150);
        input.setData("fit_points", QVariant::fromValue(onlyTwoPoints));

        ImageData output;
        bool result = plugin.execute(input, output);

        // Should fail with only 2 points
        QVERIFY2(!result, "Should fail with insufficient points");
    }

    void testValidateRejectsInvalidParams() {
        FitCirclePlugin plugin;
        QString error;

        QJsonObject params = plugin.defaultParams();
        params["threshold"] = 0.0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject non-positive threshold");
        QVERIFY(!error.isEmpty());

        error.clear();
        params = plugin.defaultParams();
        params["iterations"] = 0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject non-positive iteration count");
        QVERIFY(!error.isEmpty());

        error.clear();
        params = plugin.defaultParams();
        params["minRadius"] = 0.0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject non-positive minimum radius");
        QVERIFY(!error.isEmpty());

        error.clear();
        params = plugin.defaultParams();
        params["minRadius"] = 50.0;
        params["maxRadius"] = 50.0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject radius range without positive span");
        QVERIFY(!error.isEmpty());
    }

    void testRansacRejectsOutliers() {
        FitCirclePlugin plugin;
        QVERIFY(plugin.initialize());
        plugin.setParams(
            QJsonObject{{"threshold", 1.0}, {"iterations", 300}, {"minRadius", 1.0}, {"maxRadius", 100.0}});

        QVector<QPointF> points;
        for (int i = 0; i < 16; ++i) {
            const double angle = i * CV_PI / 8;
            points.append(QPointF(50 + 20 * cos(angle), 50 + 20 * sin(angle)));
        }
        points << QPointF(0, 0) << QPointF(100, 0) << QPointF(0, 100) << QPointF(100, 100);

        ImageData input;
        input.setData("fit_points", QVariant::fromValue(points));
        ImageData output;
        QVERIFY(plugin.execute(input, output));
        QVERIFY2(qAbs(output.data("circle_radius").toDouble() - 20.0) < 1.0,
                 "RANSAC should fit the inlier circle instead of the outliers");
    }

    void testPluginInfo() {
        FitCirclePlugin plugin;

        QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.fitcircle"));
        QCOMPARE(plugin.name(), QString("圆拟合"));
        QCOMPARE(plugin.category(), QString("geometry"));
        QVERIFY(!plugin.version().isEmpty());
    }

    void cleanupTestCase() {
        qDebug() << "=== TestFitCircle End ===";
    }
};

QTEST_MAIN(TestFitCircle)
#include "test_fitcircle.moc"
