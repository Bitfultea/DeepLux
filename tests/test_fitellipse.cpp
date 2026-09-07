#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"
#include "plugins/geometry/FitEllipse/FitEllipsePlugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <limits>

using namespace DeepLux;

class TestFitEllipse : public QObject {
    Q_OBJECT

private:
    // 阶7 批1 复核：插件库路径由 CMake $<TARGET_FILE:...> 注入（跨平台），不硬编码 .so 名。
    // 阶7 批1 复核三轮（P2-5）：metadata 与库路径均由 CMake 注入（跨平台/多配置）。
    bool installPlugin(const QString& pluginRoot, const QString& name, const QString& metaSrc,
                       const QString& libSrc) const {
        QDir root(pluginRoot);
        if (!root.mkpath(name))
            return false;
        QDir dir(root.filePath(name));
        if (!QFileInfo::exists(metaSrc) || !QFileInfo::exists(libSrc))
            return false;
        const QString destLib = dir.filePath(QFileInfo(libSrc).fileName());
        QFile::remove(dir.filePath("metadata.json"));
        QFile::remove(destLib);
        return QFile::copy(metaSrc, dir.filePath("metadata.json")) && QFile::copy(libSrc, destLib);
    }

private slots:
    void initTestCase() {
        qDebug() << "=== TestFitEllipse Start ===";
    }

    // 行为：合成椭圆点集恢复中心/长短轴/角度
    void testEllipseFitting() {
        FitEllipsePlugin plugin;
        plugin.setParams(QJsonObject{{"threshold", 0.0}, {"iterations", 1}, {"minAxis", 0.5}, {"maxAxis", 1000.0}});
        QVERIFY(plugin.initialize());

        // 半轴 a=60(b沿x), b=30, 中心(150,120), 旋转0度
        QVector<QPointF> points;
        for (int i = 0; i < 24; ++i) {
            const double t = i * M_PI / 12.0;
            points << QPointF(150 + 60 * std::cos(t), 120 + 30 * std::sin(t));
        }
        ImageData input;
        input.setData("fit_points", QVariant::fromValue(points));
        ImageData output;
        QVERIFY(plugin.execute(input, output));

        const double cx = output.data("ellipse_center_x").toDouble();
        const double cy = output.data("ellipse_center_y").toDouble();
        const double major = output.data("ellipse_major_r").toDouble();
        const double minor = output.data("ellipse_minor_r").toDouble();
        const double ellip = output.data("ellipse_ellipticity").toDouble();
        QVERIFY2(std::abs(cx - 150) < 3.0, qPrintable(QString("cx=%1").arg(cx)));
        QVERIFY2(std::abs(cy - 120) < 3.0, qPrintable(QString("cy=%1").arg(cy)));
        QVERIFY2(std::abs(major - 60) < 3.0, qPrintable(QString("major=%1").arg(major)));
        QVERIFY2(std::abs(minor - 30) < 3.0, qPrintable(QString("minor=%1").arg(minor)));
        QVERIFY2(std::abs(ellip - 0.5) < 0.05, qPrintable(QString("ellipticity=%1").arg(ellip)));
        const double phi = output.data("ellipse_phi").toDouble();
        QVERIFY2(phi >= 0.0 && phi < 180.0, qPrintable(QString("phi=%1 not normalized").arg(phi)));
        QVERIFY2(phi < 5.0 || phi > 175.0, qPrintable(QString("axis-aligned major-x phi=%1").arg(phi)));
    }

    void testInsufficientPoints() {
        FitEllipsePlugin plugin;
        QVERIFY(plugin.initialize());
        QVector<QPointF> points{QPointF(0, 0), QPointF(1, 1), QPointF(2, 2)};
        ImageData input;
        input.setData("fit_points", QVariant::fromValue(points));
        ImageData output;
        QVERIFY(!plugin.execute(input, output));
    }

    void testValidateRejectsInvalidParams() {
        FitEllipsePlugin plugin;
        QString error;
        QVERIFY(!plugin.validateParams(
            QJsonObject{{"threshold", -1.0}, {"iterations", 1}, {"minAxis", 1.0}, {"maxAxis", 10.0}}, error));
        QVERIFY(!plugin.validateParams(
            QJsonObject{{"threshold", 1.0}, {"iterations", 0}, {"minAxis", 1.0}, {"maxAxis", 10.0}}, error));
        QVERIFY(!plugin.validateParams(
            QJsonObject{{"threshold", 1.0}, {"iterations", 1}, {"minAxis", 0.0}, {"maxAxis", 10.0}}, error));
        QVERIFY(!plugin.validateParams(
            QJsonObject{{"threshold", 1.0}, {"iterations", 1}, {"minAxis", 20.0}, {"maxAxis", 10.0}}, error));
        QVERIFY(plugin.validateParams(
            QJsonObject{{"threshold", 1.0}, {"iterations", 2}, {"minAxis", 1.0}, {"maxAxis", 10.0}}, error));
    }

    void testCloneIndependence() {
        FitEllipsePlugin plugin;
        plugin.setParams(QJsonObject{{"threshold", 5.0}, {"iterations", 2}, {"minAxis", 1.0}, {"maxAxis", 100.0}});
        IModule* clone = plugin.clone();
        QVERIFY(clone != nullptr);
        auto* cloneEllipse = qobject_cast<FitEllipsePlugin*>(clone);
        QVERIFY(cloneEllipse != nullptr);
        QCOMPARE(cloneEllipse->currentParams()["threshold"].toDouble(), 5.0);
        // 修改克隆不影响原体
        cloneEllipse->setParams(
            QJsonObject{{"threshold", 9.0}, {"iterations", 2}, {"minAxis", 1.0}, {"maxAxis", 100.0}});
        QCOMPARE(plugin.currentParams()["threshold"].toDouble(), 5.0);
        delete clone;
    }

    // 流程验收：MeasurementInput(point_set) → FitEllipse，经 PluginManager 真实加载 +
    // RunEngine 调度，断言椭圆输出（镜像 test_mainwindow 双支路已验证的端口送达路径）
    void testFlowMeasurementInputToFitEllipse() {
        RunEngine& engine = RunEngine::instance();
        engine.clearModules();
        ProjectManager::instance().closeProject();

        QTemporaryDir appDir;
        QVERIFY(appDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", appDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(appDir.path()).filePath("plugins");
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasurementInput"),
                              QStringLiteral(TEST_FITELLIPSE_META_MeasurementInput),
                              QStringLiteral(TEST_FITELLIPSE_LIB_MeasurementInput)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FitEllipse"), QStringLiteral(TEST_FITELLIPSE_META_FitEllipse),
                              QStringLiteral(TEST_FITELLIPSE_LIB_FitEllipse)));
        DeepLux::PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(DeepLux::PluginManager::instance().initialize());
        QVERIFY(DeepLux::PluginManager::instance().loadPlugin(QStringLiteral("MeasurementInput")));
        QVERIFY(DeepLux::PluginManager::instance().loadPlugin(QStringLiteral("FitEllipse")));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        QJsonArray points;
        for (int i = 0; i < 24; ++i) {
            const double t = i * M_PI / 12.0;
            points.append(QJsonArray{100 + 50 * std::cos(t), 100 + 25 * std::sin(t)});
        }
        ModuleInstance input;
        input.id = QStringLiteral("mi");
        input.moduleId = QStringLiteral("MeasurementInput");
        input.params["mode"] = QStringLiteral("point_set");
        input.params["points"] = points;
        project->addModule(input);

        ModuleInstance ell;
        ell.id = QStringLiteral("ell");
        ell.moduleId = QStringLiteral("FitEllipse");
        project->addModule(ell);

        ModuleConnection conn;
        conn.fromModuleId = QStringLiteral("mi");
        conn.toModuleId = QStringLiteral("ell");
        conn.fromPort = QStringLiteral("fit_points");
        conn.toPort = QStringLiteral("fit_points");
        conn.edgeType = QStringLiteral("data");
        project->addConnection(conn);

        QVERIFY(engine.loadProject(project));

        engine.runOnce();
        const ImageData out = engine.moduleOutput(QStringLiteral("ell"));
        QVERIFY2(out.hasData("ellipse_major_r"), "flow must produce ellipse_major_r");
        QVERIFY2(std::abs(out.data("ellipse_major_r").toDouble() - 50) < 3.0, "major axis ~50");
        QVERIFY2(std::abs(out.data("ellipse_minor_r").toDouble() - 25) < 3.0, "minor axis ~25");
        engine.clearModules();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    // 阶7 批1 复核（P1-1）：强离群点不得拉偏结果（RANSAC 稳健估计）
    void testRejectsStrongOutlier() {
        // 阶7 批1 复核三轮（P1-2）：使用默认参数（threshold=2, iterations=100）验证默认配置稳健
        FitEllipsePlugin plugin;
        QVERIFY(plugin.initialize());
        QVector<QPointF> points;
        for (int i = 0; i < 24; ++i) {
            const double t = i * M_PI / 12.0;
            points << QPointF(150 + 60 * std::cos(t), 120 + 30 * std::sin(t));
        }
        points << QPointF(2000, 2000); // 远端离群点
        ImageData input;
        input.setData("fit_points", QVariant::fromValue(points));
        ImageData output;
        QVERIFY(plugin.execute(input, output));
        const double cx = output.data("ellipse_center_x").toDouble();
        const double cy = output.data("ellipse_center_y").toDouble();
        const double major = output.data("ellipse_major_r").toDouble();
        const double minor = output.data("ellipse_minor_r").toDouble();
        QVERIFY2(std::abs(cx - 150) < 5.0, qPrintable(QString("cx=%1 pulled by outlier").arg(cx)));
        QVERIFY2(std::abs(cy - 120) < 5.0, qPrintable(QString("cy=%1 pulled by outlier").arg(cy)));
        QVERIFY2(std::abs(major - 60) < 5.0, qPrintable(QString("major=%1 pulled by outlier").arg(major)));
        QVERIFY2(std::abs(minor - 30) < 5.0, qPrintable(QString("minor=%1 pulled by outlier").arg(minor)));
    }

    // 阶7 批1 复核三轮（P0-1）：重复点（唯一点<5）失败关闭，不无限循环
    void testDuplicatePointsFailClosed() {
        FitEllipsePlugin plugin;
        QVERIFY(plugin.initialize());
        QVector<QPointF> points; // 8 个点但仅 4 个唯一坐标
        for (int i = 0; i < 2; ++i) {
            points << QPointF(0, 0) << QPointF(10, 0) << QPointF(10, 5) << QPointF(0, 5);
        }
        ImageData input;
        input.setData("fit_points", QVariant::fromValue(points));
        ImageData output;
        QVERIFY(!plugin.execute(input, output));
    }

    // 阶7 批1 复核（P1-4）：phi 契约=度、归一化 [0,180)；旋转椭圆恢复角度
    void testRotatedEllipsePhiNormalized() {
        FitEllipsePlugin plugin;
        plugin.setParams(QJsonObject{{"threshold", 0.0}, {"iterations", 1}, {"minAxis", 0.5}, {"maxAxis", 1000.0}});
        QVERIFY(plugin.initialize());
        const double rot = 30.0 * M_PI / 180.0;
        QVector<QPointF> points;
        for (int i = 0; i < 24; ++i) {
            const double t = i * M_PI / 12.0;
            const double x = 60 * std::cos(t);
            const double y = 30 * std::sin(t);
            points << QPointF(150 + x * std::cos(rot) - y * std::sin(rot), 120 + x * std::sin(rot) + y * std::cos(rot));
        }
        ImageData input;
        input.setData("fit_points", QVariant::fromValue(points));
        ImageData output;
        QVERIFY(plugin.execute(input, output));
        const double phi = output.data("ellipse_phi").toDouble();
        QVERIFY2(phi >= 0.0 && phi < 180.0, qPrintable(QString("phi=%1 not in [0,180)").arg(phi)));
        QVERIFY2(std::abs(phi - 30.0) < 5.0, qPrintable(QString("phi=%1 expected ~30").arg(phi)));
    }

    // 阶7 批1 复核（P1-3）：严格类型检查，字符串/小数迭代等宽松转换被拒绝
    void testValidateRejectsLooseTypes() {
        FitEllipsePlugin plugin;
        QString error;
        QVERIFY(!plugin.validateParams(
            QJsonObject{{"threshold", QStringLiteral("2.0")}, {"iterations", 3}, {"minAxis", 0.5}, {"maxAxis", 100.0}},
            error));
        QVERIFY(!plugin.validateParams(
            QJsonObject{{"threshold", 2.0}, {"iterations", 1.5}, {"minAxis", 0.5}, {"maxAxis", 100.0}}, error));
        QVERIFY(!plugin.validateParams(
            QJsonObject{{"threshold", 2.0}, {"iterations", 3}, {"minAxis", 0.5}, {"maxAxis", QStringLiteral("100")}},
            error));
        // 阶7 批1 复核四轮（P1-1）：setParam 注入非法运行期快照（setParams 会整体拒绝），
        // 确认 currentParams 保留非法值；用确定可拟合的正常椭圆点集断言失败确因参数解析；
        // 再恢复合法参数，同一组点必须成功。
        QVector<QPointF> good;
        for (int i = 0; i < 24; ++i) {
            const double t = i * M_PI / 12.0;
            good << QPointF(150 + 60 * std::cos(t), 120 + 30 * std::sin(t));
        }
        FitEllipsePlugin badPlugin;
        QVERIFY(badPlugin.initialize());
        badPlugin.setParam(QStringLiteral("threshold"), QStringLiteral("2.0"));
        QCOMPARE(badPlugin.currentParams()["threshold"].toString(), QStringLiteral("2.0"));
        ImageData goodInput;
        goodInput.setData("fit_points", QVariant::fromValue(good));
        ImageData badOut;
        QVERIFY2(!badPlugin.execute(goodInput, badOut), "illegal runtime param must fail on parse");
        badPlugin.setParam(QStringLiteral("threshold"), 2.0);
        ImageData okOut;
        QVERIFY2(badPlugin.execute(goodInput, okOut), "legal params must succeed on same points");
    }

    // 阶7 批1 复核五轮（P1-3）：NaN/±Inf 坐标在输入边界被拒绝（排序严格弱序）
    void testRejectsNonFinitePoints() {
        FitEllipsePlugin plugin;
        QVERIFY(plugin.initialize());
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (const double bad : {nan, inf, -inf}) {
            QVector<QPointF> points;
            for (int i = 0; i < 6; ++i)
                points << QPointF(100 + i, 100 + i);
            points[3] = QPointF(bad, 100.0);
            ImageData input;
            input.setData("fit_points", QVariant::fromValue(points));
            ImageData output;
            QVERIFY2(!plugin.execute(input, output), "non-finite coordinate must be rejected");
        }
    }

    // 阶7 批1 复核五轮（P2-4）：运行期参数边界与 metadata 一致
    void testParamBoundsMatchMetadata() {
        FitEllipsePlugin plugin;
        QString error;
        const auto P = [](double th, double it, double mn, double mx) {
            return QJsonObject{{"threshold", th}, {"iterations", it}, {"minAxis", mn}, {"maxAxis", mx}};
        };
        QVERIFY(plugin.validateParams(P(1e6, 1000, 0.1, 1e6), error));      // 边界内
        QVERIFY(!plugin.validateParams(P(1e6 + 1, 1000, 0.1, 1e6), error)); // threshold 超上限
        QVERIFY(!plugin.validateParams(P(2.0, 1001, 0.1, 1e6), error));     // iterations 超上限
        QVERIFY(!plugin.validateParams(P(2.0, 1000, 0.09, 1e6), error));    // minAxis 低于下限
        QVERIFY(!plugin.validateParams(P(2.0, 1000, 0.1, 1e6 + 1), error)); // maxAxis 超上限
    }

    void testPluginInfo() {
        FitEllipsePlugin plugin;
        QCOMPARE(plugin.moduleId(), QStringLiteral("com.deeplux.plugin.fitellipse"));
        QCOMPARE(plugin.category(), QStringLiteral("geometry"));
    }
};

QTEST_MAIN(TestFitEllipse)
#include "test_fitellipse.moc"
