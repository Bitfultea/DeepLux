#include "core/deeplux/DataContract.h"
#include "core/deeplux/PayloadTypes.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"
#include "helpers/SamTestServer.h"
#include "plugins/detection/SamPredict/SamPredictPlugin.h"
#include "plugins/geometry/RegionStats/RegionStatsPlugin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace DeepLux;
using DeepLuxTest::SamTestServer;

/**
 * SAM 批1：Region2D 载荷契约 + SamPredict 生产者 + RegionStats 消费者。
 *
 * 覆盖：契约门禁（强类型/两种 map 形态/负例）、SamPredict 参数校验与
 * 对测试后端（helpers/SamTestServer.h）的成功/超时/后端错误失败关闭、
 * RegionStats 面积/质心/外接矩形数值断言与无效输入拒绝、克隆独立性，
 * 以及 GrabImage → SamPredict → RegionStats 全流程验收（PluginManager
 * 真实加载 + RunEngine 调度）。
 */
class TestSamRegionFlow : public QObject {
    Q_OBJECT

private:
    // 插件库/metadata 路径由 CMake $<TARGET_FILE:...> 注入（跨平台/多配置）
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

    static QJsonObject samParams(const QString& url, const QString& label, int timeoutMs = 5000) {
        return QJsonObject{{"serverUrl", url},
                           {"label", label},
                           {"points", QJsonArray{QJsonArray{320, 240}}},
                           {"negativePoints", QJsonArray{}},
                           {"box", QJsonValue::Null},
                           {"timeoutMs", timeoutMs}};
    }

    QTemporaryDir m_appDir;

private slots:
    void initTestCase() {
        QVERIFY(m_appDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDir.path().toLocal8Bit());
        registerDataContractMetaTypes();
        PluginManager::instance().shutdown();
        const QString pluginRoot = QDir(m_appDir.path()).filePath("plugins");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(TEST_SAMREGION_META_GrabImage),
                               QStringLiteral(TEST_SAMREGION_LIB_GrabImage)),
                 "install GrabImage");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("SamPredict"), QStringLiteral(TEST_SAMREGION_META_SamPredict),
                               QStringLiteral(TEST_SAMREGION_LIB_SamPredict)),
                 "install SamPredict");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("RegionStats"),
                               QStringLiteral(TEST_SAMREGION_META_RegionStats),
                               QStringLiteral(TEST_SAMREGION_LIB_RegionStats)),
                 "install RegionStats");
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        // Region2D 端口若在加载门禁被判为不支持，这里会直接失败（契约放行回归）
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("SamPredict")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("RegionStats")));
    }

    void cleanupTestCase() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        PluginManager::instance().shutdown();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    // ---------- Region2D 契约门禁 ----------

    void testRegion2DContract() {
        // 强类型：polygon >= 3 点有效
        Region2D region;
        region.label = QStringLiteral("obj");
        region.polygon = {QPointF(10, 10), QPointF(20, 10), QPointF(20, 20)};
        region.score = 0.5;
        QVERIFY(portValueMatchesType(QVariant::fromValue(region), DataType::Region2D));

        // 强类型无效：polygon<3 且 bbox 无效 → 拒绝
        Region2D invalid;
        QVERIFY(!portValueMatchesType(QVariant::fromValue(invalid), DataType::Region2D));

        // map 形态：QPointF 元素 + 完整可选字段
        QVariantMap pointForm;
        QVariantList poly1;
        poly1 << QVariant::fromValue(QPointF(0, 0)) << QVariant::fromValue(QPointF(4, 0))
              << QVariant::fromValue(QPointF(0, 3));
        pointForm["polygon"] = poly1;
        pointForm["bbox"] = QVariantList{0, 0, 4, 3};
        pointForm["score"] = 0.9;
        pointForm["label"] = QStringLiteral("tri");
        pointForm["maskRle"] = QStringLiteral("rle");
        QVERIFY(portValueMatchesType(pointForm, DataType::Region2D));

        // map 形态：嵌套数值列表 [[x,y],...]
        // 注意：必须显式 QVariant(...) 包装——append(QVariantList) 会匹配
        // QList::append(const QList&) 重载被展平成标量列表
        QVariantMap nestedForm;
        QVariantList poly2;
        poly2.append(QVariant(QVariantList{0, 0}));
        poly2.append(QVariant(QVariantList{4, 0}));
        poly2.append(QVariant(QVariantList{0, 3}));
        nestedForm["polygon"] = poly2;
        nestedForm["bbox"] = QVariantList{0.0, 0.0, 4.0, 3.0};
        QVERIFY(portValueMatchesType(nestedForm, DataType::Region2D));

        // bbox-only（polygon<3 且 w,h>0）
        QVariantMap bboxOnly;
        bboxOnly["polygon"] = QVariantList{};
        bboxOnly["bbox"] = QVariantList{1, 2, 3, 4};
        QVERIFY(portValueMatchesType(bboxOnly, DataType::Region2D));

        // polygon<3 且 bbox w/h<=0 → 拒绝
        QVariantMap zeroBox = bboxOnly;
        zeroBox["bbox"] = QVariantList{1, 2, 0, 4};
        QVERIFY(!portValueMatchesType(zeroBox, DataType::Region2D));

        // bbox 非 4 数值 → 拒绝
        QVariantMap shortBox = bboxOnly;
        shortBox["bbox"] = QVariantList{1, 2, 3};
        QVERIFY(!portValueMatchesType(shortBox, DataType::Region2D));

        // polygon 元素为字符串 → 拒绝
        QVariantMap stringPoints;
        QVariantList poly3;
        poly3.append(QVariantList{QStringLiteral("0"), QStringLiteral("0")});
        stringPoints["polygon"] = poly3;
        stringPoints["bbox"] = QVariantList{1, 2, 3, 4};
        QVERIFY(!portValueMatchesType(stringPoints, DataType::Region2D));

        // score 越界 → 拒绝
        QVariantMap badScore = bboxOnly;
        badScore["score"] = 1.5;
        QVERIFY(!portValueMatchesType(badScore, DataType::Region2D));

        // 非 Region2D 载荷 → 拒绝
        QVERIFY(!portValueMatchesType(QVariant(42), DataType::Region2D));
        QVERIFY(!portValueMatchesType(QVariant(), DataType::Region2D));
    }

    // ---------- SamPredict 参数校验 ----------

    void testSamPredictParamValidation() {
        SamPredictPlugin plugin;
        QString error;
        const QJsonObject base = samParams(QStringLiteral("http://127.0.0.1:8990"), QStringLiteral("object"), 30000);
        QVERIFY(plugin.validateParams(base, error));

        // 默认参数（无 prompt）必须通过参数校验——prompt 要求在运行期失败关闭
        QVERIFY2(plugin.validateParams(plugin.defaultParams(), error), "default params must validate");

        QJsonObject p = base;
        p["serverUrl"] = QStringLiteral("  ");
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["label"] = 42;
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["points"] = QJsonArray{QJsonArray{QStringLiteral("a"), 2}};
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["points"] = QJsonArray{QJsonArray{1, 2, 3}};
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["negativePoints"] = QJsonArray{QJsonArray{1}};
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["box"] = QJsonArray{1, 2, 0, 4};
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["box"] = QJsonArray{1, 2, 3};
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["points"] = QJsonArray{};
        p["box"] = QJsonArray{1, 2, 3, 4};
        QVERIFY(plugin.validateParams(p, error)); // box-only 合法

        p = base;
        p["timeoutMs"] = 50;
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["timeoutMs"] = 120001;
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["timeoutMs"] = QStringLiteral("3000");
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["timeoutMs"] = 100;
        QVERIFY(plugin.validateParams(p, error)); // 下限含

        p = base;
        p["timeoutMs"] = 120000;
        QVERIFY(plugin.validateParams(p, error)); // 上限含
    }

    // 默认参数（空 prompt）合法但运行期必须失败关闭，且不发起任何网络请求
    void testSamPredictRequiresPromptAtRuntime() {
        SamPredictPlugin plugin;
        QVERIFY(plugin.initialize());
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::white);
        ImageData input(image);
        ImageData output;
        QSignalSpy spy(&plugin, &DeepLux::IModule::errorOccurred);
        QVERIFY(!plugin.execute(input, output));
        QVERIFY(spy.count() >= 1);
        QVERIFY2(spy.first().at(0).toString().contains(QStringLiteral("prompt")),
                 qPrintable(spy.first().at(0).toString()));
        QVERIFY(!output.hasData("region")); // 不伪造区域
    }

    // ---------- SamPredict × SamTestServer ----------

    void testSamPredictSuccess() {
        SamTestServer server;
        QVERIFY(server.listen());

        SamPredictPlugin plugin;
        QVERIFY(plugin.initialize());
        plugin.setParams(samParams(server.url(), QStringLiteral("widget")));
        QCOMPARE(plugin.currentParams()["serverUrl"].toString(), server.url());

        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(Qt::gray);
        ImageData input(image);
        ImageData output;
        QVERIFY2(plugin.execute(input, output), "predict must succeed against test server");

        const QVariant regionVar = output.data("region");
        QVERIFY(regionVar.canConvert<Region2D>());
        const Region2D region = regionVar.value<Region2D>();
        QCOMPARE(region.label, QStringLiteral("widget"));
        QCOMPARE(region.polygon.size(), 4);
        QCOMPARE(region.polygon[0], QPointF(10, 10));
        QCOMPARE(region.polygon[2], QPointF(20, 20));
        QCOMPARE(region.bbox, QRectF(10, 10, 10, 10));
        QVERIFY(std::abs(region.score - 0.97) < 1e-9);
        QVERIFY(region.isValid());
        // 产物必须满足 Region2D 端口契约（下游可直接消费）
        QVERIFY(portValueMatchesType(regionVar, DataType::Region2D));
        QVERIFY(std::abs(output.data("score").toDouble() - 0.97) < 1e-9);

        // 真实调用 set_image + predict，且 prompt 参数透传后端
        QVERIFY2(server.requestLog.contains(QStringLiteral("POST /set_image")),
                 qPrintable(server.requestLog.join(QStringLiteral(", "))));
        QVERIFY(server.requestLog.contains(QStringLiteral("POST /predict")));
        QVERIFY(!server.lastSetImagePath.isEmpty());
        QCOMPARE(server.lastPredictBody.value("embedding_id").toString(), QStringLiteral("emb-1"));
        const QJsonArray sentPos = server.lastPredictBody.value("points_pos").toArray();
        QCOMPARE(sentPos.size(), 1);
        QCOMPARE(sentPos[0].toArray()[0].toDouble(), 320.0);
        QCOMPARE(sentPos[0].toArray()[1].toDouble(), 240.0);
        QVERIFY(!server.lastPredictBody.contains("box")); // 无效 box 不发送
    }

    void testSamPredictTimeoutFailClosed() {
        SamTestServer server;
        QVERIFY(server.listen());
        server.hangPaths.insert(QStringLiteral("/predict"));

        SamPredictPlugin plugin;
        QVERIFY(plugin.initialize());
        plugin.setParams(samParams(server.url(), QStringLiteral("widget"), 300));

        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::white);
        ImageData input(image);
        ImageData output;
        QSignalSpy spy(&plugin, &DeepLux::IModule::errorOccurred);
        QVERIFY2(!plugin.execute(input, output), "hung predict must fail closed");
        QVERIFY(spy.count() >= 1);
        QVERIFY2(spy.first().at(0).toString().contains(QStringLiteral("超时")),
                 qPrintable(spy.first().at(0).toString()));
        QVERIFY(!output.hasData("region")); // 超时不伪造区域
    }

    void testSamPredictBackendErrorFailClosed() {
        SamTestServer server;
        QVERIFY(server.listen());
        server.predictStatus = QStringLiteral("error");

        SamPredictPlugin plugin;
        QVERIFY(plugin.initialize());
        plugin.setParams(samParams(server.url(), QStringLiteral("widget")));

        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::white);
        ImageData input(image);
        ImageData output;
        QSignalSpy spy(&plugin, &DeepLux::IModule::errorOccurred);
        QVERIFY2(!plugin.execute(input, output), "backend error must fail closed");
        QVERIFY(spy.count() >= 1);
        QVERIFY2(spy.first().at(0).toString().contains(QStringLiteral("预测失败")),
                 qPrintable(spy.first().at(0).toString()));
        QVERIFY(!output.hasData("region")); // 后端错误不伪造区域
    }

    // ---------- RegionStats ----------

    void testRegionStatsStrongTypeSquare() {
        RegionStatsPlugin plugin;
        QVERIFY(plugin.initialize());
        Region2D region;
        region.label = QStringLiteral("sq");
        region.polygon = {QPointF(10, 10), QPointF(20, 10), QPointF(20, 20), QPointF(10, 20)};
        region.bbox = QRectF(10, 10, 10, 10);
        region.score = 0.97;
        ImageData input;
        input.setData("region", QVariant::fromValue(region));
        ImageData output;
        QVERIFY(plugin.execute(input, output));
        QCOMPARE(output.data("region_area").toDouble(), 100.0); // shoelace 精确
        QCOMPARE(output.data("point_count").toInt(), 4);
        QCOMPARE(output.data("centroid_x").toDouble(), 15.0);
        QCOMPARE(output.data("centroid_y").toDouble(), 15.0);
        QCOMPARE(output.data("bbox_x").toDouble(), 10.0);
        QCOMPARE(output.data("bbox_y").toDouble(), 10.0);
        QCOMPARE(output.data("bbox_w").toDouble(), 10.0);
        QCOMPARE(output.data("bbox_h").toDouble(), 10.0);
        QCOMPARE(output.data("score").toDouble(), 0.97);
        QCOMPARE(output.data("label").toString(), QStringLiteral("sq"));
        // 输出满足声明端口契约（Number/String）
        QVERIFY(portValueMatchesType(output.data("region_area"), DataType::Number));
        QVERIFY(portValueMatchesType(output.data("label"), DataType::String));
    }

    void testRegionStatsMapForms() {
        RegionStatsPlugin plugin;
        QVERIFY(plugin.initialize());

        // 嵌套数值列表三角形：面积 6，质心 (4/3, 1)
        QVariantMap nested;
        QVariantList poly;
        poly.append(QVariant(QVariantList{0, 0}));
        poly.append(QVariant(QVariantList{4, 0}));
        poly.append(QVariant(QVariantList{0, 3}));
        nested["polygon"] = poly;
        nested["bbox"] = QVariantList{0, 0, 4, 3};
        nested["score"] = 0.5;
        nested["label"] = QStringLiteral("tri");
        ImageData nestedInput;
        nestedInput.setData("region", nested);
        ImageData nestedOut;
        QVERIFY(plugin.execute(nestedInput, nestedOut));
        QVERIFY(std::abs(nestedOut.data("region_area").toDouble() - 6.0) < 1e-9);
        QVERIFY(std::abs(nestedOut.data("centroid_x").toDouble() - 4.0 / 3.0) < 1e-9);
        QVERIFY(std::abs(nestedOut.data("centroid_y").toDouble() - 1.0) < 1e-9);
        QCOMPARE(nestedOut.data("point_count").toInt(), 3);
        QCOMPARE(nestedOut.data("label").toString(), QStringLiteral("tri"));

        // QPointF 元素形态同样接受
        QVariantMap pointForm;
        QVariantList poly2;
        poly2 << QVariant::fromValue(QPointF(0, 0)) << QVariant::fromValue(QPointF(4, 0))
              << QVariant::fromValue(QPointF(0, 3));
        pointForm["polygon"] = poly2;
        pointForm["bbox"] = QVariantList{0, 0, 4, 3};
        ImageData pointInput;
        pointInput.setData("region", pointForm);
        ImageData pointOut;
        QVERIFY(plugin.execute(pointInput, pointOut));
        QVERIFY(std::abs(pointOut.data("region_area").toDouble() - 6.0) < 1e-9);

        // bbox-only（polygon<3）：回退 bbox 面积与中心
        QVariantMap bboxOnly;
        bboxOnly["polygon"] = QVariantList{};
        bboxOnly["bbox"] = QVariantList{0, 0, 4, 5};
        ImageData bboxInput;
        bboxInput.setData("region", bboxOnly);
        ImageData bboxOut;
        QVERIFY(plugin.execute(bboxInput, bboxOut));
        QCOMPARE(bboxOut.data("region_area").toDouble(), 20.0);
        QCOMPARE(bboxOut.data("point_count").toInt(), 0);
        QCOMPARE(bboxOut.data("centroid_x").toDouble(), 2.0);
        QCOMPARE(bboxOut.data("centroid_y").toDouble(), 2.5);
        QCOMPARE(bboxOut.data("score").toDouble(), 0.0);       // 缺省
        QCOMPARE(bboxOut.data("label").toString(), QString()); // 缺省
    }

    // polygon-only 强类型（bbox 无效）：外接矩形由多边形推导
    void testRegionStatsDerivesBboxFromPolygon() {
        RegionStatsPlugin plugin;
        QVERIFY(plugin.initialize());
        Region2D region;
        region.polygon = {QPointF(0, 0), QPointF(4, 0), QPointF(0, 3)};
        ImageData input;
        input.setData("region", QVariant::fromValue(region));
        ImageData output;
        QVERIFY(plugin.execute(input, output));
        QCOMPARE(output.data("bbox_x").toDouble(), 0.0);
        QCOMPARE(output.data("bbox_y").toDouble(), 0.0);
        QCOMPARE(output.data("bbox_w").toDouble(), 4.0);
        QCOMPARE(output.data("bbox_h").toDouble(), 3.0);
        QVERIFY(std::abs(output.data("region_area").toDouble() - 6.0) < 1e-9);
    }

    void testRegionStatsRejectsInvalid() {
        RegionStatsPlugin plugin;
        QVERIFY(plugin.initialize());

        // 缺失 region
        ImageData missing;
        ImageData missingOut;
        QSignalSpy missingSpy(&plugin, &DeepLux::IModule::errorOccurred);
        QVERIFY(!plugin.execute(missing, missingOut));
        QVERIFY2(missingSpy.first().at(0).toString().contains(QStringLiteral("未提供")),
                 qPrintable(missingSpy.first().at(0).toString()));

        // 字符串载荷
        ImageData strInput;
        strInput.setData("region", QStringLiteral("not-a-region"));
        ImageData strOut;
        QSignalSpy strSpy(&plugin, &DeepLux::IModule::errorOccurred);
        QVERIFY(!plugin.execute(strInput, strOut));
        QVERIFY2(strSpy.first().at(0).toString().contains(QStringLiteral("契约")),
                 qPrintable(strSpy.first().at(0).toString()));

        // 缺 bbox 的 map（polygon<3 时契约要求 bbox 有效）
        QVariantMap noBbox;
        QVariantList poly;
        poly.append(QVariant(QVariantList{0, 0}));
        poly.append(QVariant(QVariantList{1, 1}));
        noBbox["polygon"] = poly;
        ImageData noBboxInput;
        noBboxInput.setData("region", noBbox);
        ImageData noBboxOut;
        QVERIFY(!plugin.execute(noBboxInput, noBboxOut));

        // score 越界的 map
        QVariantMap badScore;
        badScore["polygon"] = QVariantList{};
        badScore["bbox"] = QVariantList{0, 0, 2, 2};
        badScore["score"] = 1.5;
        ImageData badScoreInput;
        badScoreInput.setData("region", badScore);
        ImageData badScoreOut;
        QVERIFY(!plugin.execute(badScoreInput, badScoreOut));
    }

    // ---------- 克隆独立性 ----------

    void testCloneIndependence() {
        SamPredictPlugin sam;
        sam.setParams(samParams(QStringLiteral("http://127.0.0.1:9"), QStringLiteral("a"), 1000));
        QCOMPARE(sam.currentParams()["label"].toString(), QStringLiteral("a"));
        IModule* c1 = sam.clone();
        auto* samClone = qobject_cast<SamPredictPlugin*>(c1);
        QVERIFY(samClone != nullptr);
        QVERIFY(samClone != &sam);
        QCOMPARE(samClone->currentParams()["serverUrl"].toString(), QStringLiteral("http://127.0.0.1:9"));
        QCOMPARE(samClone->currentParams()["timeoutMs"].toInt(), 1000);
        samClone->setParams(QJsonObject{{"label", QStringLiteral("b")}});
        QCOMPARE(samClone->currentParams()["label"].toString(), QStringLiteral("b"));
        QCOMPARE(sam.currentParams()["label"].toString(), QStringLiteral("a")); // 原体不受影响
        delete c1;

        RegionStatsPlugin stats;
        IModule* c2 = stats.clone();
        QVERIFY(c2 != nullptr);
        QVERIFY(c2 != &stats);
        QCOMPARE(qobject_cast<RegionStatsPlugin*>(c2)->moduleId(), QStringLiteral("com.deeplux.plugin.regionstats"));
        delete c2;
    }

    // ---------- 流程验收 ----------

    // GrabImage → SamPredict(image) → RegionStats(region)：PluginManager 真实加载 +
    // RunEngine 调度；SamPredict 在引擎调用线程内以本地事件循环同步收口，
    // 测试后端（同线程 QTcpServer）由同一事件循环驱动。
    void testFlowGrabImageSamPredictRegionStats() {
        SamTestServer server;
        QVERIFY(server.listen());

        RunEngine& engine = RunEngine::instance();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params["grabSource"] = QStringLiteral("Path");
        grab.params["filePath"] = QStringLiteral(TEST_SAMREGION_DATA_Circle);
        project->addModule(grab);

        ModuleInstance sam;
        sam.id = QStringLiteral("sam");
        sam.moduleId = QStringLiteral("SamPredict");
        sam.params["serverUrl"] = server.url();
        sam.params["label"] = QStringLiteral("circle");
        sam.params["points"] = QJsonArray{QJsonArray{320, 240}};
        sam.params["timeoutMs"] = 5000;
        project->addModule(sam);

        ModuleInstance stats;
        stats.id = QStringLiteral("stats");
        stats.moduleId = QStringLiteral("RegionStats");
        project->addModule(stats);

        ModuleConnection imageConn;
        imageConn.fromModuleId = QStringLiteral("grab");
        imageConn.toModuleId = QStringLiteral("sam");
        imageConn.fromPort = QStringLiteral("image");
        imageConn.toPort = QStringLiteral("image");
        imageConn.edgeType = QStringLiteral("data");
        project->addConnection(imageConn);

        ModuleConnection regionConn;
        regionConn.fromModuleId = QStringLiteral("sam");
        regionConn.toModuleId = QStringLiteral("stats");
        regionConn.fromPort = QStringLiteral("region");
        regionConn.toPort = QStringLiteral("region");
        regionConn.edgeType = QStringLiteral("data");
        project->addConnection(regionConn);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();

        // SamPredict 输出：强类型 Region2D 进入端口缓存（跨节点送达）
        const ImageData samOut = engine.moduleOutput(QStringLiteral("sam"));
        QVERIFY2(samOut.hasData("region"), "flow must produce region");
        QVERIFY(portValueMatchesType(samOut.data("region"), DataType::Region2D));

        // RegionStats 输出：测试服务固定方形 polygon (10,10)-(20,20) → 面积 100
        const ImageData statsOut = engine.moduleOutput(QStringLiteral("stats"));
        QVERIFY2(statsOut.hasData("region_area"), "flow must produce region_area");
        QVERIFY(std::abs(statsOut.data("region_area").toDouble() - 100.0) < 1e-9);
        QCOMPARE(statsOut.data("point_count").toInt(), 4);
        QVERIFY(std::abs(statsOut.data("centroid_x").toDouble() - 15.0) < 1e-9);
        QVERIFY(std::abs(statsOut.data("score").toDouble() - 0.97) < 1e-9);
        QCOMPARE(statsOut.data("label").toString(), QStringLiteral("circle"));
        QVERIFY2(server.requestLog.contains(QStringLiteral("POST /set_image")),
                 qPrintable(server.requestLog.join(QStringLiteral(", "))));
        QVERIFY(server.requestLog.contains(QStringLiteral("POST /predict")));

        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testPluginInfo() {
        SamPredictPlugin sam;
        QCOMPARE(sam.moduleId(), QStringLiteral("com.deeplux.plugin.sampredict"));
        QCOMPARE(sam.category(), QStringLiteral("detection"));
        RegionStatsPlugin stats;
        QCOMPARE(stats.moduleId(), QStringLiteral("com.deeplux.plugin.regionstats"));
        QCOMPARE(stats.category(), QStringLiteral("geometry"));
    }
};

QTEST_MAIN(TestSamRegionFlow)
#include "test_sam_region_flow.moc"
