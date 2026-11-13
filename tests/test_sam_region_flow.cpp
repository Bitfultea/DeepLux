#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "core/common/CancellationToken.h"
#include "core/common/UserManager.h"
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
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <cmath>
#include <limits>

using namespace DeepLux;
using DeepLuxTest::SamTestServer;

class ScopedUserSessionRestore final {
public:
    explicit ScopedUserSessionRestore(UserInfo previousUser) : m_previousUser(std::move(previousUser)) {}

    ~ScopedUserSessionRestore() {
        if (m_previousUser.isLoggedIn)
            UserManager::instance().login(m_previousUser.userName, QString());
        else
            UserManager::instance().logout();
    }

private:
    UserInfo m_previousUser;
};

class RegionImageFixtureModule final : public ModuleBase {
public:
    explicit RegionImageFixtureModule(bool withProvenance, bool withRegion = true)
        : m_withProvenance(withProvenance), m_withRegion(withRegion) {
        m_moduleId = QStringLiteral("RegionImageFixture");
        m_name = QStringLiteral("Region and image fixture");
        m_category = QStringLiteral("test");
        PortSpec image;
        image.id = QStringLiteral("image");
        image.type = DataType::Image2D;
        PortSpec region;
        region.id = QStringLiteral("region");
        region.type = DataType::Region2D;
        setPorts({}, {image, region});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(32, 32, QImage::Format_RGB32));
        if (m_withProvenance) {
            ImageCoordinateProvenance provenance;
            provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
            provenance.sourceSize = QSize(200, 200);
            provenance.localOriginInSource = QPointF(50, 40);
            output.setCoordinateProvenance(provenance);
        }
        if (m_withRegion) {
            Region2D region;
            region.polygon = {QPointF(120, 120), QPointF(140, 120), QPointF(140, 140), QPointF(120, 140)};
            region.bbox = QRectF(120, 120, 20, 20);
            output.setData(QStringLiteral("region"), QVariant::fromValue(region));
        }
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    bool m_withProvenance;
    bool m_withRegion;
};

class RegionStatsCentroidFixtureModule final : public ModuleBase {
public:
    explicit RegionStatsCentroidFixtureModule(QVariant centroid) : m_centroid(std::move(centroid)) {
        m_moduleId = QStringLiteral("RegionStats");
        m_name = QStringLiteral("Malformed RegionStats centroid fixture");
        PortSpec centroidPort;
        centroidPort.id = QStringLiteral("centroid");
        centroidPort.type = DataType::Point2D;
        setPorts({}, {centroidPort});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(16, 16, QImage::Format_RGB32));
        output.setData(QStringLiteral("centroid"), m_centroid);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QVariant m_centroid;
};

/**
 * SAM 批1：Region2D 载荷契约 + SamPredict 生产者 + RegionStats 消费者。
 *
 * 覆盖：契约门禁（强类型/两种 map 形态/负例，含批1 复核二轮 P2-1 非有限
 * 数值 NaN/Inf 拒绝）、SamPredict 参数校验与对测试后端
 * （helpers/SamTestServer.h）的成功/超时/取消（P2-2）/后端错误失败关闭、
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
                           {"points", QJsonArray{QJsonValue(QJsonArray{320, 240})}},
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
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("CropImage"), QStringLiteral(TEST_SAMREGION_META_CropImage),
                               QStringLiteral(TEST_SAMREGION_LIB_CropImage)),
                 "install CropImage");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("RegionStats"),
                               QStringLiteral(TEST_SAMREGION_META_RegionStats),
                               QStringLiteral(TEST_SAMREGION_LIB_RegionStats)),
                 "install RegionStats");
        const QString fixtureDir = QDir(pluginRoot).filePath(QStringLiteral("RegionImageFixture"));
        QVERIFY(QDir().mkpath(fixtureDir));
        QFile fixtureMetadata(QDir(fixtureDir).filePath(QStringLiteral("metadata.json")));
        QVERIFY(fixtureMetadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray fixtureJson =
            R"({"id":"com.deeplux.tests.regionimagefixture","name":"RegionImageFixture","category":"test","version":"1.0","ports":{"inputs":[],"outputs":[{"id":"image","displayName":"Image","type":"Image2D","required":false,"multiple":false},{"id":"region","displayName":"Region","type":"Region2D","required":false,"multiple":false}]}})";
        QCOMPARE(fixtureMetadata.write(fixtureJson), static_cast<qint64>(fixtureJson.size()));
        fixtureMetadata.close();
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        // Region2D 端口若在加载门禁被判为不支持，这里会直接失败（契约放行回归）
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("SamPredict")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("CropImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("RegionStats")));
        PluginManager::instance().scanPlugins();
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

        // 批1 复核二轮（P2-1）：非有限数值失败关闭——isNumeric 仅查类型，
        // NaN/Inf 必须显式拒绝（score=NaN 因 [0,1] 两端比较均假而会放行）
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (const double bad : {nan, inf, -inf}) {
            // map: bbox 分量非有限（polygon<3 时 NaN<=0 为假，旧校验会放行）
            QVariantMap badBox = bboxOnly;
            badBox["bbox"] = QVariant(QVariantList{1.0, 2.0, bad, 4.0});
            QVERIFY2(!portValueMatchesType(badBox, DataType::Region2D), "non-finite bbox must be rejected");
            // map: score 非有限
            QVariantMap badScoreNonFinite = bboxOnly;
            badScoreNonFinite["score"] = bad;
            QVERIFY2(!portValueMatchesType(badScoreNonFinite, DataType::Region2D), "non-finite score must be rejected");
            // 强类型：bbox x 非有限但 isValid() 仅查宽高（仍判"有效"）
            Region2D strongBadBox;
            strongBadBox.polygon = {QPointF(0, 0), QPointF(4, 0), QPointF(0, 3)};
            strongBadBox.bbox = QRectF(bad, 0, 4, 3);
            QVERIFY2(!portValueMatchesType(QVariant::fromValue(strongBadBox), DataType::Region2D),
                     "non-finite strong bbox must be rejected");
        }

        // 批3 复核二轮（P2-6）：NaN 宽/高 bbox——isValid() 为假会跳过条件式
        // 检查，必须被无条件有限性检查拒绝（旧实现 polygon≥3 即放行，
        // 消费者直读 bbox 会泄漏 NaN）
        Region2D nanWidthBox;
        nanWidthBox.polygon = {QPointF(0, 0), QPointF(4, 0), QPointF(0, 3)};
        nanWidthBox.bbox = QRectF(0, 0, nan, 3); // isValid()==false 但携带 NaN
        QVERIFY(!portValueMatchesType(QVariant::fromValue(nanWidthBox), DataType::Region2D));

        // map: polygon 嵌套数值列表含 NaN → 拒绝
        QVariantMap nanPoly;
        QVariantList polyN;
        polyN.append(QVariant(QVariantList{0.0, 0.0}));
        polyN.append(QVariant(QVariantList{nan, 0.0}));
        polyN.append(QVariant(QVariantList{0.0, 3.0}));
        nanPoly["polygon"] = polyN;
        nanPoly["bbox"] = QVariant(QVariantList{0.0, 0.0, 4.0, 3.0});
        QVERIFY(!portValueMatchesType(nanPoly, DataType::Region2D));

        // map: polygon QPointF 元素含 Inf → 拒绝
        QVariantMap infPoint;
        QVariantList polyI;
        polyI << QVariant::fromValue(QPointF(0, 0)) << QVariant::fromValue(QPointF(inf, 0))
              << QVariant::fromValue(QPointF(0, 3));
        infPoint["polygon"] = polyI;
        infPoint["bbox"] = QVariant(QVariantList{0.0, 0.0, 4.0, 3.0});
        QVERIFY(!portValueMatchesType(infPoint, DataType::Region2D));

        // 强类型：polygon 含 NaN / score=NaN 或越界 / bbox 宽=Inf → 拒绝
        Region2D strong;
        strong.polygon = {QPointF(0, 0), QPointF(4, 0), QPointF(0, 3)};
        QVERIFY(portValueMatchesType(QVariant::fromValue(strong), DataType::Region2D));
        Region2D nanPolyStrong = strong;
        nanPolyStrong.polygon[1] = QPointF(nan, 0);
        QVERIFY(!portValueMatchesType(QVariant::fromValue(nanPolyStrong), DataType::Region2D));
        Region2D nanScoreStrong = strong;
        nanScoreStrong.score = nan;
        QVERIFY(!portValueMatchesType(QVariant::fromValue(nanScoreStrong), DataType::Region2D));
        Region2D highScoreStrong = strong;
        highScoreStrong.score = 1.5;
        QVERIFY(!portValueMatchesType(QVariant::fromValue(highScoreStrong), DataType::Region2D));
        Region2D infBoxStrong;
        infBoxStrong.bbox = QRectF(0, 0, inf, 3); // isValid 判"有效"，须被有限值检查拒绝
        QVERIFY(!portValueMatchesType(QVariant::fromValue(infBoxStrong), DataType::Region2D));

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
        p["points"] = QJsonArray{QJsonValue(QJsonArray{QStringLiteral("a"), 2})};
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["points"] = QJsonArray{QJsonValue(QJsonArray{1, 2, 3})};
        QVERIFY(!plugin.validateParams(p, error));

        p = base;
        p["negativePoints"] = QJsonArray{QJsonValue(QJsonArray{1})};
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

    // 批1 复核二轮（P2-2）：嵌套事件循环等待期间引擎 stop()（取消令牌）必须被
    // 及时响应——不得干等客户端超时（5000ms）或外层 deadline（timeoutMs+2s）
    void testSamPredictCancellationResponsive() {
        SamTestServer server;
        QVERIFY(server.listen());
        server.hangPaths.insert(QStringLiteral("/predict"));

        SamPredictPlugin plugin;
        QVERIFY(plugin.initialize());
        plugin.setParams(samParams(server.url(), QStringLiteral("widget"), 5000));

        CancellationToken token;
        plugin.setCancellationToken(&token);
        // 200ms 后取消：定时器事件在 process 的嵌套事件循环内触发
        QTimer::singleShot(200, &token, &CancellationToken::cancel);

        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::white);
        ImageData input(image);
        ImageData output;
        QSignalSpy spy(&plugin, &DeepLux::IModule::errorOccurred);
        QElapsedTimer timer;
        timer.start();
        QVERIFY2(!plugin.execute(input, output), "cancelled predict must fail closed");
        const qint64 elapsed = timer.elapsed();
        // 取消轮询 100ms 间隔：~300ms 内必须退出；回归到无轮询实现则为 5000ms+（红）
        QVERIFY2(elapsed < 3000, qPrintable(QString("cancellation must be responsive, elapsed=%1ms").arg(elapsed)));
        QVERIFY(spy.count() >= 1);
        QVERIFY2(spy.first().at(0).toString().contains(QStringLiteral("取消")),
                 qPrintable(spy.first().at(0).toString()));
        QVERIFY(!output.hasData("region")); // 取消不伪造区域
        plugin.setCancellationToken(nullptr);
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
        input.setData("centroid", QPointF(-1, -1));
        ImageData output;
        QVERIFY(plugin.execute(input, output));
        QCOMPARE(output.data("region_area").toDouble(), 100.0); // shoelace 精确
        QCOMPARE(output.data("centroid").toPointF(), QPointF(15, 15));
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
        strInput.setData("centroid", QPointF(-1, -1));
        ImageData strOut;
        QSignalSpy strSpy(&plugin, &DeepLux::IModule::errorOccurred);
        QVERIFY(!plugin.execute(strInput, strOut));
        QVERIFY2(strSpy.first().at(0).toString().contains(QStringLiteral("契约")),
                 qPrintable(strSpy.first().at(0).toString()));
        QVERIFY(!strOut.hasData(QStringLiteral("centroid")));

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

        // 批1 复核二轮（P2-1）：NaN bbox 被契约门禁拒绝，不输出 NaN 面积
        QVariantMap nanBox;
        nanBox["polygon"] = QVariant(QVariantList{});
        nanBox["bbox"] = QVariant(QVariantList{0.0, 0.0, std::numeric_limits<double>::quiet_NaN(), 4.0});
        ImageData nanInput;
        nanInput.setData("region", nanBox);
        ImageData nanOut;
        QSignalSpy nanSpy(&plugin, &DeepLux::IModule::errorOccurred);
        QVERIFY(!plugin.execute(nanInput, nanOut));
        QVERIFY2(nanSpy.first().at(0).toString().contains(QStringLiteral("契约")),
                 qPrintable(nanSpy.first().at(0).toString()));
        QVERIFY(!nanOut.hasData("region_area"));
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
        sam.params["points"] = QJsonArray{QJsonValue(QJsonArray{320, 240})};
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
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY(engine.runObservation(runId).result.success);

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
        QVERIFY(std::abs(statsOut.data("centroid_y").toDouble() - 15.0) < 1e-9);
        QVERIFY(statsOut.data("centroid").canConvert<QPointF>());
        QVERIFY(!statsOut.coordinateProvenance().isValid());
        ToolSchema::instance().registerDefaultTools();
        const QJsonObject centroid = AgentActor().executeTool(QStringLiteral("get_module_output"),
                                                              QJsonObject{{"runId", runId},
                                                                          {"instanceId", QStringLiteral("stats")},
                                                                          {"port", QStringLiteral("centroid")}});
        QCOMPARE(centroid.value(QStringLiteral("value")).toArray(), (QJsonArray{15, 15}));
        QVERIFY(!centroid.contains(QStringLiteral("sourceImageCoordinates")));
        QVERIFY(std::abs(statsOut.data("score").toDouble() - 0.97) < 1e-9);
        QCOMPARE(statsOut.data("label").toString(), QStringLiteral("circle"));
        QVERIFY2(server.requestLog.contains(QStringLiteral("POST /set_image")),
                 qPrintable(server.requestLog.join(QStringLiteral(", "))));
        QVERIFY(server.requestLog.contains(QStringLiteral("POST /predict")));

        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentSamPredictRegionCoordinatesAfterNonzeroCrop() {
        ScopedUserSessionRestore userSession(UserManager::instance().currentUser());
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        SamTestServer server;
        QVERIFY(server.listen());
        server.usePredictOverride = true;
        server.predictResponseOverride = QJsonObject{
            {QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("polygon"), QJsonArray{QJsonArray{10, 10}, QJsonArray{20, 10}, QJsonArray{20, 14},
                                                   QJsonArray{14, 14}, QJsonArray{14, 20}, QJsonArray{10, 20}}},
            {QStringLiteral("bbox"), QJsonArray{10, 10, 10, 10}},
            {QStringLiteral("score"), 0.97}};
        ToolSchema::instance().registerDefaultTools();

        RunEngine& engine = RunEngine::instance();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);

        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params[QStringLiteral("grabSource")] = QStringLiteral("Path");
        grab.params[QStringLiteral("filePath")] = QStringLiteral(TEST_SAMREGION_DATA_Circle);
        project->addModule(grab);

        ModuleInstance crop;
        crop.id = QStringLiteral("crop");
        crop.moduleId = QStringLiteral("CropImage");
        crop.params[QStringLiteral("rectangles")] = QJsonArray{QJsonValue(QJsonArray{280, 220, 200, 150, 0})};
        crop.params[QStringLiteral("outputFirstAsImage")] = true;
        project->addModule(crop);

        ModuleInstance sam;
        sam.id = QStringLiteral("sam");
        sam.moduleId = QStringLiteral("SamPredict");
        const QJsonObject successfulSamParams = samParams(server.url(), QStringLiteral("concave-L"));
        sam.params = successfulSamParams;
        project->addModule(sam);

        ModuleInstance stats;
        stats.id = QStringLiteral("stats");
        stats.moduleId = QStringLiteral("RegionStats");
        project->addModule(stats);

        ModuleInstance mismatchedStats;
        mismatchedStats.id = QStringLiteral("stats_mismatched_carrier");
        mismatchedStats.moduleId = QStringLiteral("RegionStats");
        project->addModule(mismatchedStats);

        ModuleInstance missingImageStats;
        missingImageStats.id = QStringLiteral("stats_missing_image");
        missingImageStats.moduleId = QStringLiteral("RegionStats");
        project->addModule(missingImageStats);

        const auto connectPorts = [project](const QString& from, const QString& to) {
            ModuleConnection connection;
            connection.fromModuleId = from;
            connection.toModuleId = to;
            connection.fromPort = QStringLiteral("image");
            connection.toPort = QStringLiteral("image");
            connection.edgeType = QStringLiteral("data");
            project->addConnection(connection);
        };
        connectPorts(grab.id, crop.id);
        connectPorts(crop.id, sam.id);
        const auto connectPort = [project](const QString& from, const QString& fromPort, const QString& to,
                                           const QString& toPort) {
            ModuleConnection connection;
            connection.fromModuleId = from;
            connection.fromPort = fromPort;
            connection.toModuleId = to;
            connection.toPort = toPort;
            connection.edgeType = QStringLiteral("data");
            project->addConnection(connection);
        };
        connectPort(sam.id, QStringLiteral("region"), stats.id, QStringLiteral("region"));
        connectPort(sam.id, QStringLiteral("image"), stats.id, QStringLiteral("image"));
        connectPort(sam.id, QStringLiteral("region"), mismatchedStats.id, QStringLiteral("region"));
        connectPort(crop.id, QStringLiteral("image"), mismatchedStats.id, QStringLiteral("image"));
        connectPort(sam.id, QStringLiteral("region"), missingImageStats.id, QStringLiteral("region"));
        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = QStringLiteral("main");
        flow.nodeIds = QStringList{grab.id, crop.id, sam.id, stats.id, mismatchedStats.id, missingImageStats.id};
        project->setFlows({flow});

        QVERIFY(engine.loadProject(project));
        AgentActor actor;

        const QJsonObject rejectedParams =
            actor.executeTool(QStringLiteral("set_module_params"),
                              QJsonObject{{"instanceId", sam.id}, {"values", QJsonObject{{"serverUrl", QString()}}}});
        QCOMPARE(rejectedParams.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(rejectedParams.value(QStringLiteral("error")).toString(),
                 QStringLiteral("serverUrl 必须为非空字符串"));
        const auto unchangedSam = project->moduleById(sam.id);
        QVERIFY(unchangedSam.has_value());
        QCOMPARE(unchangedSam->params.value(QStringLiteral("serverUrl")).toString(),
                 successfulSamParams.value(QStringLiteral("serverUrl")).toString());
        QCOMPARE(unchangedSam->params.value(QStringLiteral("label")).toString(),
                 successfulSamParams.value(QStringLiteral("label")).toString());
        QCOMPARE(unchangedSam->params.value(QStringLiteral("points")).toArray(),
                 successfulSamParams.value(QStringLiteral("points")).toArray());
        QCOMPARE(unchangedSam->params.value(QStringLiteral("negativePoints")).toArray(),
                 successfulSamParams.value(QStringLiteral("negativePoints")).toArray());
        QVERIFY(unchangedSam->params.value(QStringLiteral("box")).isNull());
        QVERIFY(successfulSamParams.value(QStringLiteral("box")).isNull());
        QCOMPARE(unchangedSam->params.value(QStringLiteral("timeoutMs")).toInt(),
                 successfulSamParams.value(QStringLiteral("timeoutMs")).toInt());
        QVERIFY(!engine.isBusy());

        QJsonObject noPromptParams = successfulSamParams;
        noPromptParams[QStringLiteral("points")] = QJsonArray{};
        noPromptParams[QStringLiteral("negativePoints")] = QJsonArray{};
        noPromptParams[QStringLiteral("box")] = QJsonValue::Null;
        const QJsonObject noPromptConfigured = actor.executeTool(
            QStringLiteral("set_module_params"), QJsonObject{{"instanceId", sam.id}, {"values", noPromptParams}});
        QCOMPARE(noPromptConfigured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        QVERIFY(engine.loadProject(project));
        const int requestsBeforeMissingPromptRun = server.requestLog.size();
        QString missingPromptRunId;
        QVERIFY(engine.runOnceAsync(&missingPromptRunId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(missingPromptRunId).finished, 10000);
        const RunObservation missingPromptRun = engine.runObservation(missingPromptRunId);
        QVERIFY(!missingPromptRun.result.success);
        QCOMPARE(missingPromptRun.result.failedModule, sam.id);
        QVERIFY(missingPromptRun.result.errorMessage.contains(QStringLiteral("points 与 box 至少提供一个 prompt")));
        QCOMPARE(actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{"runId", missingPromptRunId}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("failed"));
        const QJsonObject unavailableSamRegion =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", missingPromptRunId}, {"instanceId", sam.id}, {"port", "region"}});
        QCOMPARE(unavailableSamRegion.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailableSamRegion.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
        QCOMPARE(server.requestLog.size(), requestsBeforeMissingPromptRun);

        const QJsonObject validParamsConfigured = actor.executeTool(
            QStringLiteral("set_module_params"), QJsonObject{{"instanceId", sam.id}, {"values", successfulSamParams}});
        QCOMPARE(validParamsConfigured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        QVERIFY(engine.loadProject(project));
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY2(engine.runObservation(runId).result.success,
                 qPrintable(engine.runObservation(runId).result.errorMessage));

        const ImageData grabOutput = engine.moduleOutput(grab.id);
        const ImageData cropOutput = engine.moduleOutput(crop.id);
        QCOMPARE(grabOutput.width(), 640);
        QCOMPARE(grabOutput.height(), 480);
        QCOMPARE(cropOutput.width(), 400);
        QCOMPARE(cropOutput.height(), 300);
        QCOMPARE(server.lastSetImage.width(), 400);
        QCOMPARE(server.lastSetImage.height(), 300);
        QVERIFY(server.lastSetImageReadable);
        QVERIFY(!server.lastSetImagePath.isEmpty());
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
        QCOMPARE(server.lastPredictBody.value(QStringLiteral("embedding_id")).toString(), QStringLiteral("emb-1"));
        QCOMPARE(server.lastPredictBody.value(QStringLiteral("points_pos")).toArray(),
                 (QJsonArray{QJsonValue(QJsonArray{320, 240})}));
        QCOMPARE(server.lastPredictBody.value(QStringLiteral("points_neg")).toArray(), QJsonArray{});

        const ImageCoordinateProvenance provenance = grabOutput.coordinateProvenance();
        QVERIFY(provenance.isValid());
        QCOMPARE(provenance.sourceSize, QSize(640, 480));
        const ImageCoordinateProvenance cropProvenance = cropOutput.coordinateProvenance();
        QVERIFY(cropProvenance.isValid());
        QCOMPARE(cropProvenance.sourceImageId, provenance.sourceImageId);
        const ImageCoordinateProvenance::Matrix3 expectedCropMatrix{{1, 0, 80, 0, 1, 70, 0, 0, 1}};
        QCOMPARE(cropProvenance.localToSourceMatrix(), expectedCropMatrix);
        const auto expectedSourceBounds = cropProvenance.mapRectToSourceBounds(QRectF(10, 10, 10, 10));
        QVERIFY(expectedSourceBounds);
        QCOMPARE(expectedSourceBounds->x(), 90.0);
        QCOMPARE(expectedSourceBounds->y(), 80.0);
        QCOMPARE(expectedSourceBounds->width(), 10.0);
        QCOMPARE(expectedSourceBounds->height(), 10.0);
        const QVector<QPointF> expectedLocalPoints{QPointF(10, 10), QPointF(20, 10), QPointF(20, 14),
                                                   QPointF(14, 14), QPointF(14, 20), QPointF(10, 20)};
        QJsonArray mappedSourcePoints;
        for (const QPointF& point : expectedLocalPoints) {
            const auto mapped = cropProvenance.mapLocalToSource(point);
            QVERIFY(mapped);
            mappedSourcePoints.append(QJsonArray{mapped->x(), mapped->y()});
        }
        const QJsonArray expectedSourcePoints{QJsonArray{90, 80}, QJsonArray{100, 80}, QJsonArray{100, 84},
                                              QJsonArray{94, 84}, QJsonArray{94, 90},  QJsonArray{90, 90}};
        QCOMPARE(mappedSourcePoints, expectedSourcePoints);
        const int requestCountBeforeQuery = server.requestLog.size();
        QCOMPARE(requestCountBeforeQuery, 2);

        const QJsonObject response =
            actor.executeTool(QStringLiteral("get_module_output"), QJsonObject{{"runId", runId},
                                                                               {"instanceId", sam.id},
                                                                               {"port", "region"},
                                                                               {"polygonOffset", 0},
                                                                               {"polygonLimit", 4}});
        QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        const QJsonObject region = response.value(QStringLiteral("value")).toObject();
        QCOMPARE(region.value(QStringLiteral("label")).toString(), QStringLiteral("concave-L"));
        QCOMPARE(region.value(QStringLiteral("score")).toDouble(), 0.97);
        QCOMPARE(region.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        QCOMPARE(region.value(QStringLiteral("polygonPointCount")).toInt(), 6);
        QCOMPARE(region.value(QStringLiteral("bbox")).toArray(), QJsonArray({10, 10, 10, 10}));
        const QJsonObject source = region.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), provenance.sourceImageId);
        QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
        QCOMPARE(source.value(QStringLiteral("bbox")).toArray(), QJsonArray({90, 80, 10, 10}));
        QCOMPARE(source.value(QStringLiteral("bbox")).toArray(),
                 QJsonArray({expectedSourceBounds->x(), expectedSourceBounds->y(), expectedSourceBounds->width(),
                             expectedSourceBounds->height()}));

        const QJsonObject polygonPage = response.value(QStringLiteral("polygonPage")).toObject();
        const QJsonArray expectedLocal{QJsonArray{10, 10}, QJsonArray{20, 10}, QJsonArray{20, 14},
                                       QJsonArray{14, 14}, QJsonArray{14, 20}, QJsonArray{10, 20}};
        const QJsonArray expectedLocalFirst{expectedLocal.at(0), expectedLocal.at(1), expectedLocal.at(2),
                                            expectedLocal.at(3)};
        const QJsonArray expectedLocalSecond{expectedLocal.at(4), expectedLocal.at(5)};
        const QJsonArray expectedSource{QJsonArray{90, 80}, QJsonArray{100, 80}, QJsonArray{100, 84},
                                        QJsonArray{94, 84}, QJsonArray{94, 90},  QJsonArray{90, 90}};
        const QJsonArray expectedSourceFirst{expectedSource.at(0), expectedSource.at(1), expectedSource.at(2),
                                             expectedSource.at(3)};
        const QJsonArray expectedSourceSecond{expectedSource.at(4), expectedSource.at(5)};
        const QJsonArray mappedSourceFirst{mappedSourcePoints.at(0), mappedSourcePoints.at(1), mappedSourcePoints.at(2),
                                           mappedSourcePoints.at(3)};
        const QJsonArray mappedSourceSecond{mappedSourcePoints.at(4), mappedSourcePoints.at(5)};
        QCOMPARE(polygonPage.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        QCOMPARE(polygonPage.value(QStringLiteral("totalPoints")).toInt(), 6);
        QCOMPARE(polygonPage.value(QStringLiteral("offset")).toInt(), 0);
        QCOMPARE(polygonPage.value(QStringLiteral("limit")).toInt(), 4);
        QVERIFY(polygonPage.value(QStringLiteral("truncated")).toBool());
        QCOMPARE(polygonPage.value(QStringLiteral("points")).toArray(), expectedLocalFirst);
        const QJsonObject sourcePage = polygonPage.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(sourcePage.value(QStringLiteral("sourceImageId")).toString(), provenance.sourceImageId);
        QCOMPARE(sourcePage.value(QStringLiteral("coordinateSystem")).toString(),
                 QStringLiteral("source_image_pixels"));
        QCOMPARE(sourcePage.value(QStringLiteral("points")).toArray(), expectedSourceFirst);
        QCOMPARE(sourcePage.value(QStringLiteral("points")).toArray(), mappedSourceFirst);

        const QJsonObject secondPageResponse =
            actor.executeTool(QStringLiteral("get_module_output"), QJsonObject{{"runId", runId},
                                                                               {"instanceId", sam.id},
                                                                               {"port", "region"},
                                                                               {"polygonOffset", 4},
                                                                               {"polygonLimit", 4}});
        QCOMPARE(secondPageResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        const QJsonObject secondPage = secondPageResponse.value(QStringLiteral("polygonPage")).toObject();
        QCOMPARE(secondPage.value(QStringLiteral("totalPoints")).toInt(), 6);
        QCOMPARE(secondPage.value(QStringLiteral("offset")).toInt(), 4);
        QCOMPARE(secondPage.value(QStringLiteral("limit")).toInt(), 4);
        QVERIFY(!secondPage.value(QStringLiteral("truncated")).toBool());
        QCOMPARE(secondPage.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        QCOMPARE(secondPage.value(QStringLiteral("points")).toArray(), expectedLocalSecond);
        const QJsonObject secondSourcePage = secondPage.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(secondSourcePage.value(QStringLiteral("sourceImageId")).toString(), provenance.sourceImageId);
        QCOMPARE(secondSourcePage.value(QStringLiteral("coordinateSystem")).toString(),
                 QStringLiteral("source_image_pixels"));
        QCOMPARE(secondSourcePage.value(QStringLiteral("points")).toArray(), expectedSourceSecond);
        QCOMPARE(secondSourcePage.value(QStringLiteral("points")).toArray(), mappedSourceSecond);

        const ImageData statsOutput = engine.moduleOutput(stats.id);
        QCOMPARE(statsOutput.data(QStringLiteral("region_area")).toDouble(), 64.0);
        QCOMPARE(statsOutput.data(QStringLiteral("point_count")).toInt(), 6);
        QCOMPARE(statsOutput.data(QStringLiteral("centroid_x")).toDouble(), 13.875);
        QCOMPARE(statsOutput.data(QStringLiteral("centroid_y")).toDouble(), 13.875);
        const QHash<QString, double> expectedStatsValues{
            {QStringLiteral("region_area"), 64.0}, {QStringLiteral("bbox_x"), 10.0},
            {QStringLiteral("bbox_y"), 10.0},      {QStringLiteral("bbox_w"), 10.0},
            {QStringLiteral("bbox_h"), 10.0},      {QStringLiteral("centroid_x"), 13.875},
            {QStringLiteral("centroid_y"), 13.875}};
        const QList<QPair<QString, QString>> pixelFields{{QStringLiteral("region_area"), QStringLiteral("px^2")},
                                                         {QStringLiteral("bbox_x"), QStringLiteral("px")},
                                                         {QStringLiteral("bbox_y"), QStringLiteral("px")},
                                                         {QStringLiteral("bbox_w"), QStringLiteral("px")},
                                                         {QStringLiteral("bbox_h"), QStringLiteral("px")},
                                                         {QStringLiteral("centroid_x"), QStringLiteral("px")},
                                                         {QStringLiteral("centroid_y"), QStringLiteral("px")}};
        for (const auto& field : pixelFields) {
            for (const QString& selector : {QStringLiteral("port"), QStringLiteral("key")}) {
                const QJsonObject query =
                    actor.executeTool(QStringLiteral("get_module_output"),
                                      QJsonObject{{"runId", runId}, {"instanceId", stats.id}, {selector, field.first}});
                QCOMPARE(query.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QCOMPARE(query.value(QStringLiteral("unit")).toString(), field.second);
                QCOMPARE(query.value(QStringLiteral("value")).toDouble(), expectedStatsValues.value(field.first));
                QCOMPARE(query.value(QStringLiteral("value")).toDouble(), statsOutput.data(field.first).toDouble());
            }
        }
        for (const QString& selector : {QStringLiteral("port"), QStringLiteral("key")}) {
            const QJsonObject pointCount = actor.executeTool(
                QStringLiteral("get_module_output"),
                QJsonObject{{"runId", runId}, {"instanceId", stats.id}, {selector, QStringLiteral("point_count")}});
            QCOMPARE(pointCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(pointCount.value(QStringLiteral("value")).toInt(), 6);
            QVERIFY(!pointCount.contains(QStringLiteral("unit")));
        }
        for (const QString& field : {QStringLiteral("score"), QStringLiteral("label")}) {
            for (const QString& selector : {QStringLiteral("port"), QStringLiteral("key")}) {
                const QJsonObject query =
                    actor.executeTool(QStringLiteral("get_module_output"),
                                      QJsonObject{{"runId", runId}, {"instanceId", stats.id}, {selector, field}});
                QCOMPARE(query.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                if (field == QLatin1String("score"))
                    QCOMPARE(query.value(QStringLiteral("value")).toDouble(), 0.97);
                else
                    QCOMPARE(query.value(QStringLiteral("value")).toString(), QStringLiteral("concave-L"));
                QVERIFY(!query.contains(QStringLiteral("unit")));
            }
        }
        const auto verifyUnlabelledStats = [&actor, &runId, &pixelFields](const QString& instanceId) {
            const ImageData output = RunEngine::instance().moduleOutput(instanceId);
            for (const auto& field : pixelFields) {
                for (const QString& selector : {QStringLiteral("port"), QStringLiteral("key")}) {
                    const QJsonObject query = actor.executeTool(
                        QStringLiteral("get_module_output"),
                        QJsonObject{{"runId", runId}, {"instanceId", instanceId}, {selector, field.first}});
                    if (query.value(QStringLiteral("status")).toString() != QLatin1String("available") ||
                        query.contains(QStringLiteral("unit")) ||
                        query.value(QStringLiteral("value")).toDouble() != output.data(field.first).toDouble())
                        return false;
                }
            }
            return true;
        };
        QVERIFY(verifyUnlabelledStats(mismatchedStats.id));
        QVERIFY(verifyUnlabelledStats(missingImageStats.id));
        const QJsonObject centroid = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{"runId", runId}, {"instanceId", stats.id}, {"port", QStringLiteral("centroid")}});
        QCOMPARE(centroid.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(centroid.value(QStringLiteral("type")).toString(), QStringLiteral("Point2D"));
        QCOMPARE(centroid.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        QCOMPARE(centroid.value(QStringLiteral("value")).toArray(), (QJsonArray{13.875, 13.875}));
        const QJsonObject centroidSource = centroid.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(centroidSource.value(QStringLiteral("sourceImageId")).toString(), provenance.sourceImageId);
        QCOMPARE(centroidSource.value(QStringLiteral("coordinateSystem")).toString(),
                 QStringLiteral("source_image_pixels"));
        QCOMPARE(centroidSource.value(QStringLiteral("value")).toArray(), (QJsonArray{93.875, 83.875}));
        const QJsonObject centroidKey = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{"runId", runId}, {"instanceId", stats.id}, {"key", QStringLiteral("centroid")}});
        QCOMPARE(centroidKey.value(QStringLiteral("type")).toString(), QStringLiteral("QPointF"));
        QCOMPARE(centroidKey.value(QStringLiteral("value")).toArray(), (QJsonArray{13.875, 13.875}));
        QVERIFY(!centroidKey.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!centroidKey.contains(QStringLiteral("sourceImageCoordinates")));
        const QJsonObject mismatchedCentroid = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{"runId", runId}, {"instanceId", mismatchedStats.id}, {"port", QStringLiteral("centroid")}});
        QCOMPARE(mismatchedCentroid.value(QStringLiteral("value")).toArray(), (QJsonArray{13.875, 13.875}));
        QVERIFY(!mismatchedCentroid.contains(QStringLiteral("sourceImageCoordinates")));
        for (const QString& parameter :
             {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("maskOffset"),
              QStringLiteral("polygonOffset"), QStringLiteral("polygonLimit")}) {
            QJsonObject args{{"runId", runId}, {"instanceId", stats.id}, {"port", QStringLiteral("centroid")}};
            args.insert(parameter, 0);
            const QJsonObject rejected = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
            QVERIFY(!rejected.contains(QStringLiteral("value")));
        }
        QCOMPARE(server.requestLog.size(), requestCountBeforeQuery);

        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testRegionStatsRejectsRepeatedSingleUpstreamInputs() {
        RunEngine& engine = RunEngine::instance();
        const auto loadGraph = [&engine](bool duplicateImage, bool duplicateRegion) {
            engine.clearModules();
            ProjectManager::instance().closeProject();
            Project* project = ProjectManager::instance().newProject();
            if (!project)
                return false;

            ModuleInstance grab;
            grab.id = QStringLiteral("grab");
            grab.moduleId = QStringLiteral("GrabImage");
            grab.params[QStringLiteral("grabSource")] = QStringLiteral("Path");
            grab.params[QStringLiteral("filePath")] = QStringLiteral(TEST_SAMREGION_DATA_Circle);
            project->addModule(grab);
            for (const QString& id : {QStringLiteral("sam1"), QStringLiteral("sam2")}) {
                ModuleInstance sam;
                sam.id = id;
                sam.moduleId = QStringLiteral("SamPredict");
                sam.params = samParams(QStringLiteral("http://127.0.0.1:1"), id);
                project->addModule(sam);
            }
            ModuleInstance stats;
            stats.id = QStringLiteral("stats");
            stats.moduleId = QStringLiteral("RegionStats");
            project->addModule(stats);

            const auto connectPort = [project](const QString& from, const QString& fromPort, const QString& to,
                                               const QString& toPort) {
                ModuleConnection connection;
                connection.fromModuleId = from;
                connection.fromPort = fromPort;
                connection.toModuleId = to;
                connection.toPort = toPort;
                connection.edgeType = QStringLiteral("data");
                project->addConnection(connection);
            };
            connectPort(QStringLiteral("grab"), QStringLiteral("image"), QStringLiteral("sam1"),
                        QStringLiteral("image"));
            connectPort(QStringLiteral("grab"), QStringLiteral("image"), QStringLiteral("sam2"),
                        QStringLiteral("image"));
            connectPort(QStringLiteral("sam1"), QStringLiteral("region"), QStringLiteral("stats"),
                        QStringLiteral("region"));
            connectPort(QStringLiteral("sam1"), QStringLiteral("image"), QStringLiteral("stats"),
                        QStringLiteral("image"));
            if (duplicateImage)
                connectPort(QStringLiteral("sam2"), QStringLiteral("image"), QStringLiteral("stats"),
                            QStringLiteral("image"));
            if (duplicateRegion)
                connectPort(QStringLiteral("sam2"), QStringLiteral("region"), QStringLiteral("stats"),
                            QStringLiteral("region"));

            ProjectFlow flow;
            flow.id = QStringLiteral("main");
            flow.name = QStringLiteral("main");
            flow.nodeIds =
                QStringList{QStringLiteral("grab"), QStringLiteral("sam1"), QStringLiteral("sam2"), stats.id};
            project->setFlows({flow});
            const bool loaded = engine.loadProject(project);
            engine.clearModules();
            ProjectManager::instance().closeProject();
            return loaded;
        };

        QVERIFY(loadGraph(false, false));
        QVERIFY(!loadGraph(true, false));
        QVERIFY(!loadGraph(false, true));
    }

    void testAgentRegionStatsCentroidUnknownProducerHasNoSourceMapping() {
        ToolSchema::instance().registerDefaultTools();
        AgentActor actor;
        RunEngine& engine = RunEngine::instance();
        const auto runFixture = [&engine](bool withProvenance, bool withRegion, QString& runId) {
            engine.clearModules();
            ProjectManager::instance().closeProject();
            Project* project = ProjectManager::instance().newProject();
            if (!project)
                return false;
            ModuleInstance source;
            source.id = QStringLiteral("source");
            source.moduleId = QStringLiteral("RegionImageFixture");
            project->addModule(source);
            ModuleInstance stats;
            stats.id = QStringLiteral("stats");
            stats.moduleId = QStringLiteral("RegionStats");
            project->addModule(stats);
            for (const auto& ports : {qMakePair(QStringLiteral("image"), QStringLiteral("image")),
                                      qMakePair(QStringLiteral("region"), QStringLiteral("region"))}) {
                ModuleConnection connection;
                connection.fromModuleId = source.id;
                connection.fromPort = ports.first;
                connection.toModuleId = stats.id;
                connection.toPort = ports.second;
                connection.edgeType = QStringLiteral("data");
                project->addConnection(connection);
            }
            ProjectFlow flow;
            flow.id = QStringLiteral("main");
            flow.name = QStringLiteral("main");
            flow.nodeIds = QStringList{source.id, stats.id};
            project->setFlows({flow});
            const bool loaded =
                engine.loadProject(project, [withProvenance, withRegion](const ModuleInstance& instance) {
                    if (instance.moduleId == QLatin1String("RegionImageFixture"))
                        return static_cast<ModuleBase*>(new RegionImageFixtureModule(withProvenance, withRegion));
                    PluginManager& plugins = PluginManager::instance();
                    if (!plugins.isPluginLoaded(instance.moduleId) && !plugins.loadPlugin(instance.moduleId))
                        return static_cast<ModuleBase*>(nullptr);
                    return qobject_cast<ModuleBase*>(plugins.createModule(instance.moduleId));
                });
            return loaded && engine.runOnceAsync(&runId);
        };

        for (bool withProvenance : {false, true}) {
            QString runId;
            QVERIFY(runFixture(withProvenance, true, runId));
            QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
            QVERIFY2(engine.runObservation(runId).result.success,
                     qPrintable(engine.runObservation(runId).result.errorMessage));
            const ImageData output = engine.moduleOutput(QStringLiteral("stats"));
            const QMap<QString, double> expectedStatsValues{
                {QStringLiteral("region_area"), 400.0}, {QStringLiteral("bbox_x"), 120.0},
                {QStringLiteral("bbox_y"), 120.0},      {QStringLiteral("bbox_w"), 20.0},
                {QStringLiteral("bbox_h"), 20.0},       {QStringLiteral("centroid_x"), 130.0},
                {QStringLiteral("centroid_y"), 130.0}};
            for (auto expected = expectedStatsValues.cbegin(); expected != expectedStatsValues.cend(); ++expected)
                QCOMPARE(output.data(expected.key()).toDouble(), expected.value());
            const QList<QPair<QString, QString>> pixelFields{{QStringLiteral("region_area"), QStringLiteral("px^2")},
                                                             {QStringLiteral("bbox_x"), QStringLiteral("px")},
                                                             {QStringLiteral("bbox_y"), QStringLiteral("px")},
                                                             {QStringLiteral("bbox_w"), QStringLiteral("px")},
                                                             {QStringLiteral("bbox_h"), QStringLiteral("px")},
                                                             {QStringLiteral("centroid_x"), QStringLiteral("px")},
                                                             {QStringLiteral("centroid_y"), QStringLiteral("px")}};
            for (const auto& field : pixelFields) {
                for (const QString& selector : {QStringLiteral("port"), QStringLiteral("key")}) {
                    const QJsonObject query = actor.executeTool(QStringLiteral("get_module_output"),
                                                                QJsonObject{{"runId", runId},
                                                                            {"instanceId", QStringLiteral("stats")},
                                                                            {selector, field.first}});
                    QCOMPARE(query.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                    QVERIFY(!query.contains(QStringLiteral("unit")));
                    QCOMPARE(query.value(QStringLiteral("value")).toDouble(), expectedStatsValues.value(field.first));
                }
            }
            const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"),
                                                           QJsonObject{{"runId", runId},
                                                                       {"instanceId", QStringLiteral("stats")},
                                                                       {"port", QStringLiteral("centroid")}});
            QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("Point2D"));
            QCOMPARE(response.value(QStringLiteral("value")).toArray(), (QJsonArray{130, 130}));
            QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        }

        QString missingRegionRunId;
        QVERIFY(runFixture(false, false, missingRegionRunId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(missingRegionRunId).finished, 10000);
        const RunObservation failedRun = engine.runObservation(missingRegionRunId);
        QVERIFY(!failedRun.result.success);
        QCOMPARE(failedRun.result.failedModule, QStringLiteral("stats"));
        QVERIFY(failedRun.result.errorMessage.contains(QStringLiteral("缺少必需输入：区域")));
        QCOMPARE(actor.executeTool(QStringLiteral("get_run_results"), {{"runId", missingRegionRunId}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("failed"));
        const QJsonObject unavailable =
            actor.executeTool(QStringLiteral("get_module_output"), {{"runId", missingRegionRunId},
                                                                    {"instanceId", QStringLiteral("stats")},
                                                                    {"key", QStringLiteral("region_area")}});
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentRegionStatsCentroidRejectsMalformedPoint() {
        ToolSchema::instance().registerDefaultTools();
        AgentActor actor;
        const QList<QVariant> invalidValues{
            QVariant(QVariantList{15.0, 15.0}),
            QVariant::fromValue(QPointF(std::numeric_limits<double>::quiet_NaN(), 15.0))};
        for (const QVariant& invalid : invalidValues) {
            RunEngine& engine = RunEngine::instance();
            engine.clearModules();
            ProjectManager::instance().closeProject();
            Project* project = ProjectManager::instance().newProject();
            QVERIFY(project);
            ModuleInstance stats;
            stats.id = QStringLiteral("stats");
            stats.moduleId = QStringLiteral("RegionStats");
            project->addModule(stats);
            ProjectFlow flow;
            flow.id = QStringLiteral("main");
            flow.name = QStringLiteral("main");
            flow.nodeIds = QStringList{stats.id};
            project->setFlows({flow});
            QVERIFY(engine.loadProject(project, [invalid](const ModuleInstance&) {
                return static_cast<ModuleBase*>(new RegionStatsCentroidFixtureModule(invalid));
            }));
            QString runId;
            QVERIFY(engine.runOnceAsync(&runId));
            QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
            QVERIFY(engine.runObservation(runId).result.success);
            const QJsonObject response = actor.executeTool(
                QStringLiteral("get_module_output"),
                QJsonObject{{"runId", runId}, {"instanceId", stats.id}, {"port", QStringLiteral("centroid")}});
            QCOMPARE(response.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            QVERIFY(!response.contains(QStringLiteral("value")));
        }
        RunEngine::instance().clearModules();
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
