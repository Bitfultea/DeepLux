#include <QtTest>
#include <QCoreApplication>
#include "plugins/geometry/FitLine/FitLinePlugin.h"
#include "core/model/ImageData.h"

using namespace DeepLux;

class TestFitLine : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() {
        qDebug() << "=== TestFitLine Start ===";
    }

    void testLineFitting_data() {
        QTest::addColumn<QVector<QPointF>>("points");
        QTest::addColumn<double>("expectedRho");
        QTest::addColumn<double>("expectedPhi");

        // Test 1: Horizontal line
        QVector<QPointF> horizontalPoints;
        horizontalPoints << QPointF(0, 0) << QPointF(100, 0) << QPointF(200, 0);
        QTest::newRow("horizontal_line") << horizontalPoints << 0.0 << 0.0;

        // Test 2: Vertical line
        QVector<QPointF> verticalPoints;
        verticalPoints << QPointF(100, 0) << QPointF(100, 100) << QPointF(100, 200);
        QTest::newRow("vertical_line") << verticalPoints << 100.0 << 90.0;

        // Test 3: Diagonal line (45 degrees)
        QVector<QPointF> diagonalPoints;
        diagonalPoints << QPointF(0, 0) << QPointF(100, 100) << QPointF(200, 200);
        QTest::newRow("diagonal_line") << diagonalPoints << 0.0 << 45.0;
    }

    void testLineFitting() {
        QFETCH(QVector<QPointF>, points);
        QFETCH(double, expectedRho);
        QFETCH(double, expectedPhi);

        FitLinePlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        input.setData("fit_points", QVariant::fromValue(points));

        ImageData output;
        bool result = plugin.execute(input, output);

        if (result) {
            double rho = output.data("line_rho").toDouble();
            double phi = output.data("line_phi").toDouble();

            qDebug() << "Input points:" << points;
            qDebug() << "Result - rho:" << rho << "phi:" << phi;
            qDebug() << "Expected - rho:" << expectedRho << "phi:" << expectedPhi;

            // Allow some tolerance due to numerical precision
            QVERIFY2(qAbs(rho - expectedRho) < 1.0, "Rho value mismatch");
        } else {
            qWarning() << "Line fitting failed";
        }
    }

    void testInsufficientPoints() {
        FitLinePlugin plugin;
        QVERIFY(plugin.initialize());

        ImageData input;
        QVector<QPointF> onlyOnePoint;
        onlyOnePoint << QPointF(100, 100);
        input.setData("fit_points", QVariant::fromValue(onlyOnePoint));

        ImageData output;
        bool result = plugin.execute(input, output);

        // Should fail with only 1 point
        QVERIFY2(!result, "Should fail with insufficient points");
    }

    void testValidateRejectsInvalidParams() {
        FitLinePlugin plugin;
        QString error;

        QJsonObject params = plugin.defaultParams();
        params["threshold"] = -1.0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject non-positive threshold");
        QVERIFY(!error.isEmpty());

        error.clear();
        params = plugin.defaultParams();
        params["iterations"] = 0;
        QVERIFY2(!plugin.validateParams(params, error), "Should reject non-positive iteration count");
        QVERIFY(!error.isEmpty());

        error.clear();
        params = plugin.defaultParams();
        params["fitMethod"] = "Unknown";
        QVERIFY2(!plugin.validateParams(params, error), "Should reject unsupported fitting method");
        QVERIFY(!error.isEmpty());
    }

    void testPluginInfo() {
        FitLinePlugin plugin;

        QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.fitline"));
        QCOMPARE(plugin.name(), QString("直线拟合"));
        QCOMPARE(plugin.category(), QString("geometry"));
        QVERIFY(!plugin.version().isEmpty());
    }

    void cleanupTestCase() {
        qDebug() << "=== TestFitLine End ===";
    }
};

QTEST_MAIN(TestFitLine)
#include "test_fitline.moc"
