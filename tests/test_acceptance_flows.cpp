#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <cmath>
#include <core/base/ModuleBase.h>
#include <core/display/DisplayData.h>
#include <core/engine/RunEngine.h>
#include <core/manager/PluginManager.h>
#include <core/model/ImageData.h>
#include <core/model/Project.h>
#include <thread>
#include <vector>

using namespace DeepLux;

/// 阶段 0.1 固定验收工程测试。
///
/// 设计约束：
/// - 测试数据固定落在 tests/acceptance/data/，不依赖用户目录临时文件。
/// - 验收工程固定落在 tests/acceptance/projects/，图像路径用 @ACCEPTANCE_DATA@ 占位，
///   测试运行时替换为真实数据目录，保证工程文件可移植。
/// - 预期结果与允许误差落在 tests/acceptance/expected/*.json。
class TestAcceptanceFlows : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void testFindCircleFlow();
    void testPointToPointDistanceFlow();
    void testPointCloudPointToPlaneDistanceFlow();
    void testFitLineFlow();
    // 阶7: 控制流 GUI 验收工程（条件分支执行顺序）
    void testControlFlowIfBranchFlow();

    // 阶段 4: 循环/条件循环/提前退出 流程验收
    void testLoopFixedCountFlow();
    void testLoopFiftyRunsNoCrossFramePollution();
    void testWhileConditionExitFlow();
    void testStopWhileEarlyExitFlow();
    void testStopCancelsLongLoopWithinDeadline();

    // 阶段 4: Parallel all/any/失败分支/blocking 不并行 流程验收
    void testParallelAllJoinFlow();
    void testParallelAnyJoinFlow();
    void testParallelFailureCancelsSiblingFlow();
    void testParallelBlockingBranchesNotParallelizedFlow();

    // 阶段 4: 交互式拾取点集 → 圆拟合 真实工作流
    void testFitCircleFromPickSessionFlow();

private:
    QTemporaryDir m_tempDir;
    QString m_acceptanceRoot; // tests/acceptance 绝对路径

    bool installPlugin(const QString& dirName, const QString& metadataRel, const QString& libName);
    QString loadProjectWithPlaceholder(const QString& projectRel, const QString& dataDir, Project& out);
};

void TestAcceptanceFlows::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", m_tempDir.filePath("appdata").toLocal8Bit());

    // build/bin -> 仓库根/tests/acceptance
    m_acceptanceRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../tests/acceptance");
    QVERIFY2(QFileInfo::exists(m_acceptanceRoot), qPrintable("missing acceptance root: " + m_acceptanceRoot));

    PluginManager::instance().shutdown();
    PluginManager::instance().addPluginPath(m_tempDir.filePath("plugins"));

    QVERIFY2(
        installPlugin("GrabImage", "src/plugins/image_processing/GrabImage/metadata.json", "libGrabImagePlugin.so"),
        "install GrabImage");
    QVERIFY2(installPlugin("FindCircle", "src/plugins/detection/FindCircle/metadata.json", "libFindCirclePlugin.so"),
             "install FindCircle");
    QVERIFY2(installPlugin("MeasurementInput", "src/plugins/geometry/MeasurementInput/metadata.json",
                           "libMeasurementInputPlugin.so"),
             "install MeasurementInput");
    QVERIFY2(installPlugin("DistancePP", "src/plugins/geometry/DistancePP/metadata.json", "libDistancePPPlugin.so"),
             "install DistancePP");
    QVERIFY2(installPlugin("LoadPointCloud", "src/plugins/image_processing/LoadPointCloud/metadata.json",
                           "libLoadPointCloudPlugin.so"),
             "install LoadPointCloud");
    QVERIFY2(installPlugin("PointSurfaceDistance", "src/plugins/geometry/PointSurfaceDistance/metadata.json",
                           "libPointSurfaceDistancePlugin.so"),
             "install PointSurfaceDistance");
    QVERIFY2(installPlugin("FitLine", "src/plugins/geometry/FitLine/metadata.json", "libFitLinePlugin.so"),
             "install FitLine");
    QVERIFY2(installPlugin("If", "src/plugins/logic/If/metadata.json", "libIfPlugin.so"), "install If");
    QVERIFY2(installPlugin("Delay", "src/plugins/logic/Delay/metadata.json", "libDelayPlugin.so"), "install Delay");
    // 阶段 4: 循环/条件循环/停止循环/并行/数学/保存/圆拟合
    QVERIFY2(installPlugin("Loop", "src/plugins/logic/Loop/metadata.json", "libLoopPlugin.so"), "install Loop");
    QVERIFY2(installPlugin("While", "src/plugins/logic/While/metadata.json", "libWhilePlugin.so"), "install While");
    QVERIFY2(installPlugin("StopWhile", "src/plugins/logic/StopWhile/metadata.json", "libStopWhilePlugin.so"),
             "install StopWhile");
    QVERIFY2(installPlugin("Parallel", "src/plugins/logic/Parallel/metadata.json", "libParallelPlugin.so"),
             "install Parallel");
    QVERIFY2(installPlugin("MathPlugin", "src/plugins/variable/MathPlugin/metadata.json", "libMathPlugin.so"),
             "install Math");
    QVERIFY2(installPlugin("SaveData", "src/plugins/system/SaveData/metadata.json", "libSaveDataPlugin.so"),
             "install SaveData");
    QVERIFY2(installPlugin("FitCircle", "src/plugins/geometry/FitCircle/metadata.json", "libFitCirclePlugin.so"),
             "install FitCircle");

    QVERIFY(PluginManager::instance().initialize());
    QVERIFY(PluginManager::instance().loadPlugin("GrabImage"));
    QVERIFY(PluginManager::instance().loadPlugin("FindCircle"));
    QVERIFY(PluginManager::instance().loadPlugin("MeasurementInput"));
    QVERIFY(PluginManager::instance().loadPlugin("DistancePP"));
    QVERIFY(PluginManager::instance().loadPlugin("LoadPointCloud"));
    QVERIFY(PluginManager::instance().loadPlugin("PointSurfaceDistance"));
    QVERIFY(PluginManager::instance().loadPlugin("FitLine"));
    QVERIFY(PluginManager::instance().loadPlugin("FitCircle"));
    QVERIFY(PluginManager::instance().loadPlugin("SaveData"));
    QVERIFY(PluginManager::instance().loadPlugin("Parallel"));
    // 插件索引键为 metadata name（中文）
    QVERIFY(PluginManager::instance().loadPlugin("条件分支"));
    QVERIFY(PluginManager::instance().loadPlugin("延时"));
    QVERIFY(PluginManager::instance().loadPlugin("循环"));
    QVERIFY(PluginManager::instance().loadPlugin("条件循环"));
    QVERIFY(PluginManager::instance().loadPlugin("停止循环"));
    QVERIFY(PluginManager::instance().loadPlugin("数学运算"));
}

void TestAcceptanceFlows::cleanup() {
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
}

bool TestAcceptanceFlows::installPlugin(const QString& dirName, const QString& metadataRel, const QString& libName) {
    const QString repoRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    const QString pluginDir = m_tempDir.filePath("plugins/" + dirName);
    if (!QDir().mkpath(pluginDir))
        return false;

    const QString metaSrc = QDir(repoRoot).filePath(metadataRel);
    const QString libSrc = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../lib/" + libName);
    if (!QFileInfo::exists(metaSrc) || !QFileInfo::exists(libSrc))
        return false;

    QFile::remove(pluginDir + "/metadata.json");
    QFile::remove(pluginDir + "/" + libName);
    return QFile::copy(metaSrc, pluginDir + "/metadata.json") && QFile::copy(libSrc, pluginDir + "/" + libName);
}

QString TestAcceptanceFlows::loadProjectWithPlaceholder(const QString& projectRel, const QString& dataDir,
                                                        Project& out) {
    QFile f(QDir(m_acceptanceRoot).filePath(projectRel));
    if (!f.open(QIODevice::ReadOnly))
        return QString("cannot open project: %1").arg(f.fileName());
    QString text = QString::fromUtf8(f.readAll());
    f.close();
    text.replace(QStringLiteral("@ACCEPTANCE_DATA@"), dataDir);

    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return QString("invalid project json: %1").arg(err.errorString());
    out.fromJson(doc.object());
    return QString();
}

void TestAcceptanceFlows::testFindCircleFlow() {
    const QString dataDir = QDir(m_acceptanceRoot).filePath("data");

    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_findcircle.json", dataDir, project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject into RunEngine failed");

    engine.runOnce();

    const ImageData out = engine.moduleOutput(QStringLiteral("findcircle"));
    QVERIFY2(out.isValid(), "FindCircle produced no output");

    // 读取预期结果与允许误差
    QFile ef(QDir(m_acceptanceRoot).filePath("expected/circle_640x480.json"));
    QVERIFY(ef.open(QIODevice::ReadOnly));
    QJsonObject expected = QJsonDocument::fromJson(ef.readAll()).object();
    ef.close();

    const double expCx = expected["circle_center_x"].toDouble();
    const double expCy = expected["circle_center_y"].toDouble();
    const double expR = expected["circle_radius"].toDouble();
    const double tolC = expected["tolerance_center_px"].toDouble();
    const double tolR = expected["tolerance_radius_px"].toDouble();

    const double gotCx = out.data("circle_center_x").toDouble();
    const double gotCy = out.data("circle_center_y").toDouble();
    const double gotR = out.data("circle_radius").toDouble();

    QVERIFY2(std::abs(gotCx - expCx) <= tolC,
             qPrintable(QString("circle_center_x off: got %1 want %2 ±%3").arg(gotCx).arg(expCx).arg(tolC)));
    QVERIFY2(std::abs(gotCy - expCy) <= tolC,
             qPrintable(QString("circle_center_y off: got %1 want %2 ±%3").arg(gotCy).arg(expCy).arg(tolC)));
    QVERIFY2(std::abs(gotR - expR) <= tolR,
             qPrintable(QString("circle_radius off: got %1 want %2 ±%3").arg(gotR).arg(expR).arg(tolR)));

    // 步4: 强类型 Circle2D 输出应存在且与标量值一致
    QVERIFY2(out.data("circle").canConvert<Circle2D>(), "FindCircle must emit typed Circle2D");
    const Circle2D circle = out.data("circle").value<Circle2D>();
    QVERIFY(circle.isValid());
    QVERIFY2(std::abs(circle.centerX - expCx) <= tolC, "Circle2D.centerX mismatch");
    QVERIFY2(std::abs(circle.radius - expR) <= tolR, "Circle2D.radius mismatch");
}

void TestAcceptanceFlows::testPointToPointDistanceFlow() {
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_distancepp.json", QString(), project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load point-to-point project");
    engine.runOnce();

    const ImageData out = engine.moduleOutput(QStringLiteral("distancepp"));
    QVERIFY2(out.data("distance").isValid(), "DistancePP produced no distance output");

    QFile expectedFile(QDir(m_acceptanceRoot).filePath("expected/two_points_640x480.json"));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();
    const double actual = out.data("distance").toDouble();
    QVERIFY2(std::abs(actual - expected["distance"].toDouble()) <= expected["tolerance_distance_px"].toDouble(),
             qPrintable(QString("distance off: got %1").arg(actual)));
}

void TestAcceptanceFlows::testPointCloudPointToPlaneDistanceFlow() {
    const QString dataDir = QDir(m_acceptanceRoot).filePath("data");
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_point_surface.json", dataDir, project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load point-cloud measurement project");
    engine.runOnce();

    const ImageData loaded = engine.moduleOutput(QStringLiteral("loadcloud"));
    QVERIFY2(loaded.data("point_count").toInt() == 273, "point cloud vertex count mismatch");
    const ImageData out = engine.moduleOutput(QStringLiteral("pointsurface"));
    QVERIFY2(out.data("distance").isValid(), "PointSurfaceDistance produced no distance output");

    QFile expectedFile(QDir(m_acceptanceRoot).filePath("expected/plane_z5.json"));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();
    const double actual = out.data("distance").toDouble();
    QVERIFY2(std::abs(actual - expected["point_surface_distance"].toDouble()) <=
                 expected["tolerance_distance"].toDouble(),
             qPrintable(QString("point-surface distance off: got %1").arg(actual)));
}

void TestAcceptanceFlows::testFitLineFlow() {
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_fitline.json", QString(), project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load fit-line project");
    engine.runOnce();

    const ImageData out = engine.moduleOutput(QStringLiteral("fitline"));
    QVERIFY2(out.data("line_error").isValid(), "FitLine produced no line_error output");

    QFile expectedFile(QDir(m_acceptanceRoot).filePath("expected/fitline_points.json"));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();

    // 共线点 LS 拟合，误差应接近 0
    const double fitError = out.data("line_error").toDouble();
    QVERIFY2(fitError <= expected["tolerance_fit_error"].toDouble(),
             qPrintable(QString("fit error too large: got %1").arg(fitError)));

    // 拟合直线应经过已知线段中点 (320,240) 附近：用拟合直线方程验证
    // line_row/col 为拟合线上两点，验证中点到拟合线距离很小
    const double row1 = out.data("line_row1").toDouble();
    const double col1 = out.data("line_col1").toDouble();
    const double row2 = out.data("line_row2").toDouble();
    const double col2 = out.data("line_col2").toDouble();
    // 拟合线方向 (col2-col1, row2-row1)，中点 (320,240) 到线的距离
    const double dx = col2 - col1;
    const double dy = row2 - row1;
    const double len = std::sqrt(dx * dx + dy * dy);
    QVERIFY(len > 1e-6);
    // 点 (x=320, y=240)；线过 (col1, row1)，方向 (dx,dy)
    const double distToLine = std::abs(dy * 320.0 - dx * 240.0 + (dx * row1 - dy * col1)) / len;
    QVERIFY2(distToLine <= expected["tolerance_endpoint_px"].toDouble(),
             qPrintable(QString("fitted line far from known midpoint: %1").arg(distToLine)));
}

void TestAcceptanceFlows::testControlFlowIfBranchFlow() {
    // 阶7: 控制流验收工程——If(1>0) 为真，真分支执行、假分支跳过
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_controlflow.json", QString(), project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load control-flow project");

    QStringList finished;
    QStringList skipped;
    QMetaObject::Connection fConn =
        connect(&engine, &RunEngine::moduleFinished, [&](const QString& id, bool, int) { finished.append(id); });
    QMetaObject::Connection sConn =
        connect(&engine, &RunEngine::moduleSkipped, [&](const QString& id) { skipped.append(id); });
    engine.runOnce();
    disconnect(fConn);
    disconnect(sConn);

    QVERIFY2(finished.contains("cond"), "condition must run");
    QVERIFY2(finished.contains("truebranch"), "true branch must run for 1>0");
    QVERIFY2(!finished.contains("falsebranch"), "false branch must not run for 1>0");
    QVERIFY2(skipped.contains("falsebranch"), "false branch must be reported skipped");
}

// ---------------------------------------------------------------------------
// 阶段 4: Loop / While / StopWhile 流程验收
// ---------------------------------------------------------------------------

namespace {
// 记录每个节点 finished 次数与顺序（同步运行 → 直连即可）
struct FlowTrace {
    QStringList finishedOrder;
    QHash<QString, int> finishedCount;
    QStringList skipped;
    bool runSuccess = false;
    int runElapsedMs = -1;

    void connectTo(RunEngine& engine, std::vector<QMetaObject::Connection>& conns) {
        conns.push_back(QObject::connect(&engine, &RunEngine::moduleFinished, [this](const QString& id, bool, int) {
            finishedOrder.append(id);
            finishedCount[id]++;
        }));
        conns.push_back(
            QObject::connect(&engine, &RunEngine::moduleSkipped, [this](const QString& id) { skipped.append(id); }));
        conns.push_back(QObject::connect(&engine, &RunEngine::runFinished, [this](const RunResult& r) {
            runSuccess = r.success;
            runElapsedMs = r.elapsedMs;
        }));
    }
    void disconnectAll(std::vector<QMetaObject::Connection>& conns) {
        for (const auto& c : conns)
            QObject::disconnect(c);
        conns.clear();
    }
};
} // namespace

void TestAcceptanceFlows::testLoopFixedCountFlow() {
    // Loop(3) → body 恰执行 3 次 → done → after；执行顺序完全确定
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_loop.json", QString(), project).isEmpty(),
             "load accept_loop.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject loop flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "loop flow must succeed");
    QCOMPARE(trace.finishedCount.value("body"), 3);
    QCOMPARE(trace.finishedCount.value("after"), 1);
    QCOMPARE(trace.finishedOrder, QStringList({"loop", "body", "loop", "body", "loop", "body", "loop", "after"}));
}

void TestAcceptanceFlows::testLoopFiftyRunsNoCrossFramePollution() {
    // 同一工程重复运行 50 次：每次循环体都恰执行 3 次，无跨帧污染
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_loop.json", QString(), project).isEmpty(),
             "load accept_loop.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject loop flow");

    for (int run = 0; run < 50; ++run) {
        FlowTrace trace;
        std::vector<QMetaObject::Connection> conns;
        trace.connectTo(engine, conns);
        engine.runOnce();
        trace.disconnectAll(conns);
        QVERIFY2(trace.runSuccess, qPrintable(QString("run %1 must succeed").arg(run)));
        QCOMPARE(trace.finishedCount.value("body"), 3);
        QCOMPARE(trace.finishedCount.value("after"), 1);
    }
}

void TestAcceptanceFlows::testWhileConditionExitFlow() {
    // While(counter < 3)：种子写 counter=0，循环体每次 +1，
    // 条件在第 4 次求值时为假退出 —— 由数据驱动而非固定次数
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_while.json", QString(), project).isEmpty(),
             "load accept_while.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject while flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "while flow must succeed");
    QCOMPARE(trace.finishedCount.value("seed"), 1);
    QCOMPARE(trace.finishedCount.value("body"), 3); // 条件退出，而非 maxIterations
    QCOMPARE(trace.finishedCount.value("after"), 1);

    // 循环体最后一次输出 counter=3（数据载体无图像，按键存在性断言）
    const ImageData bodyOut = engine.moduleOutput(QStringLiteral("body"));
    QVERIFY2(bodyOut.hasData("counter"), "while body must produce counter output");
    QCOMPARE(bodyOut.data("counter").toDouble(), 3.0);
}

void TestAcceptanceFlows::testStopWhileEarlyExitFlow() {
    // While(恒真, maxIterations=1000) 内 StopWhile 首次迭代即提前退出
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_stopwhile.json", QString(), project).isEmpty(),
             "load accept_stopwhile.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject stopwhile flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "stopwhile flow must succeed");
    QCOMPARE(trace.finishedCount.value("body"), 1); // 提前退出：仅 1 次而非 1000 次
    QCOMPARE(trace.finishedCount.value("stopMod"), 1);
    QCOMPARE(trace.finishedCount.value("after"), 1); // stop 边触发一次，不重复
}

void TestAcceptanceFlows::testStopCancelsLongLoopWithinDeadline() {
    // 停止/取消时限可断言：恒真长循环（10000 次 × 30ms）运行中请求取消，
    // 必须在 500ms 内停下，且远未跑满全部迭代
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_stop_cancel.json", QString(), project).isEmpty(),
             "load accept_stop_cancel.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject stop-cancel flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);

    std::thread runner([&engine]() { engine.runOnce(); });

    QTest::qWait(250); // 让循环跑几轮
    QElapsedTimer stopWatch;
    stopWatch.start();
    engine.requestCancellation();

    // 等待 runOnce 返回（轮询执行状态，上限 3s 防测试自身挂死）
    while (engine.isBusy() && stopWatch.elapsed() < 3000)
        QTest::qWait(10);
    const qint64 stopLatencyMs = stopWatch.elapsed();

    runner.join();
    trace.disconnectAll(conns);

    QVERIFY2(!engine.isBusy(), "engine must stop after cancellation");
    QVERIFY2(!trace.runSuccess, "cancelled run must not report success");
    QVERIFY2(stopLatencyMs < 500, qPrintable(QString("stop latency %1ms exceeds 500ms deadline").arg(stopLatencyMs)));
    QVERIFY2(trace.finishedCount.value("body") < 100,
             qPrintable(QString("body ran %1 times; cancellation ineffective").arg(trace.finishedCount.value("body"))));
    QCOMPARE(trace.finishedCount.value("after"), 0); // 取消后不应走到 done 分支
}

// ---------------------------------------------------------------------------
// 阶段 4: Parallel all / any / 失败分支 / blocking 不并行 流程验收
// ---------------------------------------------------------------------------

void TestAcceptanceFlows::testParallelAllJoinFlow() {
    // Parallel 分叉两个 60ms 线程安全分支 → all 汇合 → after。
    // 断言：真实并发（最大并发度≥2）、汇合在两个分支都完成后、各节点恰一次。
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_all.json", QString(), project).isEmpty(),
             "load accept_parallel_all.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject parallel-all flow");

    FlowTrace trace;
    QElapsedTimer clock;
    QHash<QString, qint64> startedAt;
    QHash<QString, qint64> finishedAt;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    conns.push_back(QObject::connect(&engine, &RunEngine::moduleStarted,
                                     [&](const QString& id) { startedAt[id] = clock.elapsed(); }));
    conns.push_back(QObject::connect(&engine, &RunEngine::moduleFinished,
                                     [&](const QString& id, bool, int) { finishedAt[id] = clock.elapsed(); }));

    clock.start();
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "parallel-all flow must succeed");
    QCOMPARE(trace.finishedCount.value("b1"), 1);
    QCOMPARE(trace.finishedCount.value("b2"), 1);
    QCOMPARE(trace.finishedCount.value("merge"), 1);
    QCOMPARE(trace.finishedCount.value("after"), 1);
    QVERIFY2(engine.lastParallelMaxConcurrency() >= 2,
             qPrintable(QString("expected real concurrency >=2, got %1").arg(engine.lastParallelMaxConcurrency())));
    // all 汇合：merge 必须晚于两个分支完成
    QVERIFY2(startedAt.value("merge") >= finishedAt.value("b1"), "merge started before b1 finished");
    QVERIFY2(startedAt.value("merge") >= finishedAt.value("b2"), "merge started before b2 finished");
}

void TestAcceptanceFlows::testParallelAnyJoinFlow() {
    // any 汇合：真分支触发一条控制边即可激活汇合点；假分支被跳过、
    // 其控制边永不触发，但 any 策略下汇合点仍执行且仅执行一次。
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_any.json", QString(), project).isEmpty(),
             "load accept_parallel_any.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject parallel-any flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "any-join flow must succeed");
    QCOMPARE(trace.finishedCount.value("branchA"), 1);
    QCOMPARE(trace.finishedCount.value("branchB"), 0);
    QVERIFY2(trace.skipped.contains("branchB"), "false branch must be skipped");
    // any 策略：只有一条边触发也应激活汇合点，且仅一次
    QCOMPARE(trace.finishedCount.value("merge"), 1);
}

void TestAcceptanceFlows::testParallelFailureCancelsSiblingFlow() {
    // 失败分支：快速失败分支取消同组 500ms 慢分支，
    // 总耗时必须远小于慢分支时长（取消时限可断言），汇合点不执行。
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_failure.json", QString(), project).isEmpty(),
             "load accept_parallel_failure.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject parallel-failure flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(!trace.runSuccess, "run with failed branch must fail");
    QVERIFY2(trace.finishedCount.value("bad") == 1, "failed branch must have executed");
    QVERIFY2(trace.runElapsedMs < 400,
             qPrintable(QString("sibling cancellation too slow: %1ms").arg(trace.runElapsedMs)));
    QCOMPARE(trace.finishedCount.value("merge"), 0); // 失败分支不触发后继
    QCOMPARE(trace.finishedCount.value("after"), 0);
}

void TestAcceptanceFlows::testParallelBlockingBranchesNotParallelizedFlow() {
    // blocking 模块（SaveData，文件 I/O）即使声明并发数 2 也不得并行：
    // 两个分支执行区间不得重叠，且最大并发度 ≤1。
    const QString file1 = QStringLiteral("accept_parallel_blocking_1.json");
    const QString file2 = QStringLiteral("accept_parallel_blocking_2.json");
    QFile::remove(file1);
    QFile::remove(file2);

    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_blocking.json", QString(), project).isEmpty(),
             "load accept_parallel_blocking.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject parallel-blocking flow");

    FlowTrace trace;
    QElapsedTimer clock;
    QHash<QString, qint64> startedAt;
    QHash<QString, qint64> finishedAt;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    conns.push_back(QObject::connect(&engine, &RunEngine::moduleStarted,
                                     [&](const QString& id) { startedAt[id] = clock.elapsed(); }));
    conns.push_back(QObject::connect(&engine, &RunEngine::moduleFinished,
                                     [&](const QString& id, bool, int) { finishedAt[id] = clock.elapsed(); }));

    clock.start();
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "blocking-branch flow must succeed");
    QCOMPARE(trace.finishedCount.value("s1"), 1);
    QCOMPARE(trace.finishedCount.value("s2"), 1);
    QCOMPARE(trace.finishedCount.value("merge"), 1);
    QVERIFY2(QFile::exists(file1), "blocking branch s1 must have written its file");
    QVERIFY2(QFile::exists(file2), "blocking branch s2 must have written its file");

    // 执行区间不得重叠（串行）
    QVERIFY2(startedAt.contains("s1") && startedAt.contains("s2"), "both blocking branches must run");
    const bool s2AfterS1 = startedAt.value("s2") >= finishedAt.value("s1");
    const bool s1AfterS2 = startedAt.value("s1") >= finishedAt.value("s2");
    QVERIFY2(s2AfterS1 || s1AfterS2, "blocking branches must not overlap in time");
    QVERIFY2(engine.lastParallelMaxConcurrency() <= 1,
             qPrintable(QString("blocking modules must not be parallelized, concurrency=%1")
                            .arg(engine.lastParallelMaxConcurrency())));

    QFile::remove(file1);
    QFile::remove(file2);
}

// ---------------------------------------------------------------------------
// 阶段 4: 交互式拾取点集 → 圆拟合 真实工作流
// ---------------------------------------------------------------------------

void TestAcceptanceFlows::testFitCircleFromPickSessionFlow() {
    // 工程内 MeasurementInput 点集为空（拾取前状态）：
    // 1) 未完成拾取时运行必须失败（拾取门控）；
    // 2) 通过与 UI 拾取相同的参数写入路径逐点提交 16 个圆周采样点，
    //    之后流程成功，拟合结果与已知圆一致。
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_fitcircle_pick.json", QString(), project).isEmpty(),
             "load accept_fitcircle_pick.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject fitcircle-pick flow");

    // 1) 拾取门控：空点集运行必须失败
    {
        FlowTrace trace;
        std::vector<QMetaObject::Connection> conns;
        trace.connectTo(engine, conns);
        engine.runOnce();
        trace.disconnectAll(conns);
        QVERIFY2(!trace.runSuccess, "run before picking must fail (pick gate)");
    }

    // 2) 模拟一次拾取会话：逐点累积写入 points（与 MainWindow 拾取写参同路径）
    ModuleBase* pick = engine.getModule(QStringLiteral("pickinput"));
    QVERIFY2(pick != nullptr, "pick input module must exist");

    QFile ef(QDir(m_acceptanceRoot).filePath("expected/fitcircle_pick.json"));
    QVERIFY(ef.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(ef.readAll()).object();
    ef.close();
    const double cx = expected["circle_center_x"].toDouble();
    const double cy = expected["circle_center_y"].toDouble();
    const double r = expected["circle_radius"].toDouble();
    const int pickCount = expected["pick_count"].toInt();

    QJsonArray picked;
    for (int k = 0; k < pickCount; ++k) {
        const double theta = 2.0 * M_PI * k / pickCount;
        picked.append(QJsonArray{cx + r * std::cos(theta), cy + r * std::sin(theta)});
        pick->setParam("points", picked);
    }
    QCOMPARE(picked.size(), pickCount);

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "fit flow must succeed after pick session");
    const ImageData out = engine.moduleOutput(QStringLiteral("fitcircle"));
    QVERIFY2(out.hasData("circle_center_x") && out.hasData("circle_radius"), "FitCircle produced no fit output");

    const double tolC = expected["tolerance_center_px"].toDouble();
    const double tolR = expected["tolerance_radius_px"].toDouble();
    const double tolE = expected["tolerance_fit_error"].toDouble();
    const double gotCx = out.data("circle_center_x").toDouble();
    const double gotCy = out.data("circle_center_y").toDouble();
    const double gotR = out.data("circle_radius").toDouble();
    const double gotE = out.data("circle_error").toDouble();
    QVERIFY2(std::abs(gotCx - cx) <= tolC, qPrintable(QString("center_x off: %1").arg(gotCx)));
    QVERIFY2(std::abs(gotCy - cy) <= tolC, qPrintable(QString("center_y off: %1").arg(gotCy)));
    QVERIFY2(std::abs(gotR - r) <= tolR, qPrintable(QString("radius off: %1").arg(gotR)));
    QVERIFY2(gotE <= tolE, qPrintable(QString("fit error too large: %1").arg(gotE)));
}

QTEST_MAIN(TestAcceptanceFlows)
#include "test_acceptance_flows.moc"
