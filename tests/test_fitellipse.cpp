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

using namespace DeepLux;

class TestFitEllipse : public QObject {
    Q_OBJECT

private:
    // 阶7 批1 复核：插件库路径由 CMake $<TARGET_FILE:...> 注入（跨平台），不硬编码 .so 名。
    bool installPlugin(const QString& pluginRoot, const QString& name, const QString& domain,
                       const QString& libSrc) const {
        QDir root(pluginRoot);
        if (!root.mkpath(name))
            return false;
        QDir dir(root.filePath(name));
        const QString srcRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../src/plugins");
        const QString metaSrc = QDir(srcRoot).filePath(QString("%1/%2/metadata.json").arg(domain, name));
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
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasurementInput"), QStringLiteral("geometry"),
                              QStringLiteral(TEST_FITELLIPSE_LIB_MeasurementInput)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FitEllipse"), QStringLiteral("geometry"),
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
        FitEllipsePlugin plugin;
        plugin.setParams(QJsonObject{{"threshold", 2.0}, {"iterations", 200}, {"minAxis", 0.5}, {"maxAxis", 1000.0}});
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
        // 执行同样拒绝非法快照
        plugin.setParams(
            QJsonObject{{"threshold", QStringLiteral("2.0")}, {"iterations", 3}, {"minAxis", 0.5}, {"maxAxis", 100.0}});
        QVector<QPointF> points;
        for (int i = 0; i < 8; ++i)
            points << QPointF(i, i);
        ImageData input;
        input.setData("fit_points", QVariant::fromValue(points));
        ImageData output;
        QVERIFY(!plugin.execute(input, output));
    }

    void testPluginInfo() {
        FitEllipsePlugin plugin;
        QCOMPARE(plugin.moduleId(), QStringLiteral("com.deeplux.plugin.fitellipse"));
        QCOMPARE(plugin.category(), QStringLiteral("geometry"));
    }
};

QTEST_MAIN(TestFitEllipse)
#include "test_fitellipse.moc"
