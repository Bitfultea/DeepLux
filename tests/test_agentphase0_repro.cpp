// 阶段 0 基线特征化测试（docs/agent-capability-improvement-plan.md §2/§5.5）。
//
// 本文件断言的是 F01–F13 的【当前缺陷行为】，为独立审核者提供可重复的最小复现
// （G0：至少 F02、F06、F08 可按记录重现）。这些断言不是行为规范：对应收口阶段
// （见每个用例注释）落地修复时，必须同步翻转/更新断言，禁止直接删除用例掩盖问题。
#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/base/ModuleBase.h"
#include "core/common/UserManager.h"
#include "core/display/DisplayData.h"
#include "core/engine/RunEngine.h"
#include "core/interface/IModule.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/AnnotationMaskOps.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <algorithm>
#include <thread>

using namespace DeepLux;

namespace {

// F08 复现用模拟 LLM：仅产出响应，不触碰 Project/Engine（与 test_agentcontroller 同模式）
class MockLLMClient : public ILLMClient {
    Q_OBJECT
public:
    explicit MockLLMClient(QObject* parent = nullptr) : ILLMClient(parent) {}
    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}

    AgentConversation lastCtx;
    QList<ToolDefinition> lastTools;
    int requestCount = 0;
    void sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>& tools) override {
        lastCtx = ctx;
        lastTools = tools;
        ++requestCount;
    }

    void emitResponse(const AgentResponse& resp) {
        AgentResponse correlated = resp;
        if (correlated.requestId.isEmpty())
            correlated.requestId = lastCtx.requestId;
        emit responseReceived(correlated);
        QCoreApplication::processEvents();
    }
};

class G3OutputModule : public ModuleBase {
public:
    G3OutputModule() {
        m_moduleId = QStringLiteral("com.deeplux.test.g3output");
        m_name = QStringLiteral("G3Output");
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(64, 32, 1);
        output.setData(QStringLiteral("height_invalid_value"), -999.0);
        QVariantList table;
        for (int i = 0; i < 205; ++i)
            table.append(QVariantMap{{QStringLiteral("row"), i}, {QStringLiteral("value"), i * 2.0}});
        output.setData(QStringLiteral("table"), table);
        output.setData(QStringLiteral("bigtable"),
                       QVariantList{QString(50000, QLatin1Char('x')), QString(50000, QLatin1Char('y'))});
        output.setData(QStringLiteral("tooBigRow"), QVariantList{QString(70000, QLatin1Char('z'))});
        output.setData(QStringLiteral("oversize"), QString(70000, QLatin1Char('x')));
        output.setData(QStringLiteral("unsupported"), QDateTime::currentDateTime());
        output.setData(QStringLiteral("nestedUnsupported"), QVariantList{QDateTime::currentDateTime()});
        PointCloudData cloud;
        cloud.points.emplace_back(1.0, 2.0, 3.0);
        cloud.points.emplace_back(4.0, 5.0, 6.0);
        output.setData(QStringLiteral("cloud"), QVariant::fromValue(cloud));
        QImage mask(64, 32, QImage::Format_Grayscale8);
        mask.fill(255);
        Region2D region;
        region.bbox = QRectF(0, 0, 64, 32);
        region.polygon = {QPointF(0, 0), QPointF(63, 0), QPointF(63, 31), QPointF(0, 31)};
        region.maskRle = MaskOps::encodeRle(mask);
        output.setData(QStringLiteral("region"), QVariant::fromValue(region));
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class G3SlowCycleModule : public G3OutputModule {
protected:
    bool process(const ImageData& input, ImageData& output) override {
        QThread::msleep(500);
        return G3OutputModule::process(input, output);
    }
};

class G3LargePayloadModule : public G3OutputModule {
protected:
    bool process(const ImageData& input, ImageData& output) override {
        G3OutputModule::process(input, output);
        Region2D largeRegion;
        largeRegion.bbox = QRectF(0, 0, 64, 32);
        QImage mask(64, 32, QImage::Format_Grayscale8);
        mask.fill(255);
        largeRegion.maskRle = MaskOps::encodeRle(mask) + QString(1024 * 1024, QLatin1Char(' '));
        output.setData(QStringLiteral("largeRegion"), QVariant::fromValue(largeRegion));
        PointCloudData largeCloud;
        largeCloud.points.resize(100000, Eigen::Vector3d(1.0, 2.0, 3.0));
        output.setData(QStringLiteral("largeCloud"), QVariant::fromValue(largeCloud));
        return true;
    }
};

class G3CollisionModule : public ModuleBase {
public:
    G3CollisionModule() {
        m_moduleId = QStringLiteral("com.deeplux.test.g3collision");
        m_name = QStringLiteral("G3Collision");
    }
    ExecutionResult execute(const PortValueMap&, PortValueMap& outputs, ExecutionContext&) override {
        ImageData image;
        image.setData(QStringLiteral("collision"), 11);
        outputs.insert(QStringLiteral("image"), QVariant::fromValue(image));
        outputs.insert(QStringLiteral("collision"), 99);
        return ExecutionResult::ok();
    }

protected:
    bool process(const ImageData&, ImageData&) override {
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class G3FailureModule : public ModuleBase {
public:
    G3FailureModule() {
        m_moduleId = QStringLiteral("com.deeplux.test.g3failure");
        m_name = QStringLiteral("G3Failure");
    }

protected:
    bool process(const ImageData&, ImageData&) override {
        emit errorOccurred(QStringLiteral("service timeout after 500 ms"));
        return false;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class G3BadPortModule : public G3OutputModule {
public:
    G3BadPortModule() {
        PortSpec bad;
        bad.id = QStringLiteral("value");
        bad.type = DataType::Number;
        setPorts({}, {bad});
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        G3OutputModule::process(input, output);
        output.setData(QStringLiteral("value"), QStringLiteral("not a number"));
        return true;
    }
};

class G3MissingInputModule : public G3OutputModule {
public:
    G3MissingInputModule() {
        PortSpec required;
        required.id = QStringLiteral("point");
        required.type = DataType::Point2D;
        required.required = true;
        setPorts({required}, {});
    }
};

QJsonObject makeToolCallJson(const QString& name, const QJsonObject& args) {
    return QJsonObject{{"id", QStringLiteral("repro_%1").arg(name)},
                       {"type", QStringLiteral("function")},
                       {"name", name},
                       {"arguments", args}};
}

} // namespace

class TestAgentPhase0Repro : public QObject {
    Q_OBJECT

    QTemporaryDir m_appDataDir;

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

    // 插件按 metadata "name" 注册（如 Delay→「延时」），从注入路径读取避免硬编码
    static QString metaName(const char* metaPathMacro) {
        QFile f(QString::fromUtf8(metaPathMacro));
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(f.readAll()).object().value("name").toString();
    }

    QString m_nameGrabImage, m_nameDelay, m_nameMeasurementInput, m_nameFitCircle, m_nameNPointCalibration,
        m_nameFolder, m_namePlcWrite;

    static QString waitForRunIdle(AgentActor& actor, int timeoutMs = 8000) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            const QJsonObject r = actor.executeTool("get_run_results", QJsonObject());
            if (!r.value("isRunning").toBool())
                return QString();
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        return QStringLiteral("run did not finish within %1 ms").arg(timeoutMs);
    }

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        ToolSchema::instance().registerDefaultTools();
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(m_appDataDir.path()).filePath("plugins");
        QVERIFY(installPlugin(pluginRoot, "GrabImage", QStringLiteral(TEST_REPRO_META_GrabImage),
                              QStringLiteral(TEST_REPRO_LIB_GrabImage)));
        QVERIFY(installPlugin(pluginRoot, "Delay", QStringLiteral(TEST_REPRO_META_Delay),
                              QStringLiteral(TEST_REPRO_LIB_Delay)));
        QVERIFY(installPlugin(pluginRoot, "MeasurementInput", QStringLiteral(TEST_REPRO_META_MeasurementInput),
                              QStringLiteral(TEST_REPRO_LIB_MeasurementInput)));
        QVERIFY(installPlugin(pluginRoot, "FitCircle", QStringLiteral(TEST_REPRO_META_FitCircle),
                              QStringLiteral(TEST_REPRO_LIB_FitCircle)));
        QVERIFY(installPlugin(pluginRoot, "NPointCalibration", QStringLiteral(TEST_REPRO_META_NPointCalibration),
                              QStringLiteral(TEST_REPRO_LIB_NPointCalibration)));
        QVERIFY(installPlugin(pluginRoot, "Folder", QStringLiteral(TEST_REPRO_META_Folder),
                              QStringLiteral(TEST_REPRO_LIB_Folder)));
        QVERIFY(installPlugin(pluginRoot, "PLCWrite", QStringLiteral(TEST_REPRO_META_PLCWrite),
                              QStringLiteral(TEST_REPRO_LIB_PLCWrite)));
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        m_nameGrabImage = metaName(TEST_REPRO_META_GrabImage);
        m_nameDelay = metaName(TEST_REPRO_META_Delay);
        m_nameMeasurementInput = metaName(TEST_REPRO_META_MeasurementInput);
        m_nameFitCircle = metaName(TEST_REPRO_META_FitCircle);
        m_nameNPointCalibration = metaName(TEST_REPRO_META_NPointCalibration);
        m_nameFolder = metaName(TEST_REPRO_META_Folder);
        m_namePlcWrite = metaName(TEST_REPRO_META_PLCWrite);
        for (const QString& name : {m_nameGrabImage, m_nameDelay, m_nameMeasurementInput, m_nameFitCircle,
                                    m_nameNPointCalibration, m_nameFolder, m_namePlcWrite}) {
            QVERIFY2(PluginManager::instance().loadPlugin(name), qPrintable(name));
        }
        QVERIFY(AgentController::instance().initialize());
    }

    void cleanupTestCase() {
        AgentController::instance().shutdown();
        PluginManager::instance().shutdown();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void init() {
        RunEngine::instance().stop();
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void cleanup() {
        RunEngine::instance().stop();
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    // F01（阶段 3 修复——翻转）：get_run_results 不再仅返回累计统计——
    // 含 lastRunId/status/stale;模块业务输出经 get_module_output 查询
    void testF01FixedGetRunResultsReturnsRunIdentity() {
        AgentActor actor;
        const QJsonObject r = actor.executeTool("get_run_results", QJsonObject());
        // 累计统计字段仍保留（兼容）
        for (const char* k : {"totalRuns", "successRuns", "failedRuns", "lastElapsedMs", "isRunning"})
            QVERIFY2(r.contains(k), k);
        // 阶段 3 新增：运行身份与状态
        QVERIFY2(r.contains("lastRunId"), "phase-3 fix: must carry lastRunId");
        QVERIFY2(r.contains("status"), "phase-3 fix: must carry status");
        QVERIFY2(r.contains("stale"), "phase-3 fix: must carry stale flag");
        // 模块业务输出不经 get_run_results（由 get_module_output 查询）
        QVERIFY(!r.contains("outputs"));
        QVERIFY(!r.contains("moduleResults"));
    }

    // F11（阶段 3 分段收口）：启动、运行中查询、完成查询均绑定同一 runId。
    void testF11AsyncRunHasNoRunIdentity() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_async"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameGrabImage}, {"instanceName", "grab"}})
                     .contains("error"));
        QVERIFY(!actor
                     .executeTool("set_param",
                                  QJsonObject{{"instanceId", "grab"}, {"key", "grabSource"}, {"value", "Demo"}})
                     .contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameDelay}, {"instanceName", "wait"}})
                     .contains("error"));
        QVERIFY(
            !actor.executeTool("set_param", QJsonObject{{"instanceId", "wait"}, {"key", "delayMs"}, {"value", "400"}})
                 .contains("error"));

        QSignalSpy finishedSpy(&RunEngine::instance(), &RunEngine::runFinished);
        const QJsonObject run = actor.executeTool("run_flow", QJsonObject{{"wait", false}});
        QVERIFY2(!run.contains("error"), qPrintable(run.value("error").toString()));
        QCOMPARE(run.value("status").toString(), QStringLiteral("started"));
        QVERIFY(run.value("async").toBool());
        const QString runId = run.value("runId").toString();
        QVERIFY(!runId.isEmpty());

        const QJsonObject immediate = actor.executeTool("get_run_results", QJsonObject{{"runId", runId}});
        QCOMPARE(immediate.value("runId").toString(), runId);
        QCOMPARE(immediate.value("status").toString(), QStringLiteral("running"));
        const QJsonObject pending = actor.executeTool("wait_for_run", QJsonObject{{"runId", runId}});
        QCOMPARE(pending.value("status").toString(), QStringLiteral("pending"));
        QCOMPARE(pending.value("runId").toString(), runId);

        QVERIFY2(waitForRunIdle(actor).isEmpty(), "flow should finish");
        QTRY_COMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.at(0).at(0).value<RunResult>().runId, runId);
        const QJsonObject after = actor.executeTool("get_run_results", QJsonObject{{"runId", runId}});
        QCOMPARE(after.value("runId").toString(), runId);
        QCOMPARE(after.value("status").toString(), QStringLiteral("completed"));
        QCOMPARE(actor.executeTool("wait_for_run", QJsonObject{{"runId", runId}}).value("status").toString(),
                 QStringLiteral("completed"));
        const QJsonObject firstOutput =
            actor.executeTool("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "grab"}});
        QCOMPARE(firstOutput.value("status").toString(), QStringLiteral("available"));
        QVERIFY(firstOutput.value("summary").toObject().value("hasOutput").toBool());
        ProjectManager::instance().currentProject()->setName(QStringLiteral("changed_outside_agent"));
        QCOMPARE(actor.executeTool("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "grab"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("STALE_OUTPUT"));

        const QJsonObject secondRun = actor.executeTool("run_flow", QJsonObject{{"wait", false}});
        QVERIFY(!secondRun.contains("error"));
        const QString secondId = secondRun.value("runId").toString();
        QVERIFY(!secondId.isEmpty());
        QVERIFY(secondId != runId);
        QCOMPARE(actor.executeTool("get_module_output", QJsonObject{{"runId", secondId}, {"instanceId", "grab"}})
                     .value("status")
                     .toString(),
                 QStringLiteral("pending"));
        QVERIFY2(waitForRunIdle(actor).isEmpty(), "second flow should finish");
        QTRY_COMPARE(finishedSpy.count(), 2);
        QCOMPARE(finishedSpy.at(1).at(0).value<RunResult>().runId, secondId);
        QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", secondId}}).value("status").toString(),
                 QStringLiteral("completed"));
        QCOMPARE(actor.executeTool("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "grab"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("RUN_NOT_FOUND"));
        QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", "unknown"}}).value("errorCode").toString(),
                 QStringLiteral("RUN_NOT_FOUND"));
    }

    void testG3MeasurementOutputByRunId() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "g3_measurement"}}).contains("error"));
        QVERIFY(!actor
                     .executeTool("add_module",
                                  QJsonObject{{"plugin", m_nameMeasurementInput}, {"instanceName", "measure"}})
                     .contains("error"));
        const QJsonObject values{
            {"mode", "point_pair"}, {"point1", QJsonArray{1.0, 2.0}}, {"point2", QJsonArray{4.0, 6.0}}};
        QVERIFY(!actor.executeTool("set_module_params", QJsonObject{{"instanceId", "measure"}, {"values", values}})
                     .contains("error"));
        const QJsonObject started = actor.executeTool("run_flow", QJsonObject{{"wait", false}});
        QVERIFY2(!started.contains("error"), qPrintable(started.value("error").toString()));
        const QString runId = started.value("runId").toString();
        QVERIFY(!runId.isEmpty());
        QVERIFY2(waitForRunIdle(actor).isEmpty(), "measurement flow should finish");
        QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("status").toString(),
                 QStringLiteral("completed"));
        const QJsonObject point1 = actor.executeTool(
            "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"key", "point1"}});
        QCOMPARE(point1.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(point1.value("value").toArray(), QJsonArray({1.0, 2.0}));
        const QJsonObject portPoint = actor.executeTool(
            "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"port", "point1"}});
        QCOMPARE(portPoint.value("source").toString(), QStringLiteral("port"));
        QCOMPARE(portPoint.value("value").toArray(), QJsonArray({1.0, 2.0}));
        AgentActor secondActor;
        QCOMPARE(secondActor
                     .executeTool("get_module_output",
                                  QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"key", "point1"}})
                     .value("value")
                     .toArray(),
                 QJsonArray({1.0, 2.0}));
        ProjectManager::instance().currentProject()->setName(QStringLiteral("modified_elsewhere"));
        QCOMPARE(secondActor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("stale").toBool(),
                 true);
        QCOMPARE(secondActor
                     .executeTool("get_module_output",
                                  QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"key", "point1"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("STALE_OUTPUT"));
    }

    void testG3NPointCalibrationOutputPortsByRunId() {
        AgentActor actor;
        QVERIFY(
            !actor.executeTool("create_project", QJsonObject{{"name", "g3_calibration_outputs"}}).contains("error"));

        QImage source(32, 32, QImage::Format_RGB32);
        source.fill(Qt::black);
        const QString imagePath = m_appDataDir.filePath(QStringLiteral("g3_calibration_input.png"));
        QVERIFY(source.save(imagePath));

        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameGrabImage}, {"instanceName", "grab"}})
                     .contains("error"));
        const QJsonObject grabParams = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", "grab"},
                        {"values", QJsonObject{{"grabSource", QStringLiteral("Path")}, {"filePath", imagePath}}}});
        QVERIFY2(!grabParams.contains("error"), qPrintable(grabParams.value("error").toString()));
        QVERIFY(
            !actor.executeTool("add_module", QJsonObject{{"plugin", m_nameNPointCalibration}, {"instanceName", "cal"}})
                 .contains("error"));
        const QJsonObject calibrationSchema =
            actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", "cal"}});
        const QJsonObject pointProperties = calibrationSchema.value(QStringLiteral("parameters"))
                                                .toObject()
                                                .value(QStringLiteral("calibrationPoints"))
                                                .toObject()
                                                .value(QStringLiteral("items"))
                                                .toObject()
                                                .value(QStringLiteral("properties"))
                                                .toObject();
        const QString imagePointDescription =
            pointProperties.value(QStringLiteral("image")).toObject().value(QStringLiteral("description")).toString();
        QVERIFY(imagePointDescription.contains(QStringLiteral("当前输入图像局部像素坐标 (x,y)")));
        QVERIFY(imagePointDescription.contains(QStringLiteral("不是屏幕坐标")));
        const QString worldPointDescription =
            pointProperties.value(QStringLiteral("world")).toObject().value(QStringLiteral("description")).toString();
        QVERIFY(worldPointDescription.contains(QStringLiteral("调用者提供的目标平面 (x,y)")));
        QVERIFY(worldPointDescription.contains(QStringLiteral("单位、原点和参考框架由调用者定义")));
        QVERIFY(worldPointDescription.contains(QStringLiteral("不转换为毫米或固定外部世界坐标")));
        const QJsonObject reprojectionSchema = calibrationSchema.value(QStringLiteral("results"))
                                                   .toObject()
                                                   .value(QStringLiteral("reprojection_error"))
                                                   .toObject();
        QCOMPARE(reprojectionSchema.value(QStringLiteral("unit")).toString(),
                 QStringLiteral("calibrationPoints.world 的调用者定义单位"));
        QVERIFY(reprojectionSchema.value(QStringLiteral("description"))
                    .toString()
                    .contains(QStringLiteral("不表示固定物理单位")));
        const QJsonArray outputPorts = actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", "cal"}})
                                           .value("ports")
                                           .toObject()
                                           .value("outputs")
                                           .toArray();
        const QMap<QString, QString> expectedPortTypes{{QStringLiteral("reprojection_error"), QStringLiteral("Number")},
                                                       {QStringLiteral("point_count"), QStringLiteral("Integer")}};
        for (auto it = expectedPortTypes.cbegin(); it != expectedPortTypes.cend(); ++it) {
            const auto found = std::find_if(outputPorts.cbegin(), outputPorts.cend(), [&it](const QJsonValue& value) {
                return value.toObject().value("id").toString() == it.key();
            });
            QVERIFY(found != outputPorts.cend());
            QCOMPARE(found->toObject().value("type").toString(), it.value());
            QVERIFY(!found->toObject().value("required").toBool());
        }
        QVERIFY(std::none_of(outputPorts.cbegin(), outputPorts.cend(), [](const QJsonValue& value) {
            return value.toObject().value("id").toString() == QStringLiteral("calibration_valid");
        }));
        const QJsonArray points{QJsonObject{{"image", QJsonArray{0, 0}}, {"world", QJsonArray{0, 0}}},
                                QJsonObject{{"image", QJsonArray{31, 0}}, {"world", QJsonArray{31, 0}}},
                                QJsonObject{{"image", QJsonArray{31, 31}}, {"world", QJsonArray{24, 31}}},
                                QJsonObject{{"image", QJsonArray{0, 31}}, {"world", QJsonArray{0, 31}}}};
        const QJsonObject calibrationParams = actor.executeTool(
            "set_module_params", QJsonObject{{"instanceId", "cal"},
                                             {"values", QJsonObject{{"calibrationType", QStringLiteral("Perspective")},
                                                                    {"inverseTransform", false},
                                                                    {"clearPointsOnRun", true},
                                                                    {"calibrationPoints", points}}}});
        QVERIFY2(!calibrationParams.contains("error"), qPrintable(calibrationParams.value("error").toString()));
        const QJsonObject connected = actor.executeTool(
            "connect_modules",
            QJsonObject{{"fromId", "grab"}, {"toId", "cal"}, {"fromPort", "image"}, {"toPort", "image"}});
        QVERIFY2(!connected.contains("error"), qPrintable(connected.value("error").toString()));

        const QJsonObject started = actor.executeTool("run_flow", QJsonObject{{"wait", false}});
        QVERIFY2(!started.contains("error"), qPrintable(started.value("error").toString()));
        const QString runId = started.value("runId").toString();
        QVERIFY(!runId.isEmpty());
        QVERIFY2(waitForRunIdle(actor).isEmpty(), "calibration flow should finish");
        QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("status").toString(),
                 QStringLiteral("completed"));

        const ImageData output = RunEngine::instance().moduleOutput(QStringLiteral("cal"));
        QVERIFY(output.data(QStringLiteral("calibration_valid")).toBool());
        QCOMPARE(output.data(QStringLiteral("point_count")).toInt(), 4);
        QVERIFY(output.data(QStringLiteral("reprojection_error")).toDouble() < 0.001);
        for (const auto& expected :
             {qMakePair(QStringLiteral("reprojection_error"),
                        output.data(QStringLiteral("reprojection_error")).toDouble()),
              qMakePair(QStringLiteral("point_count"), output.data(QStringLiteral("point_count")).toDouble())}) {
            const QJsonObject byKey = actor.executeTool(
                "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "cal"}, {"key", expected.first}});
            const QJsonObject byPort = actor.executeTool(
                "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "cal"}, {"port", expected.first}});
            QCOMPARE(byKey.value("status").toString(), QStringLiteral("available"));
            QCOMPARE(byPort.value("status").toString(), QStringLiteral("available"));
            QCOMPARE(byPort.value("source").toString(), QStringLiteral("port"));
            QCOMPARE(byKey.value("value").toDouble(), expected.second);
            QCOMPARE(byPort.value("value").toDouble(), expected.second);
            if (expected.first == QLatin1String("reprojection_error")) {
                QCOMPARE(byKey.value("unit").toString(), QStringLiteral("calibrationPoints.world 的调用者定义单位"));
                QCOMPARE(byPort.value("unit").toString(), QStringLiteral("calibrationPoints.world 的调用者定义单位"));
            }
        }
        const QJsonObject calibrationValid = actor.executeTool(
            "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "cal"}, {"key", "calibration_valid"}});
        QCOMPARE(calibrationValid.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(calibrationValid.value("value").toBool(), true);

        const QJsonObject affineParams = actor.executeTool(
            "set_module_params", QJsonObject{{"instanceId", "cal"},
                                             {"values", QJsonObject{{"calibrationType", QStringLiteral("Affine")},
                                                                    {"inverseTransform", false},
                                                                    {"clearPointsOnRun", true},
                                                                    {"calibrationPoints", points}}}});
        QVERIFY2(!affineParams.contains("error"), qPrintable(affineParams.value("error").toString()));
        const QJsonObject affineStarted = actor.executeTool("run_flow", {});
        QVERIFY2(!affineStarted.contains("error"), qPrintable(affineStarted.value("error").toString()));
        const QString affineRunId = affineStarted.value("runId").toString();
        QVERIFY(!affineRunId.isEmpty());
        QVERIFY2(waitForRunIdle(actor).isEmpty(), "affine calibration flow should finish");
        QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", affineRunId}}).value("status").toString(),
                 QStringLiteral("completed"));
        const QJsonObject affineCalibrationValid =
            actor.executeTool("get_module_output",
                              QJsonObject{{"runId", affineRunId}, {"instanceId", "cal"}, {"key", "calibration_valid"}});
        QCOMPARE(affineCalibrationValid.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(affineCalibrationValid.value("value").toBool(), true);
        for (const auto& expected :
             {qMakePair(QStringLiteral("reprojection_error"), 3.5), qMakePair(QStringLiteral("point_count"), 4.0)}) {
            const QJsonObject byKey =
                actor.executeTool("get_module_output",
                                  QJsonObject{{"runId", affineRunId}, {"instanceId", "cal"}, {"key", expected.first}});
            const QJsonObject byPort =
                actor.executeTool("get_module_output",
                                  QJsonObject{{"runId", affineRunId}, {"instanceId", "cal"}, {"port", expected.first}});
            QCOMPARE(byKey.value("status").toString(), QStringLiteral("available"));
            QCOMPARE(byPort.value("status").toString(), QStringLiteral("available"));
            QCOMPARE(byKey.value("value").toDouble(), byPort.value("value").toDouble());
            if (expected.first == QLatin1String("reprojection_error")) {
                QVERIFY(byKey.value("value").toDouble() >= 3.4);
                QVERIFY(byKey.value("value").toDouble() <= 3.6);
                QCOMPARE(byKey.value("unit").toString(), QStringLiteral("calibrationPoints.world 的调用者定义单位"));
                QCOMPARE(byPort.value("unit").toString(), QStringLiteral("calibrationPoints.world 的调用者定义单位"));
            } else {
                QCOMPARE(byKey.value("value").toInt(), 4);
            }
        }

        const QJsonObject cleared = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", "cal"},
                        {"values", QJsonObject{{"calibrationPoints", QJsonArray{}}, {"clearPointsOnRun", true}}}});
        QVERIFY2(!cleared.contains("error"), qPrintable(cleared.value("error").toString()));
        const QJsonObject failedStart = actor.executeTool("run_flow", QJsonObject{{"wait", false}});
        QVERIFY2(!failedStart.contains("error"), qPrintable(failedStart.value("error").toString()));
        const QString failedRunId = failedStart.value("runId").toString();
        QVERIFY(!failedRunId.isEmpty());
        QVERIFY2(waitForRunIdle(actor).isEmpty(), "invalid calibration flow should finish");
        const QJsonObject failedResult = actor.executeTool("get_run_results", QJsonObject{{"runId", failedRunId}});
        QCOMPARE(failedResult.value("status").toString(), QStringLiteral("failed"));
        const QJsonObject calibrationError = failedResult.value("lastError").toObject();
        QCOMPARE(calibrationError.value("moduleId").toString(), QStringLiteral("cal"));
        QVERIFY(!calibrationError.value("diagnostics").toString().trimmed().isEmpty());
        for (const QString& result : {QStringLiteral("calibration_valid"), QStringLiteral("reprojection_error"),
                                      QStringLiteral("point_count")}) {
            const QJsonObject unavailable = actor.executeTool(
                "get_module_output", QJsonObject{{"runId", failedRunId}, {"instanceId", "cal"}, {"key", result}});
            QCOMPARE(unavailable.value("status").toString(), QStringLiteral("unavailable"));
            QVERIFY(!unavailable.contains("value"));
        }
        for (const QString& port : {QStringLiteral("reprojection_error"), QStringLiteral("point_count")}) {
            const QJsonObject unavailable = actor.executeTool(
                "get_module_output", QJsonObject{{"runId", failedRunId}, {"instanceId", "cal"}, {"port", port}});
            QCOMPARE(unavailable.value("status").toString(), QStringLiteral("unavailable"));
            QVERIFY(!unavailable.contains("value"));
        }
    }

    void testG3CycleSessionAndIterationIds() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "g3_cycle"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameDelay}, {"instanceName", "delay"}})
                     .contains("error"));
        const QJsonObject started = actor.executeTool("run_flow", QJsonObject{{"mode", "cycle"}, {"maxCycles", 10}});
        QCOMPARE(started.value("status").toString(), QStringLiteral("started"));
        const QString sessionId = started.value("sessionId").toString();
        QVERIFY(!sessionId.isEmpty());
        QVERIFY(started.value("runId").toString() != sessionId);
        QTRY_VERIFY(!actor.executeTool("get_run_results", QJsonObject{{"runId", sessionId}})
                         .value("lastIterationRunId")
                         .toString()
                         .isEmpty());
        const QJsonObject session = actor.executeTool("get_run_results", QJsonObject{{"runId", sessionId}});
        QCOMPARE(session.value("status").toString(), QStringLiteral("running"));
        QVERIFY(session.value("isBusy").toBool());
        const QString iterationId = session.value("lastIterationRunId").toString();
        QVERIFY(iterationId != sessionId);
        QVERIFY(!actor.executeTool("get_run_results", QJsonObject{{"runId", iterationId}}).contains("error"));
        actor.executeTool("stop_flow", {});
        QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", sessionId}}).value("status").toString(),
                 QStringLiteral("cancelled"));
    }

    void testG3ImmediateLlmOutputQueryResumesWithoutPollingTurns() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "g3_llm_wait"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameGrabImage}, {"instanceName", "grab"}})
                     .contains("error"));
        QVERIFY(!actor
                     .executeTool("set_param",
                                  QJsonObject{{"instanceId", "grab"}, {"key", "grabSource"}, {"value", "Demo"}})
                     .contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameDelay}, {"instanceName", "delay"}})
                     .contains("error"));
        QVERIFY(
            !actor.executeTool("set_param", QJsonObject{{"instanceId", "delay"}, {"key", "delayMs"}, {"value", "5000"}})
                 .contains("error"));
        auto& controller = AgentController::instance();
        controller.clearConversation();
        auto* mock = new MockLLMClient(&controller);
        ILLMClient* old = controller.llmClient();
        controller.setLLMClient(mock);
        QSignalSpy loopSpy(&controller, &AgentController::agentLoopIterating);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.sendUserMessage(QStringLiteral("读取刚才运行的图像输出"));
        QCOMPARE(mock->requestCount, 1);
        const QJsonObject started = actor.executeTool("run_flow", QJsonObject{{"wait", false}});
        const QString runId = started.value("runId").toString();
        QVERIFY2(!runId.isEmpty(), qPrintable(started.value("error").toString()));
        AgentResponse query;
        query.success = true;
        query.toolCalls.append(
            makeToolCallJson("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "grab"}}));
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 lastHeartbeatMs = elapsed.elapsed();
        qint64 maxHeartbeatDelayMs = 0;
        int heartbeatCount = 0;
        QTimer heartbeat(&controller);
        heartbeat.setInterval(50);
        QObject::connect(&heartbeat, &QTimer::timeout, &controller, [&]() {
            const qint64 now = elapsed.elapsed();
            maxHeartbeatDelayMs = qMax(maxHeartbeatDelayMs, now - lastHeartbeatMs);
            lastHeartbeatMs = now;
            ++heartbeatCount;
        });
        heartbeat.start();
        mock->emitResponse(query);
        QCOMPARE(mock->requestCount, 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(logSpy.last().first()).result, QStringLiteral("pending"));
        QTRY_COMPARE_WITH_TIMEOUT(mock->requestCount, 2, 10000);
        heartbeat.stop();
        QCOMPARE(loopSpy.count(), 1);
        QVERIFY(heartbeatCount >= 3);
        QVERIFY(maxHeartbeatDelayMs <= 500);
        qInfo().noquote() << "G3 event-loop heartbeat max interval (ms):" << maxHeartbeatDelayMs;
        QJsonObject output;
        for (const AgentMessage& message : mock->lastCtx.messages) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("repro_get_module_output"))
                output = QJsonDocument::fromJson(message.content.toUtf8()).object();
        }
        QCOMPARE(output.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(logSpy.last().first()).result, QStringLiteral("success"));
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        controller.clearConversation();
        controller.setLLMClient(old);
        delete mock;
    }

    void testG3PendingQueryAuditFinalError() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "g3_audit_wait"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameDelay}, {"instanceName", "delay"}})
                     .contains("error"));
        QVERIFY(
            !actor.executeTool("set_param", QJsonObject{{"instanceId", "delay"}, {"key", "delayMs"}, {"value", "5000"}})
                 .contains("error"));
        auto& controller = AgentController::instance();
        controller.clearConversation();
        auto* mock = new MockLLMClient(&controller);
        ILLMClient* old = controller.llmClient();
        controller.setLLMClient(mock);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.sendUserMessage(QStringLiteral("读取运行结果"));
        QCOMPARE(mock->requestCount, 1);
        const QString runId = actor.executeTool("run_flow", QJsonObject{{"wait", false}}).value("runId").toString();
        QVERIFY(!runId.isEmpty());
        AgentResponse query;
        query.success = true;
        query.toolCalls.append(
            makeToolCallJson("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "delay"}}));
        mock->emitResponse(query);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(logSpy.last().first()).result, QStringLiteral("pending"));
        ProjectManager::instance().currentProject()->setName(QStringLiteral("changed_during_wait"));
        QTRY_COMPARE_WITH_TIMEOUT(mock->requestCount, 2, 10000);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(logSpy.last().first()).result, QStringLiteral("error"));
        QJsonObject finalResult;
        for (const AgentMessage& message : mock->lastCtx.messages)
            if (message.role == QLatin1String("tool"))
                finalResult = QJsonDocument::fromJson(message.content.toUtf8()).object();
        QCOMPARE(finalResult.value("errorCode").toString(), QStringLiteral("STALE_OUTPUT"));
        controller.clearConversation();
        controller.setLLMClient(old);
        delete mock;
    }

    void testG3PendingQueryCancelledAfterBatchFailure() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "g3_batch_wait"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameDelay}, {"instanceName", "delay"}})
                     .contains("error"));
        QVERIFY(
            !actor.executeTool("set_param", QJsonObject{{"instanceId", "delay"}, {"key", "delayMs"}, {"value", "5000"}})
                 .contains("error"));
        auto& controller = AgentController::instance();
        controller.clearConversation();
        auto* mock = new MockLLMClient(&controller);
        ILLMClient* old = controller.llmClient();
        controller.setLLMClient(mock);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        controller.sendUserMessage(QStringLiteral("读取运行结果"));
        const QString runId = actor.executeTool("run_flow", QJsonObject{{"wait", false}}).value("runId").toString();
        QVERIFY(!runId.isEmpty());
        AgentResponse query;
        query.success = true;
        query.toolCalls.append(
            makeToolCallJson("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "delay"}}));
        query.toolCalls.append(makeToolCallJson("get_run_results", QJsonObject{{"runId", "missing-run"}}));
        mock->emitResponse(query);
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(mock->requestCount, 1);
        QCOMPARE(logSpy.count(), 3);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(logSpy.at(0).first()).result, QStringLiteral("pending"));
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(logSpy.at(1).first()).result, QStringLiteral("error"));
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(logSpy.at(2).first()).result, QStringLiteral("cancelled"));
        controller.sendUserMessage(QStringLiteral("检查上批结果"));
        int pendingCallMessages = 0;
        for (const AgentMessage& message : mock->lastCtx.messages) {
            if (message.role == QLatin1String("tool") &&
                message.toolCallId == QLatin1String("repro_get_module_output")) {
                ++pendingCallMessages;
                const QJsonObject result = QJsonDocument::fromJson(message.content.toUtf8()).object();
                QCOMPARE(result.value("status").toString(), QStringLiteral("cancelled"));
                QCOMPARE(result.value("reason").toString(), QStringLiteral("batch_failed"));
            }
        }
        QCOMPARE(pendingCallMessages, 1);
        QVERIFY2(waitForRunIdle(actor).isEmpty(),
                 "cancelled query must not leave the engine running into the next test");
        controller.clearConversation();
        controller.setLLMClient(old);
        delete mock;
    }

    void testG3ClearingConversationClosesPendingAudit() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "g3_clear_wait"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameDelay}, {"instanceName", "delay"}})
                     .contains("error"));
        QVERIFY(
            !actor.executeTool("set_param", QJsonObject{{"instanceId", "delay"}, {"key", "delayMs"}, {"value", "5000"}})
                 .contains("error"));
        auto& controller = AgentController::instance();
        controller.clearConversation();
        auto* mock = new MockLLMClient(&controller);
        ILLMClient* old = controller.llmClient();
        controller.setLLMClient(mock);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.sendUserMessage(QStringLiteral("读取运行结果"));
        const QString runId = actor.executeTool("run_flow", QJsonObject{{"wait", false}}).value("runId").toString();
        QVERIFY(!runId.isEmpty());
        AgentResponse query;
        query.success = true;
        query.toolCalls.append(
            makeToolCallJson("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "delay"}}));
        mock->emitResponse(query);
        QCOMPARE(logSpy.count(), 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(logSpy.at(0).first()).result, QStringLiteral("pending"));
        controller.clearConversation();
        QCOMPARE(logSpy.count(), 2);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(logSpy.at(1).first()).result, QStringLiteral("cancelled"));
        QVERIFY2(waitForRunIdle(actor).isEmpty(), "cleared query must not leave the engine running into the next test");
        QCOMPARE(logSpy.count(), 2);
        controller.setLLMClient(old);
        delete mock;
    }

    void testG3OutputPagingLimitAndUnsupportedType() {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("g3-output");
        instance.moduleId = QStringLiteral("com.deeplux.test.g3output");
        instance.name = QStringLiteral("G3Output");
        project->addModule(instance);
        RunEngine& engine = RunEngine::instance();
        QVERIFY(engine.loadProject(project, [](const ModuleInstance&) { return new G3LargePayloadModule; }));
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QTRY_VERIFY(engine.runObservation(runId).finished);
        AgentActor actor;
        const QJsonObject base{{"runId", runId}, {"instanceId", instance.id}};
        const QJsonObject summary = actor.executeTool("get_module_output", base);
        QCOMPARE(summary.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(summary.value("summary").toObject().value("image").toObject().value("noData").toDouble(), -999.0);

        QJsonObject pageParams = base;
        pageParams["key"] = QStringLiteral("table");
        const QJsonObject first = actor.executeTool("get_module_output", pageParams);
        QCOMPARE(first.value("totalRows").toInt(), 205);
        QCOMPARE(first.value("value").toArray().size(), 100);
        QVERIFY(first.value("truncated").toBool());
        pageParams["offset"] = 200;
        const QJsonObject last = actor.executeTool("get_module_output", pageParams);
        QCOMPARE(last.value("value").toArray().size(), 5);
        QVERIFY(!last.value("truncated").toBool());
        pageParams["limit"] = 1001;
        QCOMPARE(actor.executeTool("get_module_output", pageParams).value("errorCode").toString(),
                 QStringLiteral("INVALID_PARAM"));

        QJsonObject bigTable = base;
        bigTable["key"] = QStringLiteral("bigtable");
        const QJsonObject boundedPage = actor.executeTool("get_module_output", bigTable);
        QCOMPARE(boundedPage.value("totalRows").toInt(), 2);
        QCOMPARE(boundedPage.value("value").toArray().size(), 1);
        QVERIFY(boundedPage.value("truncated").toBool());
        QVERIFY(QJsonDocument(boundedPage).toJson(QJsonDocument::Compact).size() <= 65536);
        bigTable["key"] = QStringLiteral("tooBigRow");
        QCOMPARE(actor.executeTool("get_module_output", bigTable).value("errorCode").toString(),
                 QStringLiteral("ROW_TOO_LARGE"));

        QJsonObject oversized = base;
        oversized["key"] = QStringLiteral("oversize");
        const QJsonObject cut = actor.executeTool("get_module_output", oversized);
        QCOMPARE(cut.value("status").toString(), QStringLiteral("truncated"));
        QVERIFY(!cut.contains("value"));
        QVERIFY(QJsonDocument(cut).toJson(QJsonDocument::Compact).size() <= 65536);
        QJsonObject unsupported = base;
        unsupported["key"] = QStringLiteral("unsupported");
        QCOMPARE(actor.executeTool("get_module_output", unsupported).value("errorCode").toString(),
                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        unsupported["key"] = QStringLiteral("nestedUnsupported");
        QCOMPARE(actor.executeTool("get_module_output", unsupported).value("errorCode").toString(),
                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        const QJsonObject cloud = actor.executeTool(
            "get_module_output",
            QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"key", "cloud"}, {"preview", true}});
        QCOMPARE(cloud.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(cloud.value("value").toObject().value("pointCount").toInt(), 2);
        QCOMPARE(cloud.value("preview").toObject().value("sampleCount").toInt(), 2);
        QCOMPARE(cloud.value("preview").toObject().value("points").toArray().first().toArray(), QJsonArray({1, 2, 3}));
        const QJsonObject region = actor.executeTool(
            "get_module_output",
            QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"key", "region"}, {"preview", true}});
        QCOMPARE(region.value("status").toString(), QStringLiteral("available"));
        const QJsonObject maskPreview = region.value("preview").toObject().value("mask").toObject();
        QCOMPARE(maskPreview.value("sourceRect").toArray(), QJsonArray({0, 0, 64, 32}));
        QVERIFY(!QImage::fromData(QByteArray::fromBase64(maskPreview.value("base64").toString().toLatin1())).isNull());
        QVERIFY(QJsonDocument(region).toJson(QJsonDocument::Compact).size() <= 65536);
        const QJsonObject largeRegion = actor.executeTool(
            "get_module_output",
            QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"key", "largeRegion"}, {"preview", true}});
        QCOMPARE(largeRegion.value("status").toString(), QStringLiteral("available"));
        const int totalMaskChars = largeRegion.value("value").toObject().value("maskRleChars").toInt();
        QVERIFY(totalMaskChars > 1024 * 1024);
        QVERIFY(largeRegion.value("preview").toObject().contains("maskUnavailable"));
        QCOMPARE(largeRegion.value("maskExport").toObject().value("chunkChars").toInt(), 8192);
        QVERIFY(QJsonDocument(largeRegion).toJson(QJsonDocument::Compact).size() <= 65536);
        QJsonObject chunkRequest{
            {"runId", runId}, {"instanceId", instance.id}, {"key", "largeRegion"}, {"maskOffset", 0}};
        const QJsonObject chunk = actor.executeTool("get_module_output", chunkRequest);
        QCOMPARE(chunk.value("maskRleChunk").toString().size(), 8192);
        QCOMPARE(chunk.value("nextMaskOffset").toInt(), 8192);
        QVERIFY(QJsonDocument(chunk).toJson(QJsonDocument::Compact).size() <= 65536);
        QString reconstructedMask = chunk.value("maskRleChunk").toString();
        for (int offset = 8192; offset < totalMaskChars;) {
            chunkRequest["maskOffset"] = offset;
            const QJsonObject next = actor.executeTool("get_module_output", chunkRequest);
            QCOMPARE(next.value("status").toString(), QStringLiteral("available"));
            QVERIFY(QJsonDocument(next).toJson(QJsonDocument::Compact).size() <= 65536);
            const QString segment = next.value("maskRleChunk").toString();
            QVERIFY(!segment.isEmpty());
            reconstructedMask += segment;
            offset = next.value("nextMaskOffset").toInt();
        }
        QCOMPARE(reconstructedMask.size(), totalMaskChars);
        bool maskValid = false;
        const QImage decodedMask = MaskOps::decodeRle(reconstructedMask, QSize(64, 32), &maskValid);
        QVERIFY(maskValid);
        QCOMPARE(decodedMask.width(), 64);
        QCOMPARE(decodedMask.height(), 32);
        QCOMPARE(decodedMask.pixelColor(0, 0), QColor(Qt::white));
        const QJsonObject largeCloud = actor.executeTool(
            "get_module_output",
            QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"key", "largeCloud"}, {"preview", true}});
        QCOMPARE(largeCloud.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(largeCloud.value("preview").toObject().value("sampleCount").toInt(), 64);
        QVERIFY(QJsonDocument(largeCloud).toJson(QJsonDocument::Compact).size() <= 65536);
        const QString oversizedIdentifier(70000, QLatin1Char('x'));
        const QJsonObject longRun = actor.executeTool("get_run_results", QJsonObject{{"runId", oversizedIdentifier}});
        QCOMPARE(longRun.value("errorCode").toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(QJsonDocument(longRun).toJson(QJsonDocument::Compact).size() <= 65536);
        const QJsonObject longKey =
            actor.executeTool("get_module_output",
                              QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"key", oversizedIdentifier}});
        QCOMPARE(longKey.value("errorCode").toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(QJsonDocument(longKey).toJson(QJsonDocument::Compact).size() <= 65536);
    }

    void testG3PortAndKeyNamespacesAreDistinct() {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("g3-collision");
        instance.moduleId = QStringLiteral("com.deeplux.test.g3collision");
        project->addModule(instance);
        RunEngine& engine = RunEngine::instance();
        QVERIFY(engine.loadProject(project, [](const ModuleInstance&) { return new G3CollisionModule; }));
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QTRY_VERIFY(engine.runObservation(runId).finished);
        AgentActor actor;
        QJsonObject query{{"runId", runId}, {"instanceId", instance.id}, {"key", "collision"}};
        QCOMPARE(actor.executeTool("get_module_output", query).value("value").toInt(), 11);
        query.remove("key");
        query["port"] = QStringLiteral("collision");
        QCOMPARE(actor.executeTool("get_module_output", query).value("value").toInt(), 99);
        query["port"] = QStringLiteral("metadata-only");
        QCOMPARE(actor.executeTool("get_module_output", query).value("errorCode").toString(),
                 QStringLiteral("OUTPUT_NOT_FOUND"));
    }

    void testG3CyclePreviousTickOutputAvailableDuringNextTick() {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("g3-cycle-output");
        instance.moduleId = QStringLiteral("com.deeplux.test.g3output");
        project->addModule(instance);
        RunEngine& engine = RunEngine::instance();
        QVERIFY(engine.loadProject(project, [](const ModuleInstance&) { return new G3SlowCycleModule; }));
        QString sessionId;
        QVERIFY(engine.startCycle(&sessionId));
        QVERIFY(!sessionId.isEmpty());
        engine.onTimerTick();
        const QString firstId = engine.runObservation(sessionId).lastCompletedRunId;
        QVERIFY(!firstId.isEmpty());
        std::thread nextTick([&engine]() { engine.onTimerTick(); });
        bool nextStarted = false;
        for (int i = 0; i < 100 && !nextStarted; ++i) {
            nextStarted = engine.runObservation(sessionId).lastIterationRunId != firstId;
            if (!nextStarted)
                QThread::msleep(10);
        }
        AgentActor actor;
        const QJsonObject session = actor.executeTool("get_run_results", QJsonObject{{"runId", sessionId}});
        const QJsonObject firstStatus = actor.executeTool("get_run_results", QJsonObject{{"runId", firstId}});
        const QJsonObject value = actor.executeTool(
            "get_module_output", QJsonObject{{"runId", firstId}, {"instanceId", instance.id}, {"key", "table"}});
        QVERIFY(nextStarted);
        QCOMPARE(session.value("lastCompletedRunId").toString(), firstId);
        QCOMPARE(firstStatus.value("status").toString(), QStringLiteral("completed"));
        QCOMPARE(value.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(value.value("totalRows").toInt(), 205);
        engine.stop();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1000);
        nextTick.join();
        QTRY_VERIFY(!engine.isExecuting());
    }

    void testG3FailedRunCannotReusePreviousOutput() {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        ModuleInstance successInstance;
        successInstance.id = QStringLiteral("g3-output");
        successInstance.moduleId = QStringLiteral("com.deeplux.test.g3output");
        project->addModule(successInstance);
        RunEngine& engine = RunEngine::instance();
        QVERIFY(engine.loadProject(project, [](const ModuleInstance&) { return new G3OutputModule; }));
        QString firstRunId;
        QVERIFY(engine.runOnceAsync(&firstRunId));
        QTRY_VERIFY(engine.runObservation(firstRunId).finished);
        AgentActor actor;
        const QJsonObject firstOutput =
            actor.executeTool("get_module_output",
                              QJsonObject{{"runId", firstRunId}, {"instanceId", successInstance.id}, {"key", "table"}});
        QCOMPARE(firstOutput.value("status").toString(), QStringLiteral("available"));

        ModuleInstance failureInstance;
        failureInstance.id = QStringLiteral("g3-failure");
        failureInstance.moduleId = QStringLiteral("com.deeplux.test.g3failure");
        project->addModule(failureInstance);
        QVERIFY(engine.loadProject(project, [](const ModuleInstance& instance) -> ModuleBase* {
            return instance.id == QLatin1String("g3-failure") ? static_cast<ModuleBase*>(new G3FailureModule)
                                                              : static_cast<ModuleBase*>(new G3OutputModule);
        }));
        QString secondRunId;
        QVERIFY(engine.runOnceAsync(&secondRunId));
        QTRY_VERIFY(engine.runObservation(secondRunId).finished);
        const QJsonObject secondStatus = actor.executeTool("get_run_results", QJsonObject{{"runId", secondRunId}});
        QCOMPARE(secondStatus.value("status").toString(), QStringLiteral("failed"));
        const QJsonObject error = secondStatus.value("lastError").toObject();
        QCOMPARE(error.value("moduleId").toString(), failureInstance.id);
        QVERIFY(error.value("diagnostics").toString().contains(QStringLiteral("service timeout")));
        const QJsonObject unavailable = actor.executeTool(
            "get_module_output",
            QJsonObject{{"runId", secondRunId}, {"instanceId", successInstance.id}, {"key", "table"}});
        QCOMPARE(unavailable.value("status").toString(), QStringLiteral("unavailable"));
        QCOMPARE(
            actor
                .executeTool("get_module_output",
                             QJsonObject{{"runId", firstRunId}, {"instanceId", successInstance.id}, {"key", "table"}})
                .value("errorCode")
                .toString(),
            QStringLiteral("RUN_NOT_FOUND"));
    }

    void testG3AgentReportsMissingInputAndTypeMismatch() {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("g3-bad-port");
        instance.moduleId = QStringLiteral("com.deeplux.test.g3output");
        project->addModule(instance);
        RunEngine& engine = RunEngine::instance();
        AgentActor actor;
        for (int kind = 0; kind < 2; ++kind) {
            QVERIFY(engine.loadProject(project, [kind](const ModuleInstance&) -> ModuleBase* {
                return kind == 0 ? static_cast<ModuleBase*>(new G3MissingInputModule)
                                 : static_cast<ModuleBase*>(new G3BadPortModule);
            }));
            QString runId;
            QVERIFY(engine.runOnceAsync(&runId));
            QTRY_VERIFY(engine.runObservation(runId).finished);
            const QJsonObject result = actor.executeTool("get_run_results", QJsonObject{{"runId", runId}});
            QCOMPARE(result.value("status").toString(), QStringLiteral("failed"));
            const QJsonObject error = result.value("lastError").toObject();
            QCOMPARE(error.value("moduleId").toString(), instance.id);
            QCOMPARE(error.value("port").toString(), kind == 0 ? QStringLiteral("point") : QStringLiteral("value"));
            QVERIFY(!error.value("diagnostics").toString().isEmpty());
        }
    }

    void testG3PausedThenCancelledRunHasNoOutput() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "g3_pause_cancel"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameDelay}, {"instanceName", "delay"}})
                     .contains("error"));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        ModuleInstance* delay = project->findModule(QStringLiteral("delay"));
        QVERIFY(delay);
        delay->breakpoint = true;
        const QJsonObject started = actor.executeTool("run_flow", {});
        const QString runId = started.value("runId").toString();
        QVERIFY2(!runId.isEmpty(), qPrintable(started.value("error").toString()));
        QTRY_COMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("status").toString(),
                     QStringLiteral("paused"));
        QCOMPARE(actor.executeTool("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "delay"}})
                     .value("status")
                     .toString(),
                 QStringLiteral("pending"));
        actor.executeTool("stop_flow", {});
        QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("status").toString(),
                 QStringLiteral("cancelled"));
        QCOMPARE(actor.executeTool("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "delay"}})
                     .value("status")
                     .toString(),
                 QStringLiteral("unavailable"));
    }

    // F02（阶段 2 翻转）：保留原复现入口，断言声明的工具可写入数值数组。
    void testF02SetParamCannotExpressArray() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_array"}}).contains("error"));
        QVERIFY(
            !actor.executeTool("add_module", QJsonObject{{"plugin", m_nameMeasurementInput}, {"instanceName", "mi"}})
                 .contains("error"));

        // 工具 Schema 允许实际 JSON 数组，而非只能提供字符串。
        const ToolDefinition def = ToolSchema::instance().findTool("set_param");
        bool acceptsJson = false;
        for (const ToolParam& p : def.params)
            if (p.name == QLatin1String("value"))
                acceptsJson = p.toJson().value(QStringLiteral("anyOf")).isArray();
        QVERIFY(acceptsJson);

        const QJsonArray expected{10, 20};
        const QJsonObject r =
            actor.executeTool("set_param", QJsonObject{{"instanceId", "mi"}, {"key", "point1"}, {"value", expected}});
        QCOMPARE(r.value(QStringLiteral("status")).toString(), QStringLiteral("param_set"));
        const QJsonObject schema = actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", "mi"}});
        const QJsonValue point1 = schema.value("currentParams").toObject().value("point1");
        QCOMPARE(point1, QJsonValue(expected));
    }

    void testG2TypedArrayTraversesController() {
        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(
            !controller.actor()->executeTool("create_project", QJsonObject{{"name", "g2_typed"}}).contains("error"));
        const QJsonObject added = controller.actor()->executeTool(
            "add_module", QJsonObject{{"plugin", m_nameMeasurementInput}, {"instanceName", "mi"}});
        QVERIFY(!added.contains("error"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        QVERIFY(!instanceId.isEmpty());
        QVERIFY(!controller.actor()
                     ->executeTool("add_module", QJsonObject{{"plugin", m_nameGrabImage}, {"instanceName", "grab"}})
                     .contains("error"));
        QVERIFY(!controller.actor()
                     ->executeTool("add_module",
                                   QJsonObject{{"plugin", m_nameNPointCalibration}, {"instanceName", "calibration"}})
                     .contains("error"));

        auto* mock = new MockLLMClient(&controller);
        ILLMClient* old = controller.llmClient();
        controller.setLLMClient(mock);
        controller.sendUserMessage(QStringLiteral("设置两组测量点"));
        QJsonObject declaredSchema;
        QJsonArray valueTypes;
        for (const ToolDefinition& tool : mock->lastTools) {
            const QJsonObject properties = tool.toOpenAIFunction()
                                               .value(QStringLiteral("function"))
                                               .toObject()
                                               .value(QStringLiteral("parameters"))
                                               .toObject()
                                               .value(QStringLiteral("properties"))
                                               .toObject();
            if (tool.name == QLatin1String("set_module_params"))
                declaredSchema = tool.toOpenAIFunction()
                                     .value(QStringLiteral("function"))
                                     .toObject()
                                     .value(QStringLiteral("parameters"))
                                     .toObject();
            if (tool.name == QLatin1String("set_param")) {
                for (const QJsonValue& alternative :
                     properties.value(QStringLiteral("value")).toObject().value("anyOf").toArray())
                    valueTypes.append(alternative.toObject().value("type"));
            }
        }
        const QJsonObject declaredProperties = declaredSchema.value("properties").toObject();
        QCOMPARE(declaredProperties.value("instanceId").toObject().value("type"), QJsonValue("string"));
        QCOMPARE(declaredProperties.value("values").toObject().value("type"), QJsonValue("object"));
        QCOMPARE(declaredSchema.value("required").toArray(), QJsonArray({"instanceId", "values"}));
        for (const char* kind : {"array", "object", "number", "boolean"})
            QVERIFY(valueTypes.contains(QString::fromLatin1(kind)));
        const auto makeDeclaredRequest = [&declaredSchema](const QString& id, const QJsonObject& values) {
            const QJsonObject source{{"instanceId", id}, {"values", values}};
            QJsonObject request;
            for (const QJsonValue& key : declaredSchema.value("required").toArray())
                request.insert(key.toString(), source.value(key.toString()));
            return request;
        };

        const QJsonArray first{10, 20};
        const QJsonArray second{30, 40};
        const QJsonObject args = makeDeclaredRequest(instanceId, QJsonObject{{"point1", first}, {"point2", second}});
        AgentResponse response;
        response.success = true;
        response.toolCalls.append(QJsonObject{
            {"id", "g2_typed_call"},
            {"type", "function"},
            {"function",
             QJsonObject{{"name", "set_module_params"},
                         {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}});
        const QJsonObject numericBoolean =
            makeDeclaredRequest("grab", QJsonObject{{"grabTimeout", 2300}, {"folderLoop", true}});
        response.toolCalls.append(QJsonObject{
            {"id", "g2_number_boolean_call"},
            {"type", "function"},
            {"function",
             QJsonObject{
                 {"name", "set_module_params"},
                 {"arguments", QString::fromUtf8(QJsonDocument(numericBoolean).toJson(QJsonDocument::Compact))}}}});
        const QJsonArray pointPairs{QJsonObject{{"image", QJsonArray{0, 0}}, {"world", QJsonArray{2, 3}}},
                                    QJsonObject{{"image", QJsonArray{10, 0}}, {"world", QJsonArray{12, 3}}},
                                    QJsonObject{{"image", QJsonArray{10, 10}}, {"world", QJsonArray{12, 13}}},
                                    QJsonObject{{"image", QJsonArray{0, 10}}, {"world", QJsonArray{2, 13}}}};
        const QJsonObject nestedObjects =
            makeDeclaredRequest("calibration", QJsonObject{{"calibrationPoints", pointPairs}});
        response.toolCalls.append(QJsonObject{
            {"id", "g2_nested_object_call"},
            {"type", "function"},
            {"function",
             QJsonObject{
                 {"name", "set_module_params"},
                 {"arguments", QString::fromUtf8(QJsonDocument(nestedObjects).toJson(QJsonDocument::Compact))}}}});
        mock->emitResponse(response);
        const QJsonObject stored = ProjectManager::instance().currentProject()->moduleById(instanceId)->params;
        QCOMPARE(stored.value(QStringLiteral("point1")), QJsonValue(first));
        QCOMPARE(stored.value(QStringLiteral("point2")), QJsonValue(second));
        const QJsonObject grabStored = ProjectManager::instance().currentProject()->moduleById("grab")->params;
        QCOMPARE(grabStored.value("grabTimeout"), QJsonValue(2300));
        QCOMPARE(grabStored.value("folderLoop"), QJsonValue(true));
        QCOMPARE(
            ProjectManager::instance().currentProject()->moduleById("calibration")->params.value("calibrationPoints"),
            QJsonValue(pointPairs));
        QVERIFY(controller.actor()->undoStack()->canUndo());

        controller.setLLMClient(old);
        delete mock;
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    // F03（阶段 2/4 分段收口）：静态参数/端口与点集 item 结构，以及流程连线端口均可查。
    void testF03SchemaAndFlowStateExposePorts() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_schema"}}).contains("error"));
        QVERIFY(
            !actor.executeTool("add_module", QJsonObject{{"plugin", m_nameMeasurementInput}, {"instanceName", "input"}})
                 .contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameFitCircle}, {"instanceName", "fit"}})
                     .contains("error"));

        const QJsonObject schema = actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", "input"}});
        QVERIFY(schema.contains("defaultParams"));
        QVERIFY(schema.contains("currentParams"));
        QVERIFY(schema.value("parameters").isObject());
        QVERIFY(schema.value("ports").isObject());
        QCOMPARE(schema.value("parameters")
                     .toObject()
                     .value("points")
                     .toObject()
                     .value("items")
                     .toObject()
                     .value("type")
                     .toString(),
                 QStringLiteral("array"));

        const QJsonObject conn = actor.executeTool(
            "connect_modules",
            QJsonObject{{"fromId", "input"}, {"toId", "fit"}, {"fromPort", "fit_points"}, {"toPort", "fit_points"}});
        QVERIFY2(!conn.contains("error"), qPrintable(conn.value("error").toString()));

        const QJsonObject st = actor.executeTool("get_flow_state", QJsonObject());
        const QJsonArray conns = st.value("connections").toArray();
        QCOMPARE(conns.size(), 1);
        const QJsonObject c0 = conns.first().toObject();
        QVERIFY(c0.contains("from"));
        QVERIFY(c0.contains("to"));
        QCOMPARE(c0.value("fromPort").toString(), QStringLiteral("fit_points"));
        QCOMPARE(c0.value("toPort").toString(), QStringLiteral("fit_points"));
        QCOMPARE(c0.value("edgeType").toString(), QStringLiteral("data"));
    }

    // F04（阶段 2 修复）：Agent 点对可运行，旧编号键通过工程加载迁移；退化点原子拒绝。
    void testF04CalibrationPointPairsRunAndMigrate() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_calib"}}).contains("error"));
        QVERIFY(
            !actor.executeTool("add_module", QJsonObject{{"plugin", m_nameNPointCalibration}, {"instanceName", "cal"}})
                 .contains("error"));
        const QJsonObject calibrationSchema =
            actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", "cal"}});
        const QJsonObject parameters = calibrationSchema.value("parameters").toObject();
        for (const QString& key : {QStringLiteral("outputWidth"), QStringLiteral("outputHeight")}) {
            const QJsonObject field = parameters.value(key).toObject();
            QCOMPARE(field.value("min"), QJsonValue(0));
            QCOMPARE(field.value("max"), QJsonValue(16384));
            QCOMPARE(field.value("decimals"), QJsonValue(0));
        }
        const QJsonObject results = calibrationSchema.value("results").toObject();
        for (const QString& key :
             {QStringLiteral("calibration_valid"), QStringLiteral("reprojection_error"), QStringLiteral("point_count")})
            QVERIFY(!results.value(key).toObject().value("label").toString().isEmpty());
        QCOMPARE(results.value("reprojection_error").toObject().value("unit").toString(),
                 QStringLiteral("calibrationPoints.world 的调用者定义单位"));
        const QJsonArray points{QJsonObject{{"image", QJsonArray{0, 0}}, {"world", QJsonArray{5, 7}}},
                                QJsonObject{{"image", QJsonArray{10, 0}}, {"world", QJsonArray{15, 7}}},
                                QJsonObject{{"image", QJsonArray{10, 10}}, {"world", QJsonArray{15, 17}}},
                                QJsonObject{{"image", QJsonArray{0, 10}}, {"world", QJsonArray{5, 17}}}};
        const QJsonObject valid =
            actor.executeTool("set_module_params", QJsonObject{{"instanceId", "cal"},
                                                               {"values", QJsonObject{{"calibrationPoints", points}}}});
        QCOMPARE(valid.value("status").toString(), QStringLiteral("params_set"));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        QCOMPARE(project->moduleById("cal")->params.value("calibrationPoints"), QJsonValue(points));
        const int undoBefore = actor.undoStack()->count();

        const QJsonArray collinear{QJsonObject{{"image", QJsonArray{0, 0}}, {"world", QJsonArray{0, 0}}},
                                   QJsonObject{{"image", QJsonArray{1, 1}}, {"world", QJsonArray{1, 1}}},
                                   QJsonObject{{"image", QJsonArray{2, 2}}, {"world", QJsonArray{2, 2}}},
                                   QJsonObject{{"image", QJsonArray{3, 3}}, {"world", QJsonArray{3, 3}}}};
        const QJsonObject rejected = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", "cal"}, {"values", QJsonObject{{"calibrationPoints", collinear}}}});
        QCOMPARE(rejected.value("errorCode").toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(project->moduleById("cal")->params.value("calibrationPoints"), QJsonValue(points));
        QCOMPARE(actor.undoStack()->count(), undoBefore);
        QJsonArray duplicate = points;
        duplicate[3] = points[0];
        const QJsonObject duplicateRejected = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", "cal"}, {"values", QJsonObject{{"calibrationPoints", duplicate}}}});
        QCOMPARE(duplicateRejected.value("errorCode").toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(project->moduleById("cal")->params.value("calibrationPoints"), QJsonValue(points));
        QCOMPARE(actor.undoStack()->count(), undoBefore);
        actor.undoStack()->undo();
        QVERIFY(project->moduleById("cal")->params.isEmpty());
        actor.undoStack()->redo();
        QCOMPARE(project->moduleById("cal")->params.value("calibrationPoints"), QJsonValue(points));

        const QString path = m_appDataDir.filePath("g2_calibration.dproj");
        QVERIFY(project->save(path));
        Project reopened;
        QVERIFY(reopened.load(path));
        QCOMPARE(reopened.moduleById("cal")->params.value("calibrationPoints"), QJsonValue(points));

        PluginManager::ModulePtr module = PluginManager::instance().createManagedModule(m_nameNPointCalibration);
        QVERIFY(module);
        module->setParams(valid.value("effectiveParams").toObject());
        QVERIFY(module->initialize());
        QImage image(32, 32, QImage::Format_RGB32);
        image.fill(Qt::black);
        ImageData input(image), output;
        ModuleBase* base = dynamic_cast<ModuleBase*>(module.get());
        QVERIFY(base);
        QVERIFY(base->execute(input, output));
        QVERIFY(output.data("calibration_valid").toBool());
        QCOMPARE(output.data("point_count").toInt(), 4);
        QVERIFY(output.data("reprojection_error").toDouble() < 0.001);
        std::unique_ptr<QWidget> config(module->createConfigWidget());
        QTableWidget* table = config->findChild<QTableWidget*>(QStringLiteral("CalibrationPointsTable"));
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 4);
        table->item(0, 0)->setText(QStringLiteral("2"));
        QCOMPARE(module->currentParams().value("calibrationPoints").toArray()[0].toObject().value("image").toArray()[0],
                 QJsonValue(2.0));

        ModuleInstance legacy;
        legacy.id = QStringLiteral("legacy");
        legacy.moduleId = m_nameNPointCalibration;
        legacy.params.insert("pointCount", 4);
        for (int i = 0; i < points.size(); ++i) {
            const QJsonObject pair = points[i].toObject();
            const QJsonArray imagePoint = pair.value("image").toArray();
            const QJsonArray worldPoint = pair.value("world").toArray();
            legacy.params.insert(QString("imgX_%1").arg(i), imagePoint[0]);
            legacy.params.insert(QString("imgY_%1").arg(i), imagePoint[1]);
            legacy.params.insert(QString("worldX_%1").arg(i), worldPoint[0]);
            legacy.params.insert(QString("worldY_%1").arg(i), worldPoint[1]);
        }
        Project legacyProject;
        legacyProject.addModule(legacy);
        const QString legacyPath = m_appDataDir.filePath("g2_calibration_legacy.dproj");
        QVERIFY(legacyProject.save(legacyPath));
        Project migratedProject;
        QVERIFY(migratedProject.load(legacyPath));
        const ModuleInstance migrated = *migratedProject.moduleById("legacy");
        QCOMPARE(migrated.params.value("calibrationPoints"), QJsonValue(points));
        QVERIFY(!migrated.params.contains("pointCount"));

        ModuleInstance malformed = legacy;
        malformed.params.remove(QStringLiteral("worldY_3"));
        const ModuleInstance parsed = ModuleInstance::fromJson(malformed.toJson());
        QCOMPARE(parsed.params, malformed.params); // 解析失败不能吞掉旧坐标
        Project malformedProject;
        malformedProject.addModule(malformed);
        const QString malformedPath = m_appDataDir.filePath("g2_calibration_malformed.dproj");
        QVERIFY(malformedProject.save(malformedPath));
        Project rejectedProject;
        QVERIFY(!rejectedProject.load(malformedPath));
    }

    void testG2PlcRegisterValuesValidateWithoutConnecting() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_plc_values"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_namePlcWrite}, {"instanceName", "plc"}})
                     .contains("error"));
        const QJsonObject valid = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", "plc"},
                        {"values", QJsonObject{{"startAddress", 65534}, {"values", QJsonArray{0, 65535}}}}});
        QCOMPARE(valid.value("status").toString(), QStringLiteral("params_set"));
        const QJsonObject before = ProjectManager::instance().currentProject()->moduleById("plc")->params;
        const int undoBefore = actor.undoStack()->count();

        const QJsonObject invalid = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", "plc"},
                        {"values", QJsonObject{{"startAddress", 65535}, {"values", QJsonArray{1, 2}}}}});
        QCOMPARE(invalid.value("errorCode").toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(ProjectManager::instance().currentProject()->moduleById("plc")->params, before);
        QCOMPARE(actor.undoStack()->count(), undoBefore);

        const QJsonObject variable = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", "plc"},
                        {"values", QJsonObject{{"valueSource", "variable"}, {"values", "register_buffer"}}}});
        QCOMPARE(variable.value("status").toString(), QStringLiteral("params_set"));
        QCOMPARE(ProjectManager::instance().currentProject()->moduleById("plc")->params.value("values"),
                 QJsonValue(QStringLiteral("register_buffer")));
        const QJsonObject afterVariable = ProjectManager::instance().currentProject()->moduleById("plc")->params;
        const int undoAfterVariable = actor.undoStack()->count();
        const QJsonObject invalidSource = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", "plc"},
                        {"values", QJsonObject{{"valueSource", "immediate"}, {"values", "register_buffer"}}}});
        QCOMPARE(invalidSource.value("errorCode").toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(ProjectManager::instance().currentProject()->moduleById("plc")->params, afterVariable);
        QCOMPARE(actor.undoStack()->count(), undoAfterVariable);
    }

    void testG2NoOpParameterAuditIsNotUndoable() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_noop_audit"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameDelay}, {"instanceName", "delay"}})
                     .contains("error"));
        auto& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject result = controller.handleToolCall(
            "set_module_params", QJsonObject{{"instanceId", "delay"}, {"values", QJsonObject()}});
        QCOMPARE(result.value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(logSpy.count(), 1);
        const AgentActionLogEntry entry = logSpy.at(0).at(0).value<AgentActionLogEntry>();
        QVERIFY(!entry.undoable);
    }

    // F05（Slice 1/2/12/14/15/16/26 已部分收口）：目录可查，受支持的相机/插件操作显式开放
    void testF05PluginAndCameraCatalogWithoutLifecycleTools() {
        for (const char* name : {"camera_capabilities", "set_camera_param", "grab_frame", "load_plugin",
                                 "unload_plugin", "reload_plugin", "plugin_status"})
            QVERIFY2(ToolSchema::instance().findTool(name).name.isEmpty(), name);
        for (const char* name : {"list_cameras", "get_camera_details", "connect_camera", "disconnect_camera",
                                 "set_camera_parameter", "load_module_plugin", "unload_module_plugin"})
            QVERIFY2(!ToolSchema::instance().findTool(name).name.isEmpty(), name);
        QVERIFY(!ToolSchema::instance().findTool("get_camera_discovery_targets").dangerous);
        QVERIFY(ToolSchema::instance().findTool("discover_cameras").dangerous);
        QVERIFY(!ToolSchema::instance().findTool("get_camera_discovery_operation").dangerous);

        AgentActor actor;
        const QJsonObject r = actor.executeTool("get_available_plugins", QJsonObject());
        QVERIFY(r.contains("plugins"));
        QVERIFY(r.value("items").isArray());
        QVERIFY(r.value("cameras").isArray());
        QVERIFY(!r.contains("loadStatus")); // 结构化状态位于每项中
        for (const QJsonValue& value : r.value("cameras").toArray())
            QVERIFY(value.toObject().contains(QStringLiteral("platformCompatibility")));
    }

    void testBuiltinDocsStaySourcedAndAuditArtifactsStayExcluded() {
        AgentActor actor;
        const QJsonObject listed = actor.executeTool(QStringLiteral("list_knowledge_documents"), QJsonObject());
        QVERIFY2(!listed.contains(QStringLiteral("error")),
                 qPrintable(listed.value(QStringLiteral("error")).toString()));

        QStringList documentIds;
        QStringList builtInNames;
        for (const QJsonValue& value : listed.value(QStringLiteral("documents")).toArray()) {
            const QJsonObject document = value.toObject();
            documentIds.append(document.value(QStringLiteral("documentId")).toString());
            if (document.value(QStringLiteral("source")).toString() == QStringLiteral("builtin"))
                builtInNames.append(document.value(QStringLiteral("name")).toString());
        }
        QCOMPARE(listed.value(QStringLiteral("total")).toInt(), 3);
        documentIds.sort();
        QCOMPARE(documentIds, QStringList({QStringLiteral("builtin:DEEPLUX.md"), QStringLiteral("builtin:PLUGINS.md"),
                                           QStringLiteral("builtin:USER_MANUAL.md")}));
        builtInNames.sort();
        QCOMPARE(builtInNames, QStringList({QStringLiteral("DEEPLUX.md"), QStringLiteral("PLUGINS.md"),
                                            QStringLiteral("USER_MANUAL.md")}));

        const QJsonObject manual = actor.executeTool(
            QStringLiteral("read_documentation"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("copy_data_source_path_to_clipboard")}});
        QVERIFY2(!manual.contains(QStringLiteral("error")),
                 qPrintable(manual.value(QStringLiteral("error")).toString()));
        QVERIFY(manual.value(QStringLiteral("content")).toString().contains(QStringLiteral("[来源: USER_MANUAL.md]")));

        const QJsonObject defectDetection = actor.executeTool(
            QStringLiteral("read_documentation"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("DefectDetection")}, {QStringLiteral("limit"), 50}});
        QVERIFY2(!defectDetection.contains(QStringLiteral("error")),
                 qPrintable(defectDetection.value(QStringLiteral("error")).toString()));
        const QString content = defectDetection.value(QStringLiteral("content")).toString();
        for (const QString& source :
             {QStringLiteral("DEEPLUX.md"), QStringLiteral("PLUGINS.md"), QStringLiteral("USER_MANUAL.md")})
            QVERIFY2(content.contains(QStringLiteral("[来源: %1]").arg(source)), qPrintable(source));
        QVERIFY(content.contains(QStringLiteral("HymsonVision3D SDK 未启用，缺陷检测不可用")));

        for (const char* shellTool :
             {"bash", "execute_shell", "run_shell_command", "run_terminal_command", "terminal_input", "write_terminal"})
            QVERIFY2(ToolSchema::instance().findTool(QString::fromLatin1(shellTool)).name.isEmpty(), shellTool);

        const auto normalized = [](QString value) {
            value = value.toLower();
            value.remove(QLatin1Char('_'));
            value.remove(QLatin1Char('-'));
            value.remove(QLatin1Char(' '));
            return value;
        };
        const QStringList executionVerbs = {QStringLiteral("exec"),   QStringLiteral("execute"), QStringLiteral("run"),
                                            QStringLiteral("launch"), QStringLiteral("start"),   QStringLiteral("eval"),
                                            QStringLiteral("invoke"), QStringLiteral("spawn")};
        const QStringList runnerObjects = {
            QStringLiteral("shell"),  QStringLiteral("bash"),       QStringLiteral("terminal"),
            QStringLiteral("python"), QStringLiteral("javascript"), QStringLiteral("js"),
            QStringLiteral("script"), QStringLiteral("command"),    QStringLiteral("process"),
            QStringLiteral("code"),   QStringLiteral("executable")};
        const QStringList payloadNames = {
            QStringLiteral("code"),           QStringLiteral("sourcecode"),   QStringLiteral("pythoncode"),
            QStringLiteral("javascriptcode"), QStringLiteral("script"),       QStringLiteral("scriptbody"),
            QStringLiteral("shell"),          QStringLiteral("shellcommand"), QStringLiteral("command"),
            QStringLiteral("commandline"),    QStringLiteral("cmd"),          QStringLiteral("executable"),
            QStringLiteral("program"),        QStringLiteral("source")};
        const QStringList executionPhrases = {QStringLiteral("execute arbitrary code"),
                                              QStringLiteral("run arbitrary code"),
                                              QStringLiteral("run arbitrary script"),
                                              QStringLiteral("execute shell"),
                                              QStringLiteral("run shell"),
                                              QStringLiteral("run command"),
                                              QStringLiteral("execute command"),
                                              QStringLiteral("launch process"),
                                              QStringLiteral("start process"),
                                              QStringLiteral("spawn process")};
        const auto isExecutionCapable = [&](const ToolDefinition& tool) {
            const QString toolName = normalized(tool.name);
            QStringList textSurfaces{tool.description.toLower()};
            bool hasPayload = false;
            for (const ToolParam& param : tool.params) {
                hasPayload = hasPayload || payloadNames.contains(normalized(param.name));
                textSurfaces.append(param.description.toLower());
            }
            const QString descriptionText = textSurfaces.join(QLatin1Char(' '));
            const QString normalizedDescription = normalized(descriptionText);
            const bool hasVerb = std::any_of(executionVerbs.cbegin(), executionVerbs.cend(),
                                             [&toolName](const QString& token) { return toolName.contains(token); });
            const bool hasVerbInDescription = std::any_of(
                executionVerbs.cbegin(), executionVerbs.cend(),
                [&normalizedDescription](const QString& token) { return normalizedDescription.contains(token); });
            const bool hasRunnerObject =
                std::any_of(runnerObjects.cbegin(), runnerObjects.cend(),
                            [&toolName](const QString& token) { return toolName.contains(token); });
            const bool hasExecutionPhrase =
                std::any_of(executionPhrases.cbegin(), executionPhrases.cend(),
                            [&descriptionText](const QString& phrase) { return descriptionText.contains(phrase); });
            if (toolName == QLatin1String("selectlogpaneltab"))
                return hasPayload || hasExecutionPhrase;
            return (hasVerb && hasRunnerObject) || hasExecutionPhrase ||
                   (hasPayload && (hasVerb || hasVerbInDescription));
        };

        for (const ToolDefinition& tool : ToolSchema::instance().allTools())
            QVERIFY2(!isExecutionCapable(tool), qPrintable(tool.name));

        QVERIFY(isExecutionCapable(
            ToolDefinition{QStringLiteral("execute_python"),
                           QString(),
                           {ToolParam(QStringLiteral("code"), QStringLiteral("string"), QString())}}));
        QVERIFY(isExecutionCapable(
            ToolDefinition{QStringLiteral("python_exec"),
                           QString(),
                           {ToolParam(QStringLiteral("script"), QStringLiteral("string"), QString())}}));
        QVERIFY(isExecutionCapable(
            ToolDefinition{QStringLiteral("run_command"),
                           QString(),
                           {ToolParam(QStringLiteral("command"), QStringLiteral("string"), QString())}}));
        QVERIFY(isExecutionCapable(ToolDefinition{
            QStringLiteral("value_summary"),
            QStringLiteral("Runs the supplied code in Python."),
            {ToolParam(QStringLiteral("code"), QStringLiteral("string"), QStringLiteral("Source text"))}}));
        QVERIFY(isExecutionCapable(ToolDefinition{QStringLiteral("value_summary"),
                                                  QStringLiteral("Returns a summary of the supplied value."),
                                                  {ToolParam(QStringLiteral("code"), QStringLiteral("string"),
                                                             QStringLiteral("Run this code as Python."))}}));

        const ToolDefinition terminalTab = ToolSchema::instance().findTool(QStringLiteral("select_log_panel_tab"));
        QVERIFY(!terminalTab.name.isEmpty());
        QVERIFY(terminalTab.description.contains(QStringLiteral("terminal"), Qt::CaseInsensitive));
        QVERIFY(terminalTab.params.constFirst().enumValues.contains(QStringLiteral("terminal")));
        QVERIFY(!isExecutionCapable(terminalTab));
        ToolDefinition terminalTabWithPayload = terminalTab;
        terminalTabWithPayload.params.append(
            ToolParam(QStringLiteral("script"), QStringLiteral("string"), QStringLiteral("Executable script")));
        QVERIFY(isExecutionCapable(terminalTabWithPayload));
        ToolDefinition terminalTabWithLaunch = terminalTab;
        terminalTabWithLaunch.description += QStringLiteral(" Launch process to run a script.");
        QVERIFY(isExecutionCapable(terminalTabWithLaunch));
    }

    // F06（阶段 1 已收口——断言自基线缺陷翻转为修复后行为）：
    // 保存失败携带 error + errorCode=SAVE_FAILED + retryable；保留 status:failed 兼容
    void testF06FixedSaveFailureCarriesErrorCode() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_save"}}).contains("error"));
        const QJsonObject r = actor.executeTool("save_project", QJsonObject()); // 未指定路径
        QCOMPARE(r.value("status").toString(), QStringLiteral("failed"));       // 兼容字段保留
        QVERIFY2(r.contains("error"), "phase-1 fix: failure must carry error");
        QCOMPARE(r.value("errorCode").toString(), QStringLiteral("SAVE_FAILED"));
        QVERIFY(r.contains("retryable"));
        QVERIFY(!r.value("retryable").toBool());
    }

    // F06 端到端（阶段 1 已收口——翻转）：经控制器确认链路执行失败的保存，
    // 审计日志记录 error（不再是假 success），且 undoable=false（保存不可撤销，§6）
    void testF06FixedControllerLogsFailedSaveAsError() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_save_e2e"}}).contains("error"));

        auto& ctrl = AgentController::instance();
        ctrl.clearConversation();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);
        QSignalSpy confirmSpy(&ctrl, &AgentController::toolsPendingConfirmation);

        // save_project 为 dangerous → Autopilot 进入确认；确认载荷含工程/摘要/副作用（§4）
        const QJsonObject r = ctrl.handleToolCall("save_project", QJsonObject()); // 无路径 → 底层必然失败
        QCOMPARE(r.value("status").toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(confirmSpy.count(), 1);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Confirming);
        const QJsonObject payload = ctrl.pendingConfirmationPayload();
        QVERIFY(!payload.value("confirmationId").toString().isEmpty());
        QVERIFY(payload.value("context").toObject().contains("summary"));
        QVERIFY(payload.value("context").toObject().contains("projectVersion"));

        QVERIFY(ctrl.confirmPendingTools(ctrl.pendingConfirmationId())); // 确认执行排入 GUI 事件队列
        QTRY_COMPARE_WITH_TIMEOUT(logSpy.count(), 1, 5000);
        const AgentActionLogEntry entry = logSpy.at(0).at(0).value<AgentActionLogEntry>();
        QCOMPARE(entry.action, QStringLiteral("save_project"));
        // 修复后：失败被如实记录为 error（不再是假 success）
        QCOMPARE(entry.result, QStringLiteral("error"));
        // 修复后：保存不再被标记为可撤销（外部持久化动作，§6）
        QVERIFY(!entry.undoable);
        // 实际未保存：工程仍无文件路径
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);
        QVERIFY(ProjectManager::instance().currentProject()->filePath().isEmpty());
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    // F07（阶段 1 已收口——翻转）：打开损坏工程失败后，原工程对象/修改标记/撤销历史保持可用
    void testF07FixedOpenCorruptProjectKeepsCurrent() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "keeper"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameGrabImage}, {"instanceName", "grab"}})
                     .contains("error"));
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);

        const QString broken = QDir(m_appDataDir.path()).filePath("broken.dproj");
        {
            QFile f(broken);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{ this is not valid json ");
        }

        // keeper 已修改（加过模块）→ 需显式 discard 越过阶段1未保存守卫,聚焦 F07 本体
        const QJsonObject r =
            actor.executeTool("open_project", QJsonObject{{"path", broken}, {"onUnsaved", "discard"}});
        QVERIFY(r.contains("error")); // 打开失败被正确报告
        QCOMPARE(r.value("errorCode").toString(), QStringLiteral("OPEN_FAILED"));

        // 阶段1 §3（F07 修复——翻转）：先验证候选后替换——原工程、模块与撤销历史全部保留
        const QJsonObject st = actor.executeTool("get_flow_state", QJsonObject());
        QVERIFY2(!st.contains("error"), "phase-1 fix: original project must survive a failed open");
        QCOMPARE(st.value("modules").toArray().size(), 1);
        QVERIFY(actor.undoStack()->canUndo()); // 撤销历史可用
    }

    // F08（阶段 1 已收口——翻转）：聊天与 Bridge 路径共用 evaluateToolGate——
    // Observer 的只读工具在两条路径都执行并继续问答；写操作在两条路径都被拒绝
    void testF08FixedObserverChatExecutesReadOnlyLikeBridge() {
        auto& ctrl = AgentController::instance();
        ctrl.clearConversation(); // 内部 transitionTo(Idle)
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Observer);
        auto* mock = new MockLLMClient(&ctrl);
        ILLMClient* old = ctrl.llmClient();
        ctrl.setLLMClient(mock);

        QSignalSpy actionSpy(&ctrl, &AgentController::agentActionCompleted);
        ctrl.sendUserMessage(QStringLiteral("查询手册：workflow 的用法"));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);

        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("让我查一下手册");
        resp.toolCalls.append(makeToolCallJson("read_documentation", QJsonObject{{"topic", "workflow"}}));
        mock->emitResponse(resp);

        // 聊天路径（修复后）：只读工具被执行，结果回传后闭环继续（再次请求 LLM → Thinking）
        QVERIFY2(actionSpy.count() >= 1, "phase-1 fix: Observer chat path must execute read-only tools");
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        // 工具结果进入对话上下文（tool 角色消息）
        bool sawToolMessage = false;
        for (const AgentMessage& m : mock->lastCtx.messages)
            if (m.role == QLatin1String("tool"))
                sawToolMessage = true;
        QVERIFY(sawToolMessage);
        ctrl.clearConversation(); // 回到 Idle 以便走 Bridge 路径

        // Bridge 路径：同一权限级别下只读工具同样执行——两入口策略一致
        const QJsonObject bridge = ctrl.handleToolCall("read_documentation", QJsonObject{{"topic", "workflow"}});
        QVERIFY2(!bridge.contains("error"), qPrintable(bridge.value("error").toString()));
        QVERIFY(bridge.contains("content"));

        // 写操作在 Bridge 路径被拒绝且带结构化错误码（聊天路径拒绝由 test_agentcontroller 覆盖）
        const QJsonObject denied = ctrl.handleToolCall("add_module", QJsonObject{{"plugin", "GrabImage"}});
        QCOMPARE(denied.value("errorCode").toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(denied.value("error").toString().contains(QStringLiteral("Observer")));

        ctrl.setLLMClient(old);
        delete mock;
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    // F09（收口阶段 4/6）：单步/重置/运行前校验已接入，其他界面业务操作仍缺失
    void testF09WhitelistMissingUiOperations() {
        for (const char* name :
             {"step", "toggle_breakpoint", "get_variables", "set_variable", "create_variable", "reorder_module"})
            QVERIFY2(ToolSchema::instance().findTool(name).name.isEmpty(), name);
        QVERIFY(!ToolSchema::instance().findTool("move_module").name.isEmpty());
        QVERIFY(!ToolSchema::instance().findTool("rename_module").name.isEmpty());
        QVERIFY(!ToolSchema::instance().findTool("set_module_enabled").name.isEmpty());
        QVERIFY(!ToolSchema::instance().findTool("set_breakpoint").name.isEmpty());
        QVERIFY(!ToolSchema::instance().findTool("set_module_note").name.isEmpty());
        QVERIFY(!ToolSchema::instance().findTool("step_once").name.isEmpty());
        QVERIFY(!ToolSchema::instance().findTool("reset_step").name.isEmpty());
        QVERIFY(!ToolSchema::instance().findTool("validate_flow").name.isEmpty());
        // 工具目录由 test_toolschema 锁定；本用例只复现 F09 的缺失操作映射。
    }

    // F10（阶段 1 已收口——翻转）：未知实例失败且不产生撤销记录；
    // 非法状态的 pause/resume/stop 返回显式幂等 no_op，不谎称完成动作
    void testF10FixedUnknownInstanceFailsAndIdleControlIsNoOp() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_noop"}}).contains("error"));

        const int undoBefore = actor.undoStack()->count();
        const QJsonObject rm = actor.executeTool("remove_module", QJsonObject{{"instanceId", "does_not_exist"}});
        QVERIFY2(rm.contains("error"), "phase-1 fix: unknown instance must fail");
        QCOMPARE(rm.value("errorCode").toString(), QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(actor.undoStack()->count(), undoBefore); // 不再压入空撤销命令

        const QJsonObject p = actor.executeTool("pause_flow", QJsonObject()); // 空闲时暂停
        QVERIFY(!p.contains("error"));
        QCOMPARE(p.value("status").toString(), QStringLiteral("no_op"));
        QVERIFY(!p.value("reason").toString().isEmpty());
        const QJsonObject rs = actor.executeTool("resume_flow", QJsonObject()); // 空闲时继续
        QCOMPARE(rs.value("status").toString(), QStringLiteral("no_op"));
        const QJsonObject st = actor.executeTool("stop_flow", QJsonObject()); // 空闲时停止
        QCOMPARE(st.value("status").toString(), QStringLiteral("no_op"));
    }

    // F13 底层事实（阶段 1 后保留为架构特征记录）：Actor 是低层执行器，本身不含
    // 权限门禁——确认/拒绝统一在控制器 evaluateToolGate（端到端行为见
    // testF13FixedAutopilotRunFlowRequiresConfirmation）。本用例锁定两个底层事实：
    // run_flow 在 Schema 中仍非 dangerous（保守确认由控制器执行入口口径实现，
    // 阶段 5 再按流程实际副作用精细化）；Folder(Delete) 确会递归删除目录。
    // 仅使用本测试创建的临时目录，不触碰任何用户数据。
    void testF13RunFlowFolderDeleteNoConfirmation() {
        // run_flow 在 Schema 中非 dangerous（Autopilot 的确认由控制器执行入口口径保证）
        QVERIFY(!ToolSchema::instance().findTool("run_flow").dangerous);
        // 对照：Folder(Delete) 会递归删除目录（FolderPlugin.cpp:55-57 removeRecursively）
        QVERIFY(ToolSchema::instance().findTool("sam_annotate").dangerous); // 直接写盘工具已标记

        QTemporaryDir victimRoot;
        QVERIFY(victimRoot.isValid());
        const QString victim = QDir(victimRoot.path()).filePath("victim_dir");
        QVERIFY(QDir().mkpath(victim));
        {
            QFile f(QDir(victim).filePath("important.txt"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("data");
        }
        QVERIFY(QFileInfo::exists(victim));

        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_side"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameFolder}, {"instanceName", "del"}})
                     .contains("error"));
        // 顺序依赖：Delete+空路径的中间态被校验拒绝（FolderPlugin doValidateParams），
        // 必须先设 folderPath 再设 operation——单参数逐步提交的半配置约束（计划 §7.7 阶段2 原子化）
        QVERIFY(
            !actor
                 .executeTool("set_param", QJsonObject{{"instanceId", "del"}, {"key", "folderPath"}, {"value", victim}})
                 .contains("error"));
        const QJsonObject opSet = actor.executeTool(
            "set_param", QJsonObject{{"instanceId", "del"}, {"key", "operation"}, {"value", "Delete"}});
        QVERIFY2(!opSet.contains("error"), qPrintable(opSet.value("error").toString()));

        const QJsonObject run = actor.executeTool("run_flow", QJsonObject());
        QVERIFY2(!run.contains("error"), qPrintable(run.value("error").toString()));
        QVERIFY2(waitForRunIdle(actor).isEmpty(), "folder flow should finish");

        // 基线缺陷：文件删除副作用已发生，全程无任何确认门槛
        QVERIFY2(!QFileInfo::exists(victim), "baseline defect: victim dir must have been deleted without confirmation");
    }

    // F13 端到端（阶段 1 已收口——翻转）：run_flow 属执行入口，Autopilot 也必须确认（§6.5
    // 保守口径）；确认前零副作用，拒绝零副作用且留审计，批准后才执行删除
    void testF13FixedAutopilotRunFlowRequiresConfirmation() {
        QTemporaryDir victimRoot;
        QVERIFY(victimRoot.isValid());
        const QString victim = QDir(victimRoot.path()).filePath("victim_autopilot");
        QVERIFY(QDir().mkpath(victim));
        {
            QFile f(QDir(victim).filePath("important.txt"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("data");
        }

        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "repro_side_e2e"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", m_nameFolder}, {"instanceName", "del"}})
                     .contains("error"));
        QVERIFY(
            !actor
                 .executeTool("set_param", QJsonObject{{"instanceId", "del"}, {"key", "folderPath"}, {"value", victim}})
                 .contains("error"));
        QVERIFY(!actor
                     .executeTool("set_param",
                                  QJsonObject{{"instanceId", "del"}, {"key", "operation"}, {"value", "Delete"}})
                     .contains("error"));

        auto& ctrl = AgentController::instance();
        ctrl.clearConversation();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        auto* mock = new MockLLMClient(&ctrl);
        ILLMClient* old = ctrl.llmClient();
        ctrl.setLLMClient(mock);
        QSignalSpy confirmSpy(&ctrl, &AgentController::toolsPendingConfirmation);
        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);

        ctrl.sendUserMessage(QStringLiteral("运行流程清理临时目录"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(makeToolCallJson("run_flow", QJsonObject()));
        mock->emitResponse(resp);

        // 修复后：run_flow 虽未标 dangerous，执行入口按保守口径进入确认；确认前零副作用
        QCOMPARE(confirmSpy.count(), 1);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Confirming);
        QVERIFY2(QFileInfo::exists(victim), "no side effect before confirmation");

        // 拒绝：零副作用 + 每个待确认 call 一条 rejected 审计（§4 可追踪）
        ctrl.rejectPendingTools(ctrl.pendingConfirmationId());
        QVERIFY2(QFileInfo::exists(victim), "rejection must have zero side effects");
        bool sawRejected = false;
        for (const auto& args : logSpy) {
            const AgentActionLogEntry e = args.at(0).value<AgentActionLogEntry>();
            if (e.action == QLatin1String("run_flow") && e.result == QLatin1String("rejected"))
                sawRejected = true;
        }
        QVERIFY(sawRejected);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);

        // 批准路径（Bridge 入口，Autopilot 同样先确认）：确认后才发生删除
        const QJsonObject pending = ctrl.handleToolCall("run_flow", QJsonObject());
        QCOMPARE(pending.value("status").toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject payload = ctrl.pendingConfirmationPayload();
        QVERIFY(payload.value("context").toObject().value("sideEffects").toArray().size() >= 1);
        ctrl.confirmPendingTools(ctrl.pendingConfirmationId());

        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(victim), 5000);
        QVERIFY2(waitForRunIdle(actor).isEmpty(), "flow should finish");
        QVERIFY2(!QFileInfo::exists(victim), "approved run must execute the deletion");

        // 审计日志将批准执行的 run_flow 记为 success，且 undoable=false（执行动作不可撤销）
        bool sawRunFlowSuccess = false;
        for (const auto& args : logSpy) {
            const AgentActionLogEntry e = args.at(0).value<AgentActionLogEntry>();
            if (e.action == QLatin1String("run_flow") && e.result == QLatin1String("success")) {
                sawRunFlowSuccess = true;
                QVERIFY(!e.undoable);
            }
        }
        QVERIFY(sawRunFlowSuccess);

        ctrl.setLLMClient(old);
        delete mock;
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    // G1（阶段 1）：含 PLCWrite 的流程在确认前零执行，模拟 PLC 无连接；拒绝后同样零副作用。
    void testG1PlcFlowZeroSideEffectBeforeConfirmation() {
        QTcpServer plc;
        QVERIFY(plc.listen(QHostAddress::LocalHost));
        QSignalSpy connectionSpy(&plc, &QTcpServer::newConnection);
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "plc_gate"}}).contains("error"));
        const QJsonObject added =
            actor.executeTool("add_module", QJsonObject{{"plugin", m_namePlcWrite}, {"instanceName", "plc"}});
        QVERIFY(!added.contains("error"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        QVERIFY(!instanceId.isEmpty());
        QVERIFY(!actor
                     .executeTool("set_param",
                                  QJsonObject{{"instanceId", instanceId}, {"key", "ipAddress"}, {"value", "127.0.0.1"}})
                     .contains("error"));
        QVERIFY(!actor
                     .executeTool("set_param", QJsonObject{{"instanceId", instanceId},
                                                           {"key", "port"},
                                                           {"value", static_cast<int>(plc.serverPort())}})
                     .contains("error"));

        const int runsBefore =
            actor.executeTool("get_run_results", QJsonObject()).value(QStringLiteral("totalRuns")).toInt();

        auto& ctrl = AgentController::instance();
        ctrl.clearConversation();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        auto* mock = new MockLLMClient(&ctrl);
        ILLMClient* old = ctrl.llmClient();
        ctrl.setLLMClient(mock);
        QSignalSpy confirmSpy(&ctrl, &AgentController::toolsPendingConfirmation);

        ctrl.sendUserMessage(QStringLiteral("运行含 PLC 写入的流程"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(makeToolCallJson("run_flow", QJsonObject()));
        mock->emitResponse(resp);

        // 确认前：进入 Confirming、引擎完全未启动、运行计数零变化（= 零设备副作用）
        QCOMPARE(confirmSpy.count(), 1);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Confirming);
        const QJsonArray effects = ctrl.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray();
        bool showsPlcTarget = false;
        for (const QJsonValue& effect : effects) {
            const QString description = effect.toString();
            if (description.contains(QStringLiteral("plc")) &&
                description.contains(QStringLiteral("127.0.0.1:%1").arg(plc.serverPort())))
                showsPlcTarget = true;
        }
        QVERIFY2(showsPlcTarget, "confirmation must show the effective PLC write target");
        QVERIFY(!RunEngine::instance().isRunning());
        QVERIFY(!RunEngine::instance().isBusy());
        QCOMPARE(connectionSpy.count(), 0);
        QVERIFY(!plc.hasPendingConnections());
        QCOMPARE(actor.executeTool("get_run_results", QJsonObject()).value(QStringLiteral("totalRuns")).toInt(),
                 runsBefore);

        // 拒绝：仍零执行
        ctrl.rejectPendingTools(ctrl.pendingConfirmationId());
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QVERIFY(!RunEngine::instance().isRunning());
        QCOMPARE(connectionSpy.count(), 0);
        QVERIFY(!plc.hasPendingConnections());
        QCOMPARE(actor.executeTool("get_run_results", QJsonObject()).value(QStringLiteral("totalRuns")).toInt(),
                 runsBefore);

        ctrl.setLLMClient(old);
        delete mock;
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }
};

QTEST_MAIN(TestAgentPhase0Repro)
#include "test_agentphase0_repro.moc"
