#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"
#include "plugins/detection/EdgeDefectDetection/EdgeDefectDetectionPlugin.h"
#include "plugins/detection/MeasureCircle/MeasureCirclePlugin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

using namespace DeepLux;

namespace {
// 合成深色背景+亮环图像（圆心 cx,cy 半径 r，线宽 3；bump≠0 时 90..120 度弧段径向
// 偏移 bump：正=凸出缺陷，负=凹陷缺陷）
ImageData makeRingImage(int cx, int cy, int r, int bump = 0) {
    cv::Mat mat = cv::Mat::zeros(480, 640, CV_8UC1);
    for (int a = 0; a < 360; ++a) {
        const double t = a * M_PI / 180.0;
        int rr = r;
        if (bump != 0 && a >= 90 && a < 120) {
            rr = r + bump;
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

// 合成实心圆盘图像（阶7 批2 复核二轮：半径处为阶跃边缘，r±1 采样必有梯度，
// 用于 searchLength=1 单点搜索边界与 16 位归一化测试）
ImageData makeDiskImage(int cx, int cy, int r, bool mono16 = false) {
    if (mono16) {
        cv::Mat mat = cv::Mat::zeros(480, 640, CV_16UC1);
        cv::circle(mat, cv::Point(cx, cy), r, cv::Scalar(65535), cv::FILLED);
        return ImageData(mat);
    }
    cv::Mat mat = cv::Mat::zeros(480, 640, CV_8UC1);
    cv::circle(mat, cv::Point(cx, cy), r, cv::Scalar(255), cv::FILLED);
    return ImageData(mat);
}

// 理想圆参考点集（72 点，5 度间隔）
QVector<QPointF> idealCircle(int cx, int cy, double r) {
    QVector<QPointF> ref;
    for (int a = 0; a < 72; ++a) {
        const double t = a * 5 * M_PI / 180.0;
        ref << QPointF(cx + r * std::cos(t), cy + r * std::sin(t));
    }
    return ref;
}
} // namespace

class TestDetectionBatch2 : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appDir;

    // 阶7 批2 复核二轮（P2-1/P2-2）：metadata 与库路径均由 CMake 注入
    // （$<TARGET_FILE:...>/${CMAKE_SOURCE_DIR}，跨平台/多配置），不再以可执行文件
    // 拼相对路径；删除未使用的 domain 参数。
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
        QVERIFY(m_appDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDir.path().toLocal8Bit());
        PluginManager::instance().shutdown();
        const QString pluginRoot = QDir(m_appDir.path()).filePath("plugins");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(TEST_BATCH2_META_GrabImage),
                               QStringLiteral(TEST_BATCH2_LIB_GrabImage)),
                 "install GrabImage");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("MeasurementInput"),
                               QStringLiteral(TEST_BATCH2_META_MeasurementInput),
                               QStringLiteral(TEST_BATCH2_LIB_MeasurementInput)),
                 "install MeasurementInput");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("MeasureCircle"),
                               QStringLiteral(TEST_BATCH2_META_MeasureCircle),
                               QStringLiteral(TEST_BATCH2_LIB_MeasureCircle)),
                 "install MeasureCircle");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("EdgeDefectDetection"),
                               QStringLiteral(TEST_BATCH2_META_EdgeDefectDetection),
                               QStringLiteral(TEST_BATCH2_LIB_EdgeDefectDetection)),
                 "install EdgeDefectDetection");
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasurementInput")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasureCircle")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("EdgeDefectDetection")));
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void cleanup() {
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        RunEngine::instance().clearOutputs();
    }

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
        const auto P = [](double cx, double cy, double r, double th, double mc, double sl, double ex) {
            return QJsonObject{{"initialCenterX", cx}, {"initialCenterY", cy}, {"initialRadius", r},
                               {"threshold", th},      {"measureCount", mc},   {"searchLength", sl},
                               {"exclusionRadius", ex}};
        };
        QVERIFY(!plugin.validateParams(P(0, 0, 0, 20, 36, 20, 0), error));   // radius<1
        QVERIFY(!plugin.validateParams(P(0, 0, 80, 300, 36, 20, 0), error)); // threshold>255
        QVERIFY(!plugin.validateParams(P(0, 0, 80, 20, 7, 20, 0), error));   // measureCount<8
        QVERIFY(!plugin.validateParams(P(0, 0, 80, 20, 36, 0.5, 0), error)); // searchLength<1
        QVERIFY(!plugin.validateParams(P(0, 0, 80, 20, 36, 501, 0), error)); // searchLength>500
        QVERIFY(plugin.validateParams(P(0, 0, 80, 20, 36, 1, 0), error));    // searchLength=1 合法下限
        QVERIFY(plugin.validateParams(P(0, 0, 80, 20, 36, 500, 0), error));  // searchLength=500 合法上限
    }

    // 阶7 批2 复核二轮（P1-1）：threshold=0 时纯色图像不得以零梯度伪造边缘圆，
    // 必须失败关闭并报"边缘点不足"（QSignalSpy 断言错误来源）。
    void testMeasureCircleSolidImageFails() {
        MeasureCirclePlugin plugin;
        plugin.setParams(QJsonObject{{"initialCenterX", 200.0},
                                     {"initialCenterY", 200.0},
                                     {"initialRadius", 80.0},
                                     {"threshold", 0.0},
                                     {"measureCount", 36},
                                     {"searchLength", 20.0},
                                     {"exclusionRadius", 0.0}});
        QVERIFY(plugin.initialize());
        cv::Mat solid(480, 640, CV_8UC1, cv::Scalar(128));
        ImageData input(solid);
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData output;
        QVERIFY2(!plugin.execute(input, output), "solid image must not fabricate a circle");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("边缘点不足")),
                 qPrintable(QString("error must be edge-point shortage, got: %1")
                                .arg(errSpy.count() ? errSpy.first().at(0).toString() : QString())));
    }

    // 阶7 批2 复核二轮（P1-2）：searchLength=1 时闭区间仍有单点搜索（旧开区间为空循环，
    // 会报"边缘点不足"），用阶跃边缘圆盘验证半径恢复。
    void testMeasureCircleSearchLengthOneBoundary() {
        MeasureCirclePlugin plugin;
        plugin.setParams(QJsonObject{{"initialCenterX", 200.0},
                                     {"initialCenterY", 200.0},
                                     {"initialRadius", 80.0},
                                     {"threshold", 20.0},
                                     {"measureCount", 36},
                                     {"searchLength", 1.0},
                                     {"exclusionRadius", 0.0}});
        QVERIFY(plugin.initialize());
        ImageData input = makeDiskImage(200, 200, 80);
        ImageData output;
        QVERIFY2(plugin.execute(input, output), "searchLength=1 must still search (closed interval)");
        QVERIFY2(std::abs(output.data("circle_radius").toDouble() - 80) < 2.0, "radius ~80");
        QVERIFY2(output.data("edge_point_count").toDouble() >= 30.0, "most calipers must find the step edge");
    }

    // 阶7 批2 复核二轮（P1-6）：16 位图归一化到 0..255 后测量成功（旧 convertTo 截断为全 0）
    void testMeasureCircleMono16Normalized() {
        MeasureCirclePlugin plugin;
        plugin.setParams(QJsonObject{{"initialCenterX", 200.0},
                                     {"initialCenterY", 200.0},
                                     {"initialRadius", 80.0},
                                     {"threshold", 20.0},
                                     {"measureCount", 36},
                                     {"searchLength", 20.0},
                                     {"exclusionRadius", 0.0}});
        QVERIFY(plugin.initialize());
        ImageData input = makeDiskImage(200, 200, 80, true);
        QCOMPARE(input.toMat().depth(), CV_16U);
        ImageData output;
        QVERIFY2(plugin.execute(input, output), "16U image must succeed after normalization");
        QVERIFY2(std::abs(output.data("circle_radius").toDouble() - 80) < 2.0, "radius ~80");
    }

    // 阶7 批2 复核二轮（P1-6）：2 通道图像明确失败（cvtColor 无法处理，旧代码抛异常/静默错）
    void testMeasureCircleTwoChannelRejected() {
        MeasureCirclePlugin plugin;
        QVERIFY(plugin.initialize());
        cv::Mat c2(480, 640, CV_8UC2, cv::Scalar(100, 150));
        ImageData input(c2);
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData output;
        QVERIFY2(!plugin.execute(input, output), "2-channel image must fail");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("2 通道")),
                 qPrintable(QString("error must mention 2-channel, got: %1")
                                .arg(errSpy.count() ? errSpy.first().at(0).toString() : QString())));
    }

    void testEdgeDefectCleanVsDefect() {
        // 参考边缘 = 理想圆点集；干净环无缺陷，带凸出弧段检出凸出缺陷
        const QVector<QPointF> ref = idealCircle(200, 200, 80.0);
        EdgeDefectDetectionPlugin plugin;
        plugin.setParams(QJsonObject{{"threshold", 2.0}, {"searchLength", 10.0}, {"isConvex", true}});
        QVERIFY(plugin.initialize());

        ImageData clean = makeRingImage(200, 200, 80);
        clean.setData("reference_edge", QVariant::fromValue(ref));
        ImageData cleanOut;
        QVERIFY2(plugin.execute(clean, cleanOut), "clean ring must succeed");
        QCOMPARE(cleanOut.data("defect_count").toDouble(), 0.0);
        QVERIFY(cleanOut.data("has_defect").toBool() == false);
        QVERIFY2(cleanOut.data("defect_regions").toString().isEmpty(), "clean ring must have no region list");

        ImageData bumped = makeRingImage(200, 200, 80, 6);
        bumped.setData("reference_edge", QVariant::fromValue(ref));
        ImageData bumpOut;
        QVERIFY2(plugin.execute(bumped, bumpOut), "bumped ring must succeed");
        // 阶7 批2 复核二轮（P1-5）：连续凸出弧段（6 条采样射线）合并为 1 个缺陷区域，
        // defect_count = 选定极性区域数且与 has_defect 恒一致
        QCOMPARE(bumpOut.data("convex_count").toDouble(), 1.0);
        QCOMPARE(bumpOut.data("defect_count").toDouble(), 1.0);
        QCOMPARE(bumpOut.data("concave_count").toDouble(), 0.0);
        QVERIFY(bumpOut.data("has_defect").toBool());
        QVERIFY(bumpOut.data("max_deviation").toDouble() > 2.0);
        const QString regions = bumpOut.data("defect_regions").toString();
        QVERIFY2(regions.contains(QStringLiteral("convex")) && !regions.contains(QLatin1Char(';')),
                 qPrintable(QString("expected single convex region, got: %1").arg(regions)));
    }

    // 阶7 批2 复核二轮（P1-5/P2-3）：凹陷极性——isConvex=false 时凹陷计入缺陷；
    // isConvex=true 时 has_defect=false 且 defect_count=0（两者恒一致），
    // 但凹陷区域数仍如实报告（不因极性选择丢失）。
    void testEdgeDefectConcavePolarity() {
        const QVector<QPointF> ref = idealCircle(200, 200, 80.0);
        ImageData dented = makeRingImage(200, 200, 80, -6);
        dented.setData("reference_edge", QVariant::fromValue(ref));

        EdgeDefectDetectionPlugin concave;
        concave.setParams(QJsonObject{{"threshold", 2.0}, {"searchLength", 10.0}, {"isConvex", false}});
        QVERIFY(concave.initialize());
        ImageData out1;
        QVERIFY2(concave.execute(dented, out1), "dented ring must succeed");
        QCOMPARE(out1.data("concave_count").toDouble(), 1.0);
        QCOMPARE(out1.data("convex_count").toDouble(), 0.0);
        QCOMPARE(out1.data("defect_count").toDouble(), 1.0);
        QVERIFY(out1.data("has_defect").toBool());
        QVERIFY2(out1.data("defect_regions").toString().contains(QStringLiteral("concave")),
                 "region list must contain concave");
        QVERIFY(out1.data("max_deviation").toDouble() > 2.0);

        EdgeDefectDetectionPlugin convexOnly;
        convexOnly.setParams(QJsonObject{{"threshold", 2.0}, {"searchLength", 10.0}, {"isConvex", true}});
        QVERIFY(convexOnly.initialize());
        ImageData out2;
        QVERIFY2(convexOnly.execute(dented, out2), "dented ring must succeed");
        QCOMPARE(out2.data("defect_count").toDouble(), 0.0);
        QCOMPARE(out2.data("has_defect").toBool(), false);
        QCOMPARE(out2.data("concave_count").toDouble(), 1.0);
    }

    // 阶7 批2 复核二轮（P1-2）：EdgeDefect searchLength=1 单点闭区间仍能提取边缘，
    // 与参考圆一致时无缺陷。
    void testEdgeDefectSearchLengthOneBoundary() {
        EdgeDefectDetectionPlugin plugin;
        plugin.setParams(QJsonObject{{"threshold", 2.0}, {"searchLength", 1.0}, {"isConvex", true}});
        QVERIFY(plugin.initialize());
        ImageData disk = makeDiskImage(200, 200, 80);
        disk.setData("reference_edge", QVariant::fromValue(idealCircle(200, 200, 80.0)));
        ImageData output;
        QVERIFY2(plugin.execute(disk, output), "searchLength=1 must still search (closed interval)");
        QCOMPARE(output.data("defect_count").toDouble(), 0.0);
        QVERIFY2(output.data("max_deviation").toDouble() <= 1.0, "disk edge must match reference circle");
    }

    // 阶7 批2 复核二轮（P1-4）：偏差必须以拟合基准圆半径为基准——参考边缘带 ±5
    // 径向交替噪声（75/85）时，基准圆拟合半径≈80，图像仍是 80 的标准环 → 无缺陷。
    // 旧行为按各参考点自身半径算偏差，噪声直接进入偏差（+3.15/-6.85）→ 伪缺陷。
    void testEdgeDefectBaselineIsFittedCircle() {
        EdgeDefectDetectionPlugin plugin;
        plugin.setParams(QJsonObject{{"threshold", 3.0}, {"searchLength", 10.0}, {"isConvex", false}});
        QVERIFY(plugin.initialize());
        QVector<QPointF> noisy;
        for (int a = 0; a < 72; ++a) {
            const double t = a * 5 * M_PI / 180.0;
            const double rr = (a % 2 == 0) ? 75.0 : 85.0; // ±5 径向交替噪声
            noisy << QPointF(200 + rr * std::cos(t), 200 + rr * std::sin(t));
        }
        ImageData ring = makeRingImage(200, 200, 80);
        ring.setData("reference_edge", QVariant::fromValue(noisy));
        ImageData output;
        QVERIFY2(plugin.execute(ring, output), "noisy reference must succeed");
        QCOMPARE(output.data("defect_count").toDouble(), 0.0);
        QCOMPARE(output.data("has_defect").toBool(), false);
        QVERIFY2(output.data("max_deviation").toDouble() < 3.0,
                 qPrintable(QString("deviation must be measured against fitted baseline, got max %1")
                                .arg(output.data("max_deviation").toDouble())));
    }

    // 流程验收：GrabImage → MeasureCircle（PluginManager 真实加载 + RunEngine 调度）
    void testFlowMeasureCircle() {
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params["grabSource"] = QStringLiteral("Path");
        grab.params["filePath"] = QStringLiteral(TEST_BATCH2_DATA_Circle);
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
    }

    // 流程验收（阶7 批2 复核二轮 P2-3）：GrabImage → EdgeDefectDetection(image) +
    // MeasurementInput(point_set) → EdgeDefectDetection(reference_edge)，
    // 参考圆与图像圆盘一致 → 无缺陷（跨名端口 fit_points→reference_edge 送达）。
    void testFlowEdgeDefect() {
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params["grabSource"] = QStringLiteral("Path");
        grab.params["filePath"] = QStringLiteral(TEST_BATCH2_DATA_Circle);
        project->addModule(grab);

        ModuleInstance mi;
        mi.id = QStringLiteral("mi");
        mi.moduleId = QStringLiteral("MeasurementInput");
        mi.params["mode"] = QStringLiteral("point_set");
        QJsonArray points;
        for (const QPointF& p : idealCircle(320, 240, 100.0)) {
            points.append(QJsonArray{p.x(), p.y()});
        }
        mi.params["points"] = points;
        project->addModule(mi);

        ModuleInstance ed;
        ed.id = QStringLiteral("ed");
        ed.moduleId = QStringLiteral("EdgeDefectDetection");
        ed.params["threshold"] = 3.0;
        ed.params["searchLength"] = 10.0;
        ed.params["isConvex"] = true;
        project->addModule(ed);

        ModuleConnection imgConn;
        imgConn.fromModuleId = QStringLiteral("grab");
        imgConn.toModuleId = QStringLiteral("ed");
        imgConn.fromPort = QStringLiteral("image");
        imgConn.toPort = QStringLiteral("image");
        imgConn.edgeType = QStringLiteral("data");
        project->addConnection(imgConn);
        ModuleConnection refConn;
        refConn.fromModuleId = QStringLiteral("mi");
        refConn.toModuleId = QStringLiteral("ed");
        refConn.fromPort = QStringLiteral("fit_points");
        refConn.toPort = QStringLiteral("reference_edge");
        refConn.edgeType = QStringLiteral("data");
        project->addConnection(refConn);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();
        const ImageData out = engine.moduleOutput(QStringLiteral("ed"));
        QVERIFY2(out.hasData("has_defect"), "flow must produce has_defect");
        QVERIFY2(out.hasData("defect_regions"), "flow must produce defect_regions");
        QCOMPARE(out.data("has_defect").toBool(), false);
        QCOMPARE(out.data("defect_count").toDouble(), 0.0);
        QVERIFY2(out.data("max_deviation").toDouble() < 3.0, "clean disk deviation below threshold");
    }
};

QTEST_MAIN(TestDetectionBatch2)
#include "test_detection_batch2.moc"
