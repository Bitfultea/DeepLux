#include "core/deeplux/DataContract.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"
#include "plugins/calibration/CalculateOffset/CalculateOffsetPlugin.h"
#include "plugins/image_processing/CropImage/CropImagePlugin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <limits>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

using namespace DeepLux;

namespace {
// 确定性纹理图：value = (7x + 13y) % 256，裁剪内容可逐像素校验
cv::Mat makePatternImage(int w, int h) {
    cv::Mat m(h, w, CV_8UC1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            m.at<uchar>(y, x) = static_cast<uchar>((7 * x + 13 * y) % 256);
        }
    }
    return m;
}

QJsonObject cropParams(const QJsonArray& rects, bool outputFirst = true) {
    return QJsonObject{{"rectangles", rects}, {"outputFirstAsImage", outputFirst}};
}

QJsonArray rect(double cx, double cy, double l1, double l2, double deg) {
    return QJsonArray{cx, cy, l1, l2, deg};
}

QJsonObject offsetParams(double cx, double cy, double ca, double tx, double ty, double ta) {
    return QJsonObject{{"currentX", cx}, {"currentY", cy}, {"currentAngle", ca},
                       {"targetX", tx},  {"targetY", ty},  {"targetAngle", ta}};
}
} // namespace

class TestBatch4 : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appDir;

    // 阶7 批4：metadata/库/数据路径全部 CMake 注入（沿用批2复核二轮模式）
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
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(TEST_BATCH4_META_GrabImage),
                               QStringLiteral(TEST_BATCH4_LIB_GrabImage)),
                 "install GrabImage");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("CropImage"), QStringLiteral(TEST_BATCH4_META_CropImage),
                               QStringLiteral(TEST_BATCH4_LIB_CropImage)),
                 "install CropImage");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("MeasureCircle"),
                               QStringLiteral(TEST_BATCH4_META_MeasureCircle),
                               QStringLiteral(TEST_BATCH4_LIB_MeasureCircle)),
                 "install MeasureCircle");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("MeasurementInput"),
                               QStringLiteral(TEST_BATCH4_META_MeasurementInput),
                               QStringLiteral(TEST_BATCH4_LIB_MeasurementInput)),
                 "install MeasurementInput");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("CalculateOffset"),
                               QStringLiteral(TEST_BATCH4_META_CalculateOffset),
                               QStringLiteral(TEST_BATCH4_LIB_CalculateOffset)),
                 "install CalculateOffset");
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("CropImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasureCircle")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasurementInput")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("CalculateOffset")));
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void cleanup() {
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        RunEngine::instance().clearOutputs();
    }

    // ---------- CropImage ----------

    void testCropAxisAligned() {
        CropImagePlugin plugin;
        QVERIFY(plugin.initialize()); // 默认参数：中心 100×100 矩形
        plugin.setParams(cropParams(QJsonArray{rect(320, 240, 200, 100, 0)}));
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "axis-aligned crop must succeed");
        QCOMPARE(out.data("crop_count").toDouble(), 1.0);
        // outputFirstAsImage 默认 true：image 端口即首个裁剪图
        QCOMPARE(out.toMat().cols, 200);
        QCOMPARE(out.toMat().rows, 100);
        // 裁剪内容逐像素一致：crop(0,0) == src(190,220)
        const cv::Mat src = makePatternImage(640, 480);
        QCOMPARE(static_cast<int>(out.toMat().at<uchar>(0, 0)), static_cast<int>(src.at<uchar>(190, 220)));
        QCOMPARE(static_cast<int>(out.toMat().at<uchar>(99, 199)), static_cast<int>(src.at<uchar>(289, 419)));
        QVERIFY(portValueMatchesType(out.data("crop_images"), DataType::Table));
        const QVariantList rows = out.data("crop_images").toList();
        QCOMPARE(rows.size(), 1);
        const QVariantMap row = rows.first().toMap();
        QCOMPARE(row.value(QStringLiteral("valid")).toBool(), true);
        QCOMPARE(row.value(QStringLiteral("crop_x")).toInt(), 220);
        QCOMPARE(row.value(QStringLiteral("crop_y")).toInt(), 190);
        QCOMPARE(row.value(QStringLiteral("crop_width")).toInt(), 200);
        QCOMPARE(row.value(QStringLiteral("crop_height")).toInt(), 100);
        QVERIFY(row.value(QStringLiteral("image")).canConvert<ImageData>());
        QCOMPARE(row.value(QStringLiteral("image")).value<ImageData>().toMat().cols, 200);
    }

    void testCropRotated45() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{rect(320, 240, 140, 20, 45)}));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "rotated crop must succeed");
        const QVariantMap row = out.data("crop_images").toList().first().toMap();
        // 包围盒 = 140·cos45 + 20·sin45 ≈ 113.1 → 半开区间取整 114
        QCOMPARE(row.value(QStringLiteral("crop_width")).toInt(), 114);
        QCOMPARE(row.value(QStringLiteral("crop_height")).toInt(), 114);
        QCOMPARE(row.value(QStringLiteral("valid")).toBool(), true);
    }

    void testCropMultipleRectangles() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(
            QJsonArray{rect(100, 100, 80, 60, 0), rect(300, 200, 100, 100, 30), rect(500, 400, 120, 40, -45)}));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "multi-rect crop must succeed");
        QCOMPARE(out.data("crop_count").toDouble(), 3.0);
        const QVariantList rows = out.data("crop_images").toList();
        QCOMPARE(rows.size(), 3);
        for (int i = 0; i < rows.size(); ++i) {
            const QVariantMap row = rows[i].toMap();
            QCOMPARE(row.value(QStringLiteral("index")).toInt(), i);
            QCOMPARE(row.value(QStringLiteral("valid")).toBool(), true);
            const ImageData crop = row.value(QStringLiteral("image")).value<ImageData>();
            QCOMPARE(crop.toMat().cols, row.value(QStringLiteral("crop_width")).toInt());
            QCOMPARE(crop.toMat().rows, row.value(QStringLiteral("crop_height")).toInt());
        }
    }

    void testCropOutsideImageInvalidRow() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{rect(320, 240, 100, 100, 0), rect(5000, 5000, 50, 50, 0)}));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "mixed valid/invalid must succeed");
        QCOMPARE(out.data("crop_count").toDouble(), 1.0); // 无效矩形不计数
        const QVariantList rows = out.data("crop_images").toList();
        QCOMPARE(rows.size(), 2); // 但保留 valid=false 行（不静默丢弃）
        QCOMPARE(rows[1].toMap().value(QStringLiteral("valid")).toBool(), false);
        QCOMPARE(rows[1].toMap().value(QStringLiteral("crop_width")).toInt(), 0);
        QVERIFY(!rows[1].toMap().value(QStringLiteral("image")).canConvert<ImageData>());
    }

    void testCropAllOutsideFails() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{rect(5000, 5000, 50, 50, 0), rect(-9000, 10, 50, 50, 0)}));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData out;
        QVERIFY2(!plugin.execute(input, out), "all-outside must fail closed");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("无交集")),
                 "error must report no intersection");
    }

    void testCropClampedToImage() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{rect(10, 10, 100, 100, 0)})); // 越出左上边界
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "clamped crop must succeed");
        const QVariantMap row = out.data("crop_images").toList().first().toMap();
        QCOMPARE(row.value(QStringLiteral("crop_x")).toInt(), 0);
        QCOMPARE(row.value(QStringLiteral("crop_y")).toInt(), 0);
        QCOMPARE(row.value(QStringLiteral("crop_width")).toInt(), 60);
        QCOMPARE(row.value(QStringLiteral("crop_height")).toInt(), 60);
    }

    void testCropOutputFirstAsImageFalse() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{rect(320, 240, 200, 100, 0)}, false));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "passthrough mode must succeed");
        QCOMPARE(out.toMat().cols, 640); // image 端口原样透传
        QCOMPARE(out.toMat().rows, 480);
        QCOMPARE(out.data("crop_count").toDouble(), 1.0); // 裁剪图组仍在 Table
        QCOMPARE(out.data("crop_images").toList().size(), 1);
    }

    void testCropValidation() {
        CropImagePlugin plugin;
        QString error;
        QVERIFY(plugin.validateParams(cropParams(QJsonArray{rect(0, 0, 10, 10, 0)}), error));
        QVERIFY(!plugin.validateParams(QJsonObject{{"rectangles", 42}, {"outputFirstAsImage", true}}, error)); // 非数组
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{}), error)); // 空数组
        QJsonArray tooMany;
        for (int i = 0; i < 65; ++i) {
            tooMany.append(rect(0, 0, 10, 10, 0));
        }
        QVERIFY(!plugin.validateParams(cropParams(tooMany), error));                                    // 超上限 64
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{QJsonArray{1.0, 2.0, 3.0, 4.0}}), error)); // 4 元
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{QJsonArray{1.0, 2.0, 3.0, 4.0, QStringLiteral("0")}}),
                                       error));                                                  // 字符串元素
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{rect(0, 0, 0, 10, 0)}), error));    // l1=0
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{rect(0, 0, -5, 10, 0)}), error));   // l1<0
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{rect(0, 0, 10, 10, 400)}), error)); // deg 超界
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{rect(1000000.5, 0, 10, 10, 0)}), error)); // cx 超界
        QVERIFY(!plugin.validateParams(QJsonObject{{"rectangles", QJsonArray{rect(0, 0, 10, 10, 0)}},
                                                   {"outputFirstAsImage", QStringLiteral("yes")}},
                                       error));                                                   // 布尔严格
        QVERIFY(plugin.validateParams(cropParams(QJsonArray{rect(0, 0, 1e6, 1e6, 360)}), error)); // 边界合法
    }

    void testCropClone() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{rect(50, 50, 40, 30, 15)}, false));
        IModule* clone = plugin.clone();
        QVERIFY(clone != nullptr);
        auto* cloneCrop = qobject_cast<CropImagePlugin*>(clone);
        QVERIFY(cloneCrop != nullptr);
        QCOMPARE(cloneCrop->currentParams()["outputFirstAsImage"].toBool(), false);
        cloneCrop->setParams(cropParams(QJsonArray{rect(0, 0, 10, 10, 0)}));
        QCOMPARE(plugin.currentParams()["outputFirstAsImage"].toBool(), false); // 原体不受影响
        delete clone;
    }

    // ---------- CalculateOffset ----------

    void testOffsetBasic() {
        CalculateOffsetPlugin plugin;
        plugin.setParams(offsetParams(10, 20, 30, 35, 55, 40));
        QVERIFY(plugin.initialize());
        ImageData input;
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "basic offset must succeed");
        QCOMPARE(out.data("offset_x").toDouble(), 25.0);
        QCOMPARE(out.data("offset_y").toDouble(), 35.0);
        QCOMPARE(out.data("offset_a").toDouble(), 10.0);
    }

    void testOffsetAngleNormalization() {
        CalculateOffsetPlugin plugin;
        QVERIFY(plugin.initialize());
        const auto run = [&plugin](double curA, double tgtA) {
            plugin.setParams(offsetParams(0, 0, curA, 0, 0, tgtA));
            ImageData in;
            ImageData out;
            if (!plugin.execute(in, out)) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return out.data("offset_a").toDouble();
        };
        QCOMPARE(run(170, -170), 20.0); // -340 → +20
        QCOMPARE(run(0, 181), -179.0);  // 181 → -179
        QCOMPARE(run(0, 180), 180.0);   // 上界保留 +180
        QCOMPARE(run(0, -180), 180.0);  // -180 归一化为 +180（(-180,180] 口径）
        QCOMPARE(run(-350, 0), -10.0);  // 350 → -10
        QCOMPARE(run(0, 0), 0.0);
    }

    // Point3D 端口覆盖当前坐标：插件判定与核心契约一致（批1模式），
    // 非 3 数值列表/字符串/QPointF 一律拒绝，非有限值失败关闭
    void testOffsetPointPortContract() {
        CalculateOffsetPlugin plugin;
        plugin.setParams(offsetParams(0, 0, 0, 15, 25, 0));
        QVERIFY(plugin.initialize());

        // 合法：3 数值列表 → 覆盖 currentX/Y
        ImageData okIn;
        okIn.setData("current_point", QVariantList{5.0, 7.0, 9.0});
        QVERIFY(portValueMatchesType(okIn.data("current_point"), DataType::Point3D));
        ImageData okOut;
        QVERIFY2(plugin.execute(okIn, okOut), "Point3D list must override current coords");
        QCOMPARE(okOut.data("offset_x").toDouble(), 10.0); // 15-5
        QCOMPARE(okOut.data("offset_y").toDouble(), 18.0); // 25-7

        // 非法载荷：判定必须与核心契约一致（全部拒绝）
        const QVariantList badPayloads{
            QVariant(QVariantList{5.0, 7.0}),                      // 2 数值（Point2D 不是 Point3D）
            QVariant(QVariantList{QStringLiteral("5"), 7.0, 9.0}), // 字符串元素
            QVariant(QPointF(5, 7)),                               // QPointF 非数值列表
            QVariant(QVariantList{std::numeric_limits<double>::quiet_NaN(), 7.0, 9.0}), // NaN（类型合法值非法）
        };
        for (const QVariant& bad : badPayloads) {
            ImageData in;
            in.setData("current_point", bad);
            QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
            ImageData out;
            QVERIFY2(!plugin.execute(in, out), "illegal current_point must fail closed");
            QVERIFY2(errSpy.count() >= 1, "must emit error");
            const QString msg = errSpy.first().at(0).toString();
            QVERIFY2(msg.contains(QStringLiteral("格式非法")) || msg.contains(QStringLiteral("非有限")),
                     qPrintable(QString("error must be format/non-finite, got: %1").arg(msg)));
        }
        // 无端口时回退参数
        ImageData noPortIn;
        ImageData noPortOut;
        QVERIFY2(plugin.execute(noPortIn, noPortOut), "no port must fall back to params");
        QCOMPARE(noPortOut.data("offset_x").toDouble(), 15.0);
    }

    void testOffsetValidation() {
        CalculateOffsetPlugin plugin;
        QString error;
        QVERIFY(plugin.validateParams(offsetParams(0, 0, 0, 0, 0, 0), error));
        QVERIFY(!plugin.validateParams(offsetParams(0, 0, 400, 0, 0, 0), error));       // 角度超界
        QVERIFY(!plugin.validateParams(offsetParams(1000000.5, 0, 0, 0, 0, 0), error)); // X 超界
        QVERIFY(!plugin.validateParams(QJsonObject{{"currentX", QStringLiteral("1")},
                                                   {"currentY", 0.0},
                                                   {"currentAngle", 0.0},
                                                   {"targetX", 0.0},
                                                   {"targetY", 0.0},
                                                   {"targetAngle", 0.0}},
                                       error));                                               // 字符串数值
        QVERIFY(plugin.validateParams(offsetParams(-1e6, -1e6, -360, 1e6, 1e6, 360), error)); // 边界合法
    }

    void testOffsetClone() {
        CalculateOffsetPlugin plugin;
        plugin.setParams(offsetParams(1, 2, 3, 4, 5, 6));
        IModule* clone = plugin.clone();
        QVERIFY(clone != nullptr);
        auto* cloneOffset = qobject_cast<CalculateOffsetPlugin*>(clone);
        QVERIFY(cloneOffset != nullptr);
        QCOMPARE(cloneOffset->currentParams()["targetX"].toDouble(), 4.0);
        cloneOffset->setParams(offsetParams(0, 0, 0, 0, 0, 0));
        QCOMPARE(plugin.currentParams()["targetX"].toDouble(), 4.0); // 原体不受影响
        delete clone;
    }

    // ---------- 流程验收 ----------

    // 流程验收：GrabImage → CropImage(300×300 中心裁剪) → MeasureCircle——
    // 裁剪图直接驱动下游圆测量（圆心换算到裁剪坐标系 150,150），证明
    // outputFirstAsImage 的 image 端口替换在真实调度中生效
    void testFlowGrabToCropToMeasureCircle() {
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params["grabSource"] = QStringLiteral("Path");
        grab.params["filePath"] = QStringLiteral(TEST_BATCH4_DATA_Circle);
        project->addModule(grab);

        ModuleInstance crop;
        crop.id = QStringLiteral("crop");
        crop.moduleId = QStringLiteral("CropImage");
        QJsonArray rects;
        rects.append(rect(320, 240, 300, 300, 0));
        crop.params["rectangles"] = rects;
        crop.params["outputFirstAsImage"] = true;
        project->addModule(crop);

        ModuleInstance mc;
        mc.id = QStringLiteral("mc");
        mc.moduleId = QStringLiteral("MeasureCircle");
        mc.params["initialCenterX"] = 150.0; // 裁剪坐标系：320-170
        mc.params["initialCenterY"] = 150.0; // 240-90
        mc.params["initialRadius"] = 100.0;
        mc.params["threshold"] = 20.0;
        mc.params["measureCount"] = 36;
        mc.params["searchLength"] = 30.0;
        project->addModule(mc);

        ModuleConnection c1;
        c1.fromModuleId = QStringLiteral("grab");
        c1.toModuleId = QStringLiteral("crop");
        c1.fromPort = QStringLiteral("image");
        c1.toPort = QStringLiteral("image");
        c1.edgeType = QStringLiteral("data");
        project->addConnection(c1);
        ModuleConnection c2;
        c2.fromModuleId = QStringLiteral("crop");
        c2.toModuleId = QStringLiteral("mc");
        c2.fromPort = QStringLiteral("image");
        c2.toPort = QStringLiteral("image");
        c2.edgeType = QStringLiteral("data");
        project->addConnection(c2);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();

        const ImageData cropOut = engine.moduleOutput(QStringLiteral("crop"));
        QCOMPARE(cropOut.data("crop_count").toDouble(), 1.0);
        QCOMPARE(cropOut.toMat().cols, 300);
        QCOMPARE(cropOut.toMat().rows, 300);

        const ImageData mcOut = engine.moduleOutput(QStringLiteral("mc"));
        QVERIFY2(mcOut.hasData("circle_radius"), "cropped image must drive circle measurement");
        QVERIFY2(std::abs(mcOut.data("circle_radius").toDouble() - 100) < 5.0, "radius ~100 in crop frame");
        QVERIFY2(std::abs(mcOut.data("circle_center_x").toDouble() - 150) < 5.0, "center x ~150 in crop frame");
        QVERIFY2(std::abs(mcOut.data("circle_center_y").toDouble() - 150) < 5.0, "center y ~150 in crop frame");
    }

    // 流程验收：MeasurementInput(point_line) → CalculateOffset(current_point)——
    // 跨名端口送达 Point3D 载荷并覆盖参数坐标（offset = target − 端口点）
    void testFlowMeasurementInputToCalculateOffset() {
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        ModuleInstance mi;
        mi.id = QStringLiteral("mi");
        mi.moduleId = QStringLiteral("MeasurementInput");
        mi.params["mode"] = QStringLiteral("point_line");
        mi.params["point"] = QJsonArray{10.0, 20.0, 0.0};
        mi.params["line"] = QJsonArray{0.0, 0.0, 100.0, 0.0};
        project->addModule(mi);

        ModuleInstance co;
        co.id = QStringLiteral("co");
        co.moduleId = QStringLiteral("CalculateOffset");
        co.params["currentX"] = 999.0; // 故意错误：必须被端口覆盖
        co.params["currentY"] = 999.0;
        co.params["currentAngle"] = 30.0;
        co.params["targetX"] = 35.0;
        co.params["targetY"] = 55.0;
        co.params["targetAngle"] = 40.0;
        project->addModule(co);

        ModuleConnection conn;
        conn.fromModuleId = QStringLiteral("mi");
        conn.toModuleId = QStringLiteral("co");
        conn.fromPort = QStringLiteral("point");
        conn.toPort = QStringLiteral("current_point");
        conn.edgeType = QStringLiteral("data");
        project->addConnection(conn);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();

        const ImageData out = engine.moduleOutput(QStringLiteral("co"));
        QVERIFY2(out.hasData("offset_x"), "flow must produce offset_x");
        QCOMPARE(out.data("offset_x").toDouble(), 25.0); // 35-10（端口值生效）
        QCOMPARE(out.data("offset_y").toDouble(), 35.0); // 55-20
        QCOMPARE(out.data("offset_a").toDouble(), 10.0);
    }
};

QTEST_MAIN(TestBatch4)
#include "test_batch4.moc"
