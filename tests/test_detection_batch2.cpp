#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"
#include "plugins/detection/EdgeDefectDetection/EdgeDefectDetectionPlugin.h"
#include "plugins/detection/MeasureCircle/MeasureCirclePlugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

using namespace DeepLux;

namespace {
// 合成深色背景+亮环图像（圆心 cx,cy 半径 r，线宽 3）
ImageData makeRingImage(int cx, int cy, int r, int bump = 0) {
    cv::Mat mat = cv::Mat::zeros(480, 640, CV_8UC1);
    for (int a = 0; a < 360; ++a) {
        const double t = a * M_PI / 180.0;
        int rr = r;
        if (bump > 0 && a >= 90 && a < 120) {
            rr = r + bump; // 凸出缺陷弧段
        }
        for (int w = -1; w <= 1; ++w) {
            const int x = cvRound(cx + (rr + w) * std::cos(t));
            const int y = cvRound(cy + (rr + w) * std::sin(t));
            if (x >= 0 && y >= 0 && x < mat.cols && y < mat.rows) {
                mat.at<uchar>(y, x) = 255;
            }
        }
    }
    return ImageData(mat);
}
} // namespace

class TestDetectionBatch2 : public QObject {
    Q_OBJECT

private:
    bool installPlugin(const QString& pluginRoot, const QString& name, const QString& domain, const QString& metaSrc,
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
    void testMeasureCircleRecoversCircle() {
        MeasureCirclePlugin plugin;
        plugin.setParams(QJsonObject{{"initialCenterX", 200.0},
                                     {"initialCenterY", 200.0},
                                     {"initialRadius", 80.0},
                                     {"threshold", 20.0},
                                     {"measureCount", 36},
                                     {"searchLength", 20.0},
                                     {"exclusionRadius", 0.0}});
        QVERIFY(plugin.initialize());
        ImageData input = makeRingImage(200, 200, 80);
        ImageData output;
        QVERIFY2(plugin.execute(input, output), "measure circle must succeed on synthetic ring");
        QVERIFY2(std::abs(output.data("circle_center_x").toDouble() - 200) < 3.0, "center x ~200");
        QVERIFY2(std::abs(output.data("circle_center_y").toDouble() - 200) < 3.0, "center y ~200");
        QVERIFY2(std::abs(output.data("circle_radius").toDouble() - 80) < 3.0, "radius ~80");
        QVERIFY2(std::abs(output.data("circle_diameter").toDouble() - 160) < 6.0, "diameter ~160");
        QVERIFY(output.data("circle_roundness").toDouble() > 0.9);
    }

    void testMeasureCircleValidation() {
        MeasureCirclePlugin plugin;
        QString error;
        QVERIFY(!plugin.validateParams(QJsonObject{{"initialCenterX", 0.0},
                                                   {"initialCenterY", 0.0},
                                                   {"initialRadius", 0.0},
                                                   {"threshold", 20.0},
                                                   {"measureCount", 36},
                                                   {"searchLength", 20.0},
                                                   {"exclusionRadius", 0.0}},
                                       error)); // radius<1
        QVERIFY(!plugin.validateParams(QJsonObject{{"initialCenterX", 0.0},
                                                   {"initialCenterY", 0.0},
                                                   {"initialRadius", 80.0},
                                                   {"threshold", 300.0},
                                                   {"measureCount", 36},
                                                   {"searchLength", 20.0},
                                                   {"exclusionRadius", 0.0}},
                                       error)); // threshold>255
        QVERIFY(!plugin.validateParams(QJsonObject{{"initialCenterX", 0.0},
                                                   {"initialCenterY", 0.0},
                                                   {"initialRadius", 80.0},
                                                   {"threshold", 20.0},
                                                   {"measureCount", 7},
                                                   {"searchLength", 20.0},
                                                   {"exclusionRadius", 0.0}},
                                       error)); // measureCount<8
    }

    void testEdgeDefectCleanVsDefect() {
        // 参考边缘 = 理想圆点集；干净环无缺陷，带凸出弧段检出凸出缺陷
        QVector<QPointF> ref;
        for (int a = 0; a < 72; ++a) {
            const double t = a * 5 * M_PI / 180.0;
            ref << QPointF(200 + 80 * std::cos(t), 200 + 80 * std::sin(t));
        }
        EdgeDefectDetectionPlugin plugin;
        plugin.setParams(QJsonObject{{"threshold", 2.0}, {"searchLength", 10.0}, {"isConvex", true}});
        QVERIFY(plugin.initialize());

        ImageData clean = makeRingImage(200, 200, 80);
        clean.setData("reference_edge", QVariant::fromValue(ref));
        ImageData cleanOut;
        QVERIFY2(plugin.execute(clean, cleanOut), "clean ring must succeed");
        QCOMPARE(cleanOut.data("defect_count").toDouble(), 0.0);
        QVERIFY(cleanOut.data("has_defect").toBool() == false);

        ImageData bumped = makeRingImage(200, 200, 80, 6);
        bumped.setData("reference_edge", QVariant::fromValue(ref));
        ImageData bumpOut;
        QVERIFY2(plugin.execute(bumped, bumpOut), "bumped ring must succeed");
        QVERIFY2(bumpOut.data("convex_count").toDouble() > 0.0, "bump must produce convex defects");
        QVERIFY(bumpOut.data("has_defect").toBool());
        QVERIFY(bumpOut.data("max_deviation").toDouble() > 2.0);
    }

    // 流程验收：GrabImage → MeasureCircle（PluginManager 真实加载 + RunEngine 调度）
    void testFlowMeasureCircle() {
        RunEngine& engine = RunEngine::instance();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        QTemporaryDir appDir;
        QVERIFY(appDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", appDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(appDir.path()).filePath("plugins");
        const QString srcRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../src/plugins");
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral("image_processing"),
                              srcRoot + "/image_processing/GrabImage/metadata.json",
                              QStringLiteral(TEST_BATCH2_LIB_GrabImage)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasureCircle"), QStringLiteral("detection"),
                              srcRoot + "/detection/MeasureCircle/metadata.json",
                              QStringLiteral(TEST_BATCH2_LIB_MeasureCircle)));
        DeepLux::PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(DeepLux::PluginManager::instance().initialize());
        QVERIFY(DeepLux::PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(DeepLux::PluginManager::instance().loadPlugin(QStringLiteral("MeasureCircle")));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        const QString imagePath =
            QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../tests/acceptance/data/circle_640x480.png");
        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params["grabSource"] = QStringLiteral("Path");
        grab.params["filePath"] = imagePath;
        project->addModule(grab);
        ModuleInstance mc;
        mc.id = QStringLiteral("mc");
        mc.moduleId = QStringLiteral("MeasureCircle");
        mc.params["initialCenterX"] = 320.0;
        mc.params["initialCenterY"] = 240.0;
        mc.params["initialRadius"] = 100.0;
        mc.params["threshold"] = 20.0;
        mc.params["measureCount"] = 36;
        mc.params["searchLength"] = 30.0;
        project->addModule(mc);
        ModuleConnection conn;
        conn.fromModuleId = QStringLiteral("grab");
        conn.toModuleId = QStringLiteral("mc");
        conn.fromPort = QStringLiteral("image");
        conn.toPort = QStringLiteral("image");
        conn.edgeType = QStringLiteral("data");
        project->addConnection(conn);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();
        const ImageData out = engine.moduleOutput(QStringLiteral("mc"));
        QVERIFY2(out.hasData("circle_radius"), "flow must produce circle_radius");
        QVERIFY2(std::abs(out.data("circle_radius").toDouble() - 100) < 5.0, "flow radius ~100");
        engine.clearModules();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }
};

QTEST_MAIN(TestDetectionBatch2)
#include "test_detection_batch2.moc"
