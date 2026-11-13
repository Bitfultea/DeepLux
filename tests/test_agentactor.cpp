#define DEEPLUX_BUILD_TESTS 1
#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/AgentRunWaiter.h"
#include "core/agent/AgentToolImage.h"
#include "core/agent/ToolSchema.h"
#include "core/base/ModuleBase.h"
#include "core/common/Logger.h"
#include "core/common/UserManager.h"
#include "core/device/CameraManager.h"
#include "core/display/DisplayData.h"
#include "core/engine/FlowExecutionService.h"
#include "core/engine/RunEngine.h"
#include "core/model/Project.h"
#include "manager/GlobalVarManager.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"
#include "ui/widgets/FlowCanvas.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QRect>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <atomic>
#include <limits>
#include <thread>

using namespace DeepLux;

namespace {
/// L2：只负责产出一个引擎不支持序列化的自定义端口类型（QRect）
class UnsupportedPortModule final : public ModuleBase {
public:
    UnsupportedPortModule() {
        m_moduleId = QStringLiteral("com.deeplux.test.unsupportedport");
        m_name = QStringLiteral("UnsupportedPort");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("unsupported_port"));
    }

    PortValueMap customOutputs() const {
        PortValueMap outputs;
        outputs.insert(QStringLiteral("rect"), QVariant::fromValue(QRect(1, 2, 3, 4)));
        outputs.insert(QStringLiteral("count"), 7);
        return outputs;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class BlockingAgentModule final : public ModuleBase {
public:
    explicit BlockingAgentModule(int sleepMs = 900) : m_sleepMs(sleepMs) {
        m_moduleId = QStringLiteral("com.deeplux.test.agentblocking");
        m_name = QStringLiteral("AgentBlocking");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("agent_blocking"));
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        m_entered.store(true, std::memory_order_release);
        entered.release();
        QElapsedTimer timer;
        timer.start();
        while (!isCancellationRequested() && timer.elapsed() < m_sleepMs)
            QThread::msleep(5);
        output = input;
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

public:
    std::atomic_bool m_entered{false};
    QSemaphore entered; // 已进入执行的确定性屏障

private:
    int m_sleepMs = 900;
};

/// 计数探针：验证后台服务驱动的 Cycle/暂停恢复中各模块执行次数与线程。
class CountingAgentModule final : public ModuleBase {
public:
    explicit CountingAgentModule(const QString& name) {
        m_moduleId = QStringLiteral("com.deeplux.test.agentcount.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
        setInstanceName(name);
    }
    std::atomic<int> runs{0};
    std::atomic<Qt::HANDLE> executedOn{nullptr};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        executedOn.store(QThread::currentThreadId());
        ++runs;
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class NonCooperativeAgentModule final : public ModuleBase {
public:
    NonCooperativeAgentModule() {
        m_moduleId = QStringLiteral("com.deeplux.test.agentnoncooperative");
        m_name = QStringLiteral("AgentNonCooperative");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("agent_noncooperative"));
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        m_entered.store(true, std::memory_order_release);
        if (m_releaseGate)
            m_releaseGate->acquire();
        else
            QThread::msleep(350);
        output = input;
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

public:
    std::atomic_bool m_entered{false};
    QSemaphore* m_releaseGate = nullptr;
};

class UncertainOutcomeAgentModule final : public ModuleBase {
public:
    UncertainOutcomeAgentModule() {
        m_moduleId = QStringLiteral("com.deeplux.test.uncertainoutcome");
        m_name = QStringLiteral("UncertainOutcome");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("uncertain_outcome"));
    }

    ExecutionResult execute(const PortValueMap&, PortValueMap&, ExecutionContext&) override {
        return ExecutionResult::fail(ExecError::SideEffectUncertain,
                                     QStringLiteral("outcome cannot be confirmed after request dispatch"));
    }

protected:
    bool process(const ImageData&, ImageData&) override {
        return false;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class StepFailureAfterFileModule final : public ModuleBase {
public:
    explicit StepFailureAfterFileModule(const QString& outputPath) : m_outputPath(outputPath) {
        m_moduleId = QStringLiteral("com.deeplux.test.agentstepfailure");
        m_name = QStringLiteral("AgentStepFailure");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("step_failure"));
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        QFile file(m_outputPath);
        m_sideEffectWritten = file.open(QIODevice::WriteOnly) && file.write("side-effect") == 11;
        emit errorOccurred(QStringLiteral("intentional failure after side effect"));
        return false;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

public:
    bool sideEffectWritten() const {
        return m_sideEffectWritten;
    }

private:
    QString m_outputPath;
    bool m_sideEffectWritten = false;
};

class CoordinateSummaryAgentModule final : public ModuleBase {
public:
    CoordinateSummaryAgentModule() {
        m_moduleId = QStringLiteral("com.deeplux.test.coordinatesummary");
        m_name = QStringLiteral("CoordinateSummary");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("coordinate_summary"));
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        const QString sourceId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        const auto image = [](const QString& id) {
            ImageData nested(QImage(5, 4, QImage::Format_RGB888));
            ImageCoordinateProvenance provenance;
            provenance.sourceImageId = id;
            provenance.sourceSize = QSize(311, 227);
            provenance.hasLocalToSourceTransform = true;
            provenance.localToSource = {{1.5, 0.25, 11.0, -0.5, 2.0, 17.0, 0.0, 0.0, 1.0}};
            nested.setCoordinateProvenance(provenance);
            return nested;
        };

        output = image(sourceId);
        output.setData(QStringLiteral("nested_list"),
                       QVariantList{QVariant::fromValue(image(sourceId)),
                                    QVariant::fromValue(image(QStringLiteral("/private/source/image.png"))),
                                    QVariant::fromValue(image(QString(128, QLatin1Char('a'))))});
        output.setData(QStringLiteral("nested_map"),
                       QVariantMap{{QStringLiteral("child"), QVariant::fromValue(image(sourceId))}});
        ImageData withoutProvenance(QImage(5, 4, QImage::Format_RGB888));
        withoutProvenance.invalidateCoordinateProvenance();
        output.setData(QStringLiteral("without_provenance"), QVariant::fromValue(withoutProvenance));
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class StructuredCoordinatesAgentModule final : public ModuleBase {
public:
    explicit StructuredCoordinatesAgentModule(const ImageCoordinateProvenance& provenance) : m_provenance(provenance) {
        m_moduleId = QStringLiteral("com.deeplux.test.structuredcoordinates");
        m_name = QStringLiteral("StructuredCoordinates");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("structured_coordinates"));
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(16, 16, QImage::Format_RGB888));
        output.setCoordinateProvenance(m_provenance);
        Circle2D circle;
        circle.centerX = 3.0;
        circle.centerY = 2.5;
        circle.radius = 4.5;
        circle.score = 0.875;
        output.setData(QStringLiteral("circle"), QVariant::fromValue(circle));

        Region2D region;
        region.label = QStringLiteral("sample");
        region.bbox = QRectF(1.0, 2.0, 4.0, 3.0);
        region.polygon = {QPointF(1.0, 2.0), QPointF(5.0, 2.0), QPointF(3.0, 5.0)};
        region.maskRle = QStringLiteral("001101");
        region.score = 0.75;
        output.setData(QStringLiteral("region"), QVariant::fromValue(region));

        Region2D horizonRegion;
        horizonRegion.bbox = QRectF(0.0, 0.0, 1.0, 1.0);
        horizonRegion.polygon = {QPointF(1.0, 2.0), QPointF(1.0, 1002.0)};
        output.setData(QStringLiteral("horizon_region"), QVariant::fromValue(horizonRegion));
        Region2D nonfiniteRegion;
        nonfiniteRegion.bbox = QRectF(0.0, 0.0, 1.0, 1.0);
        nonfiniteRegion.polygon = {QPointF(1.0, 2.0), QPointF(std::numeric_limits<double>::quiet_NaN(), 3.0)};
        output.setData(QStringLiteral("nonfinite_region"), QVariant::fromValue(nonfiniteRegion));
        Region2D largeRegion;
        largeRegion.bbox = QRectF(0.0, 0.0, 1.0, 1.0);
        largeRegion.polygon.reserve(1000);
        for (int i = 0; i < 1000; ++i)
            largeRegion.polygon.append(
                QPointF(i % 2 ? std::numeric_limits<double>::max() : std::numeric_limits<double>::lowest(),
                        i % 3 ? std::numeric_limits<double>::lowest() : std::numeric_limits<double>::max()));
        output.setData(QStringLiteral("large_region"), QVariant::fromValue(largeRegion));

        PointCloudData pointCloud;
        PointCloudData nonfinitePointCloud;
        PointCloudData outOfSampleNonfinitePointCloud;
        for (int i = 0; i < 65; ++i) {
            pointCloud.points.emplace_back(i + 0.25, -i - 0.5, i + 0.75);
            nonfinitePointCloud.points.emplace_back(i + 0.25, -i - 0.5, i + 0.75);
            outOfSampleNonfinitePointCloud.points.emplace_back(i + 0.25, -i - 0.5, i + 0.75);
        }
        nonfinitePointCloud.points[10].x() = std::numeric_limits<double>::quiet_NaN();
        outOfSampleNonfinitePointCloud.points[64].z() = std::numeric_limits<double>::infinity();
        output.setData(QStringLiteral("point_cloud"), QVariant::fromValue(pointCloud));
        output.setData(QStringLiteral("nonfinite_point_cloud"), QVariant::fromValue(nonfinitePointCloud));
        output.setData(QStringLiteral("out_of_sample_nonfinite_point_cloud"),
                       QVariant::fromValue(outOfSampleNonfinitePointCloud));

        DetectionList detections;
        for (const int labelLength : {33000, 33000, 65500}) {
            Detection detection;
            detection.x = 1.0;
            detection.y = 2.0;
            detection.width = 4.0;
            detection.height = 3.0;
            detection.score = 0.625;
            detection.label = QString(labelLength, QLatin1Char('a'));
            detections.items.append(detection);
        }
        output.setData(QStringLiteral("detections"), QVariant::fromValue(detections));
        DetectionList nestedDetections;
        Detection nestedDetection;
        nestedDetection.x = 1.0;
        nestedDetection.y = 2.0;
        nestedDetection.width = 4.0;
        nestedDetection.height = 3.0;
        nestedDetection.score = 0.5;
        nestedDetection.label = QStringLiteral("nested");
        nestedDetections.items.append(nestedDetection);
        output.setData(
            QStringLiteral("nested_detections"),
            QVariant::fromValue(QVariantMap{{QStringLiteral("items"), QVariant::fromValue(nestedDetections)}}));
        output.setData(QStringLiteral("point2d_compat"), QVariantList{1.25, -2.5});
        output.setData(QStringLiteral("pointset2d_compat"),
                       QVariantList{QVariantList{1.25, -2.5}, QVariantList{3.75, 4.5}});
        output.setData(QStringLiteral("point3d_compat"), QVariantList{1.25, -2.5, 3.75});
        output.setData(QStringLiteral("plane3d_compat"), QVariantList{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0});
        output.setData(QStringLiteral("qt_point2d"), QVariant::fromValue(QPointF(6.25, -8.5)));
        output.setData(QStringLiteral("qt_pointset2d"),
                       QVariant::fromValue(QVector<QPointF>{QPointF(6.25, -8.5), QPointF(2.5, 9.75)}));
        output.setData(
            QStringLiteral("nested_qt_geometry"),
            QVariantMap{{QStringLiteral("point"), QVariant::fromValue(QPointF(-1.5, 4.25))},
                        {QStringLiteral("points"), QVariant::fromValue(QVector<QPointF>{QPointF(3.5, 7.25)})}});
        output.setData(QStringLiteral("nested_qt_list"),
                       QVariantList{QVariant::fromValue(QPointF(5.5, 6.75)),
                                    QVariant::fromValue(QVector<QPointF>{QPointF(-3.25, 8.5)})});
        output.setData(QStringLiteral("nonfinite_qt_point"),
                       QVariant::fromValue(QPointF(std::numeric_limits<double>::quiet_NaN(), 1.0)));
        output.setData(QStringLiteral("nonfinite_qt_points"),
                       QVariant::fromValue(
                           QVector<QPointF>{QPointF(1.0, 2.0), QPointF(3.0, std::numeric_limits<double>::infinity())}));
        output.setData(QStringLiteral("nested_nonfinite_qt_pointset"),
                       QVariantMap{{QStringLiteral("points"),
                                    QVariant::fromValue(QVector<QPointF>{
                                        QPointF(1.0, 2.0), QPointF(3.0, std::numeric_limits<double>::quiet_NaN())})}});
        output.setData(QStringLiteral("extreme_qt_points"),
                       QVariant::fromValue(QVector<QPointF>{
                           QPointF(std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest())}));
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    ImageCoordinateProvenance m_provenance;
};
} // namespace

class TestAgentActor : public QObject {
    Q_OBJECT

private:
    static QJsonObject waitForRun(AgentActor& actor, const QString& runId, int timeoutMs = 5000) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            QJsonObject result =
                actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
            const QString status = result.value(QStringLiteral("status")).toString();
            if (status == QLatin1String("completed") || status == QLatin1String("failed") ||
                status == QLatin1String("cancelled") || status == QLatin1String("uncertain"))
                return result;
            QTest::qWait(5);
        }
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("test_timeout")}};
    }

    // 阶7 Agent 评审修复：set_param/undo 回归需要真实可 createModule 的插件，
    // 以 CMake 注入路径安装到临时目录（hermetic）
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

    // GrabImage 默认 grabSource=Camera（无相机环境必然失败）——测试统一切到 Demo 演示图
    bool addDemoGrab(AgentActor& actor, const QString& instanceName) const {
        const QJsonObject added =
            actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", instanceName}});
        if (added.contains("error"))
            return false;
        Project* project = ProjectManager::instance().currentProject();
        return project && project->setModuleParam(instanceName, QStringLiteral("grabSource"), QStringLiteral("Demo"));
    }

    QTemporaryDir m_appDataDir;

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        ToolSchema::instance().registerDefaultTools();
        qRegisterMetaType<ModuleInstance>();
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(m_appDataDir.path()).filePath("plugins");
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(TEST_AGENTACTOR_META_GrabImage),
                              QStringLiteral(TEST_AGENTACTOR_LIB_GrabImage)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasureCircle"),
                              QStringLiteral(TEST_AGENTACTOR_META_MeasureCircle),
                              QStringLiteral(TEST_AGENTACTOR_LIB_MeasureCircle)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasurementInput"),
                              QStringLiteral(TEST_AGENTACTOR_META_MeasurementInput),
                              QStringLiteral(TEST_AGENTACTOR_LIB_MeasurementInput)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FitCircle"), QStringLiteral(TEST_AGENTACTOR_META_FitCircle),
                              QStringLiteral(TEST_AGENTACTOR_LIB_FitCircle)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("If"), QStringLiteral(TEST_AGENTACTOR_META_If),
                              QStringLiteral(TEST_AGENTACTOR_LIB_If)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("Parallel"), QStringLiteral(TEST_AGENTACTOR_META_Parallel),
                              QStringLiteral(TEST_AGENTACTOR_LIB_Parallel)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("SamPredict"), QStringLiteral(TEST_AGENTACTOR_META_SamPredict),
                              QStringLiteral(TEST_AGENTACTOR_LIB_SamPredict)));
        const QString cameraDir = QDir(pluginRoot).filePath(QStringLiteral("LegacyCamera"));
        QVERIFY(QDir().mkpath(cameraDir));
        QFile cameraMetadata(QDir(cameraDir).filePath(QStringLiteral("metadata.json")));
        QVERIFY(cameraMetadata.open(QIODevice::WriteOnly));
        QVERIFY(cameraMetadata.write(R"({"id":"com.deeplux.test.legacycamera","name":"LegacyCamera",
            "category":"camera","version":"1.0.0"})") > 0);
        cameraMetadata.close();
        const QString collisionCameraDir = QDir(pluginRoot).filePath(QStringLiteral("CameraNameCollision"));
        QVERIFY(QDir().mkpath(collisionCameraDir));
        QFile collisionCameraMetadata(QDir(collisionCameraDir).filePath(QStringLiteral("metadata.json")));
        QVERIFY(collisionCameraMetadata.open(QIODevice::WriteOnly));
        QVERIFY(collisionCameraMetadata.write(R"({"id":"com.deeplux.test.sam-predict-camera","name":"SamPredict",
            "category":"camera","version":"1.0.0"})") > 0);
        collisionCameraMetadata.close();
        const QString missingLibraryDir = QDir(pluginRoot).filePath(QStringLiteral("MissingLibraryModule"));
        QVERIFY(QDir().mkpath(missingLibraryDir));
        QFile validModuleMetadata(QStringLiteral(TEST_AGENTACTOR_META_GrabImage));
        QVERIFY(validModuleMetadata.open(QIODevice::ReadOnly));
        QJsonParseError metadataParseError;
        QJsonObject missingLibraryMetadata =
            QJsonDocument::fromJson(validModuleMetadata.readAll(), &metadataParseError).object();
        validModuleMetadata.close();
        QCOMPARE(metadataParseError.error, QJsonParseError::NoError);
        missingLibraryMetadata.insert(QStringLiteral("id"), QStringLiteral("com.deeplux.test.missinglibrary"));
        missingLibraryMetadata.insert(QStringLiteral("name"), QStringLiteral("MissingLibraryModule"));
        QFile missingLibraryMetadataFile(QDir(missingLibraryDir).filePath(QStringLiteral("metadata.json")));
        QVERIFY(missingLibraryMetadataFile.open(QIODevice::WriteOnly));
        QCOMPARE(missingLibraryMetadataFile.write(QJsonDocument(missingLibraryMetadata).toJson()),
                 QJsonDocument(missingLibraryMetadata).toJson().size());
        missingLibraryMetadataFile.close();
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasureCircle")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasurementInput")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("FitCircle")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("条件分支")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("Parallel")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("SamPredict")));
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void init() {
        // 后台执行（设计 §8.2）：任意上一个用例遗留的 Service job 先停止并回到 Idle，
        // 否则 clearModules 会被 job 租约拒绝。
        FlowExecutionService::instance().stop();
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 5000);
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        GlobalVarManager::instance().clearAllVariables();
    }

    void cleanup() {
        FlowExecutionService::instance().stop();
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 5000);
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        GlobalVarManager::instance().clearAllVariables();
    }

    void testGlobalVariableTypedCrudIsLiteralAndFailClosed() {
        AgentActor actor;
        GlobalVarManager& manager = GlobalVarManager::instance();
        const QJsonObject created =
            actor.executeTool(QStringLiteral("create_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("agentCounter")},
                                          {QStringLiteral("type"), QStringLiteral("int")},
                                          {QStringLiteral("value"), 7}});
        QCOMPARE(created.value(QStringLiteral("status")).toString(), QStringLiteral("created"));
        QCOMPARE(created.value(QStringLiteral("value")).toInt(), 7);

        const QJsonObject literal =
            actor.executeTool(QStringLiteral("create_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("agentLiteral")},
                                          {QStringLiteral("type"), QStringLiteral("string")},
                                          {QStringLiteral("value"), QStringLiteral("${agentCounter} + 1")}});
        QCOMPARE(literal.value(QStringLiteral("value")).toString(), QStringLiteral("${agentCounter} + 1"));

        const QJsonObject array = actor.executeTool(QStringLiteral("create_global_variable"),
                                                    QJsonObject{{QStringLiteral("name"), QStringLiteral("agentArray")},
                                                                {QStringLiteral("type"), QStringLiteral("int[]")},
                                                                {QStringLiteral("value"), QJsonArray{1, 2, 3}}});
        QCOMPARE(array.value(QStringLiteral("value")).toArray(), QJsonArray({1, 2, 3}));

        VarModel* literalModel = manager.getVariable(QStringLiteral("agentLiteral"));
        QVERIFY(literalModel);
        literalModel->setExpression(QStringLiteral("other expression"));
        literalModel->setIsLinked(true);
        const QJsonObject setLiteral =
            actor.executeTool(QStringLiteral("set_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("agentLiteral")},
                                          {QStringLiteral("value"), QStringLiteral("${notEvaluated}")}});
        QCOMPARE(setLiteral.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
        QCOMPARE(setLiteral.value(QStringLiteral("value")).toString(), QStringLiteral("${notEvaluated}"));
        QCOMPARE(manager.getVariable(QStringLiteral("agentLiteral"))->expression(), QStringLiteral("other expression"));
        QVERIFY(manager.getVariable(QStringLiteral("agentLiteral"))->isLinked());

        const QJsonObject badSet = actor.executeTool(
            QStringLiteral("set_global_variable"), QJsonObject{{QStringLiteral("name"), QStringLiteral("agentCounter")},
                                                               {QStringLiteral("value"), QStringLiteral("1+2")}});
        QCOMPARE(badSet.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VARIABLE_TYPE_MISMATCH"));
        QCOMPARE(manager.getVariableValue(QStringLiteral("agentCounter")).toInt(), 7);

        const QJsonObject badArray =
            actor.executeTool(QStringLiteral("set_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("agentArray")},
                                          {QStringLiteral("value"), QJsonArray{1, QStringLiteral("bad")}}});
        QCOMPARE(badArray.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VARIABLE_TYPE_MISMATCH"));
        QCOMPARE(manager.getVariableValue(QStringLiteral("agentArray")).toList(), QVariantList({1, 2, 3}));

        const QJsonObject duplicate =
            actor.executeTool(QStringLiteral("create_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("agentCounter")},
                                          {QStringLiteral("type"), QStringLiteral("int")},
                                          {QStringLiteral("value"), 9}});
        QCOMPARE(duplicate.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VARIABLE_EXISTS"));

        const QJsonObject list = actor.executeTool(QStringLiteral("list_global_variables"), {});
        QVERIFY(list.value(QStringLiteral("total")).toInt() >= 3);
        for (const QJsonValue& item : list.value(QStringLiteral("variables")).toArray())
            QVERIFY(!item.toObject().contains(QStringLiteral("value")));
        const QJsonObject get =
            actor.executeTool(QStringLiteral("get_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("agentCounter")}});
        QCOMPARE(get.value(QStringLiteral("value")).toInt(), 7);

        const QJsonObject snapshot = manager.userVariableSnapshotJson(QStringLiteral("agentCounter"));
        QVERIFY(manager.setVariableValue(QStringLiteral("agentCounter"), 11));
        AgentActor::ToolInvocation stale;
        stale.callId = QStringLiteral("stale-set");
        stale.tool = QStringLiteral("set_global_variable");
        stale.params =
            QJsonObject{{QStringLiteral("name"), QStringLiteral("agentCounter")}, {QStringLiteral("value"), 13}};
        stale.variableSnapshot = snapshot;
        const QJsonObject staleBatch = actor.executeTools({stale}, QStringLiteral("stale variable set"));
        QCOMPARE(staleBatch.value(QStringLiteral("results"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("CONFIRMATION_EXPIRED"));
        QCOMPARE(manager.getVariableValue(QStringLiteral("agentCounter")).toInt(), 11);

        const QJsonObject remove =
            actor.executeTool(QStringLiteral("remove_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("agentCounter")}});
        QCOMPARE(remove.value(QStringLiteral("status")).toString(), QStringLiteral("removed"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("get_global_variable"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("agentCounter")}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("VARIABLE_NOT_FOUND"));

        QVERIFY(manager.beginExecutionLease());
        const QJsonObject busy =
            actor.executeTool(QStringLiteral("create_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("blockedWhileRunning")},
                                          {QStringLiteral("type"), QStringLiteral("int")},
                                          {QStringLiteral("value"), 1}});
        manager.endExecutionLease();
        QCOMPARE(busy.value(QStringLiteral("errorCode")).toString(), QStringLiteral("ENGINE_BUSY"));
        QVERIFY(!manager.hasVariable(QStringLiteral("blockedWhileRunning")));
    }

    void testGlobalVariableAllSupportedTypesRoundTrip() {
        struct TypeCase {
            QString typeName;
            QJsonValue initial;
            QJsonValue updated;
            QJsonValue invalid;
            int valueMetaType;
            QList<int> itemMetaTypes;
        };
        const QList<TypeCase> cases{
            {QStringLiteral("int"), 3, 8, QStringLiteral("3"), QMetaType::Int, {}},
            {QStringLiteral("double"), 1.25, 9.5, QStringLiteral("9.5"), QMetaType::Double, {}},
            {QStringLiteral("string"), QStringLiteral("before"), QStringLiteral("after"), 2, QMetaType::QString, {}},
            {QStringLiteral("bool"), true, false, 1, QMetaType::Bool, {}},
            {QStringLiteral("int[]"),
             QJsonArray{1, 2},
             QJsonArray{3, 4},
             QJsonArray{3, QStringLiteral("bad")},
             QMetaType::QVariantList,
             {QMetaType::Int}},
            {QStringLiteral("double[]"),
             QJsonArray{1.25, 2.5},
             QJsonArray{3.5, 4.75},
             QJsonArray{3.5, QStringLiteral("bad")},
             QMetaType::QVariantList,
             {QMetaType::Double}},
            {QStringLiteral("string[]"),
             QJsonArray{QStringLiteral("a"), QStringLiteral("b")},
             QJsonArray{QStringLiteral("c")},
             QJsonArray{QStringLiteral("c"), 1},
             QMetaType::QVariantList,
             {QMetaType::QString}},
            {QStringLiteral("bool[]"),
             QJsonArray{true, false},
             QJsonArray{false},
             QJsonArray{true, 1},
             QMetaType::QVariantList,
             {QMetaType::Bool}},
        };

        AgentActor actor;
        GlobalVarManager& manager = GlobalVarManager::instance();
        for (int i = 0; i < cases.size(); ++i) {
            const TypeCase& typeCase = cases.at(i);
            const QString name = QStringLiteral("roundTrip%1").arg(i);
            const QJsonObject created = actor.executeTool(QStringLiteral("create_global_variable"),
                                                          QJsonObject{{QStringLiteral("name"), name},
                                                                      {QStringLiteral("type"), typeCase.typeName},
                                                                      {QStringLiteral("value"), typeCase.initial}});
            QCOMPARE(created.value(QStringLiteral("status")).toString(), QStringLiteral("created"));
            QCOMPARE(manager.userVariableSnapshot(name).value.userType(), typeCase.valueMetaType);
            QCOMPARE(
                actor.executeTool(QStringLiteral("get_global_variable"), QJsonObject{{QStringLiteral("name"), name}})
                    .value(QStringLiteral("value")),
                typeCase.initial);

            const QJsonObject mismatch = actor.executeTool(
                QStringLiteral("set_global_variable"),
                QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("value"), typeCase.invalid}});
            QCOMPARE(mismatch.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VARIABLE_TYPE_MISMATCH"));
            QCOMPARE(
                actor.executeTool(QStringLiteral("get_global_variable"), QJsonObject{{QStringLiteral("name"), name}})
                    .value(QStringLiteral("value")),
                typeCase.initial);

            const QJsonObject updated = actor.executeTool(
                QStringLiteral("set_global_variable"),
                QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("value"), typeCase.updated}});
            QCOMPARE(updated.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
            const QJsonObject readback =
                actor.executeTool(QStringLiteral("get_global_variable"), QJsonObject{{QStringLiteral("name"), name}});
            QCOMPARE(readback.value(QStringLiteral("value")), typeCase.updated);

            const QVariant actual = manager.userVariableSnapshot(name).value;
            QCOMPARE(actual.userType(), typeCase.valueMetaType);
            if (actual.userType() == QMetaType::QVariantList) {
                const QVariantList items = actual.toList();
                for (const QVariant& item : items)
                    QVERIFY(typeCase.itemMetaTypes.contains(item.userType()));
            }
            const QJsonObject removed = actor.executeTool(QStringLiteral("remove_global_variable"),
                                                          QJsonObject{{QStringLiteral("name"), name}});
            QCOMPARE(removed.value(QStringLiteral("status")).toString(), QStringLiteral("removed"));
        }
    }

    void testDataSourceListAndReferenceRemovalPreserveFile() {
        AgentActor actor;
        QCOMPARE(
            actor.executeTool(QStringLiteral("list_data_sources"), {}).value(QStringLiteral("errorCode")).toString(),
            QStringLiteral("NO_PROJECT"));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString imagePath = directory.filePath(QStringLiteral("sample image.bin"));
        QFile imageFile(imagePath);
        QVERIFY(imageFile.open(QIODevice::WriteOnly));
        const QByteArray originalBytes("preserve this source\0bytes", 26);
        QCOMPARE(imageFile.write(originalBytes), originalBytes.size());
        imageFile.close();

        DataSource image;
        image.id = QStringLiteral("agent-image");
        image.name = QStringLiteral("sample image");
        image.type = QStringLiteral("image");
        image.filePath = imagePath;
        project->addDataSource(image);
        DataSource cloud;
        cloud.id = QStringLiteral("agent-cloud");
        cloud.name = QStringLiteral("missing cloud");
        cloud.type = QStringLiteral("pointcloud");
        cloud.filePath = directory.filePath(QStringLiteral("missing.pcd"));
        project->addDataSource(cloud);

        const QJsonObject all = actor.executeTool(QStringLiteral("list_data_sources"), {});
        QCOMPARE(all.value(QStringLiteral("total")).toInt(), 2);
        const QJsonArray sources = all.value(QStringLiteral("dataSources")).toArray();
        QCOMPARE(sources.at(0).toObject().value(QStringLiteral("id")).toString(), image.id);
        QVERIFY(sources.at(0).toObject().value(QStringLiteral("fileExists")).toBool());
        QVERIFY(!sources.at(1).toObject().value(QStringLiteral("fileExists")).toBool());
        const QJsonObject filtered = actor.executeTool(QStringLiteral("list_data_sources"),
                                                       QJsonObject{{QStringLiteral("type"), QStringLiteral("image")}});
        QCOMPARE(filtered.value(QStringLiteral("total")).toInt(), 1);
        QCOMPARE(filtered.value(QStringLiteral("dataSources"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("id"))
                     .toString(),
                 image.id);
        QCOMPARE(actor
                     .executeTool(QStringLiteral("list_data_sources"),
                                  QJsonObject{{QStringLiteral("type"), QStringLiteral("other")}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));

        QSignalSpy removedSpy(project, &Project::dataSourceRemoved);
        const QJsonObject removed = actor.executeTool(QStringLiteral("remove_data_source_reference"),
                                                      QJsonObject{{QStringLiteral("dataSourceId"), image.id}});
        QCOMPARE(removed.value(QStringLiteral("status")).toString(), QStringLiteral("removed"));
        QVERIFY(removed.value(QStringLiteral("filePreserved")).toBool());
        QCOMPARE(removedSpy.count(), 1);
        QVERIFY(project->findDataSource(image.id) == std::nullopt);
        QFile preserved(imagePath);
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), originalBytes);

        DataSource duplicateA = image;
        duplicateA.filePath = directory.filePath(QStringLiteral("duplicate-a.bin"));
        DataSource duplicateB = image;
        duplicateB.filePath = directory.filePath(QStringLiteral("duplicate-b.bin"));
        project->addDataSource(duplicateA);
        project->addDataSource(duplicateB);
        QCOMPARE(actor
                     .executeTool(QStringLiteral("remove_data_source_reference"),
                                  QJsonObject{{QStringLiteral("dataSourceId"), image.id}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));
        QCOMPARE(project->dataSources().size(), 3);
        QCOMPARE(actor
                     .executeTool(QStringLiteral("remove_data_source_reference"),
                                  QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("absent")}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("DATA_SOURCE_NOT_FOUND"));
    }

    void testUndoStackExists() {
        AgentActor actor;
        QVERIFY(actor.undoStack() != nullptr);
    }

    void testModuleImageSummariesExposeSafeSourceCoordinates() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("coordinate_summary");
        instance.moduleId = QStringLiteral("com.deeplux.test.coordinatesummary");
        instance.name = QStringLiteral("Coordinate summary");
        project->addModule(instance);

        RunEngine& engine = RunEngine::instance();
        QVERIFY(engine.loadProject(project, [](const ModuleInstance&) { return new CoordinateSummaryAgentModule(); }));
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 3000);
        QVERIFY(engine.runObservation(runId).result.success);

        AgentActor actor;
        const QJsonObject imageResult =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"preview", false}});
        QVERIFY2(imageResult.value(QStringLiteral("status")).toString() == QLatin1String("available"),
                 qPrintable(QJsonDocument(imageResult).toJson(QJsonDocument::Compact)));
        const QJsonObject imageSummary =
            imageResult.value(QStringLiteral("summary")).toObject().value(QStringLiteral("image")).toObject();
        QCOMPARE(imageSummary.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        QCOMPARE(imageSummary.value(QStringLiteral("width")).toInt(), 5);
        QCOMPARE(imageSummary.value(QStringLiteral("height")).toInt(), 4);
        QVERIFY(!imageResult.value(QStringLiteral("summary")).toObject().contains(QStringLiteral("preview")));
        QVERIFY(!imageSummary.contains(QStringLiteral("pixels")));
        QVERIFY(!imageSummary.contains(QStringLiteral("path")));

        const QJsonObject source = imageSummary.value(QStringLiteral("sourceCoordinates")).toObject();
        QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(),
                 QStringLiteral("12345678-1234-4234-9234-123456789abc"));
        QCOMPARE(source.value(QStringLiteral("sourceWidth")).toInt(), 311);
        QCOMPARE(source.value(QStringLiteral("sourceHeight")).toInt(), 227);
        const QJsonArray matrixJson = source.value(QStringLiteral("localToSource")).toArray();
        QCOMPARE(matrixJson.size(), 9);
        const ImageCoordinateProvenance::Matrix3 expectedMatrix{{1.5, 0.25, 11.0, -0.5, 2.0, 17.0, 0.0, 0.0, 1.0}};
        for (int i = 0; i < static_cast<int>(expectedMatrix.size()); ++i)
            QCOMPARE(matrixJson.at(i).toDouble(), expectedMatrix.at(static_cast<size_t>(i)));
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = source.value(QStringLiteral("sourceImageId")).toString();
        provenance.sourceSize = QSize(source.value(QStringLiteral("sourceWidth")).toInt(),
                                      source.value(QStringLiteral("sourceHeight")).toInt());
        provenance.hasLocalToSourceTransform = true;
        for (int i = 0; i < static_cast<int>(expectedMatrix.size()); ++i)
            provenance.localToSource[static_cast<size_t>(i)] = matrixJson.at(i).toDouble();
        for (const QPointF local : {QPointF(2.0, 5.0), QPointF(4.0, 1.0)}) {
            const auto expected = provenance.mapLocalToSource(local);
            QVERIFY(expected);
            const double denominator = matrixJson.at(6).toDouble() * local.x() +
                                       matrixJson.at(7).toDouble() * local.y() + matrixJson.at(8).toDouble();
            const QPointF actual((matrixJson.at(0).toDouble() * local.x() + matrixJson.at(1).toDouble() * local.y() +
                                  matrixJson.at(2).toDouble()) /
                                     denominator,
                                 (matrixJson.at(3).toDouble() * local.x() + matrixJson.at(4).toDouble() * local.y() +
                                  matrixJson.at(5).toDouble()) /
                                     denominator);
            QCOMPARE(actual, *expected);
        }

        const QJsonObject listResult = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"key", QStringLiteral("nested_list")}});
        const QJsonArray nested = listResult.value(QStringLiteral("value")).toArray();
        QCOMPARE(nested.size(), 3);
        QVERIFY(nested.at(0).toObject().value(QStringLiteral("sourceCoordinates")).isObject());
        QVERIFY(!nested.at(1).toObject().contains(QStringLiteral("sourceCoordinates")));
        QVERIFY(!nested.at(2).toObject().contains(QStringLiteral("sourceCoordinates")));
        const QJsonObject mapResult = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"key", QStringLiteral("nested_map")}});
        QVERIFY(mapResult.value(QStringLiteral("value"))
                    .toObject()
                    .value(QStringLiteral("child"))
                    .toObject()
                    .value(QStringLiteral("sourceCoordinates"))
                    .isObject());
        const QJsonObject noProvenance = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"key", QStringLiteral("without_provenance")}});
        QVERIFY(!noProvenance.value(QStringLiteral("value")).toObject().contains(QStringLiteral("sourceCoordinates")));

        const QJsonObject previewResult =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", instance.id}, {"preview", true}});
        const QJsonObject preview =
            previewResult.value(QStringLiteral("summary")).toObject().value(QStringLiteral("preview")).toObject();
        const QByteArray png = QByteArray::fromBase64(preview.value(QStringLiteral("base64")).toString().toLatin1());
        QVERIFY(!QImage::fromData(png, "PNG").isNull());
        QCOMPARE(preview.value(QStringLiteral("width")).toInt(), 64);
        QCOMPARE(preview.value(QStringLiteral("height")).toInt(), 51);
        QCOMPARE(preview.value(QStringLiteral("sourceRect")).toArray(), QJsonArray({0, 0, 5, 4}));
        QCOMPARE(preview.value(QStringLiteral("scaleX")).toDouble(), 5.0 / 64.0);
        QCOMPARE(preview.value(QStringLiteral("scaleY")).toDouble(), 4.0 / 51.0);
        engine.clearModules();
    }

    void testStructuredModuleOutputsIncludeVerifiedSourceCoordinates() {
        constexpr auto instanceId = "structured_coordinates";
        RunEngine& engine = RunEngine::instance();
        AgentActor actor;
        const auto startRun = [&](const ImageCoordinateProvenance& provenance, QString& runId) {
            Project* project = ProjectManager::instance().newProject();
            if (!project)
                return false;
            ModuleInstance instance;
            instance.id = QString::fromLatin1(instanceId);
            instance.moduleId = QStringLiteral("com.deeplux.test.structuredcoordinates");
            project->addModule(instance);
            if (!engine.loadProject(project, [provenance](const ModuleInstance&) {
                    return new StructuredCoordinatesAgentModule(provenance);
                }))
                return false;
            return engine.runOnceAsync(&runId);
        };
        const auto provenanceFor = [](const QString& id, const ImageCoordinateProvenance::Matrix3& matrix) {
            ImageCoordinateProvenance provenance;
            provenance.sourceImageId = id;
            provenance.sourceSize = QSize(311, 227);
            provenance.hasLocalToSourceTransform = true;
            provenance.localToSource = matrix;
            return provenance;
        };
        const auto query = [&](const QString& runId, const QString& key) {
            return actor.executeTool(
                QStringLiteral("get_module_output"),
                QJsonObject{{"runId", runId}, {"instanceId", QString::fromLatin1(instanceId)}, {"key", key}});
        };
        const auto queryWithPreview = [&](const QString& runId, const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{"runId", runId},
                                                 {"instanceId", QString::fromLatin1(instanceId)},
                                                 {"key", key},
                                                 {"preview", true}});
        };
        const auto queryPolygonPage = [&](const QString& runId, const QString& key, int offset, int limit) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{"runId", runId},
                                                 {"instanceId", QString::fromLatin1(instanceId)},
                                                 {"key", key},
                                                 {"polygonOffset", offset},
                                                 {"polygonLimit", limit}});
        };
        const QString validId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        const ImageCoordinateProvenance::Matrix3 projective{{1.2, 0.1, 11.0, -0.2, 0.8, 17.0, 0.002, -0.001, 1.0}};

        QString runId;
        const ImageCoordinateProvenance valid = provenanceFor(validId, projective);
        QVERIFY(startRun(valid, runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 3000);
        QVERIFY(engine.runObservation(runId).result.success);
        const QJsonObject pointCloudSummary = query(runId, QStringLiteral("point_cloud"));
        QCOMPARE(pointCloudSummary.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        const QJsonObject pointCloudValue = pointCloudSummary.value(QStringLiteral("value")).toObject();
        QCOMPARE(pointCloudValue.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("unspecified"));
        QVERIFY(!pointCloudSummary.contains(QStringLiteral("preview")));
        const QJsonObject pointCloudPreviewResponse = queryWithPreview(runId, QStringLiteral("point_cloud"));
        const QJsonObject pointCloudPreviewValue = pointCloudPreviewResponse.value(QStringLiteral("value")).toObject();
        QCOMPARE(pointCloudPreviewValue.value(QStringLiteral("coordinateSystem")).toString(),
                 QStringLiteral("unspecified"));
        const QJsonObject pointCloudPreview = pointCloudPreviewResponse.value(QStringLiteral("preview")).toObject();
        QCOMPARE(pointCloudPreview.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("unspecified"));
        QCOMPARE(pointCloudPreview.value(QStringLiteral("sampleCount")).toInt(), 64);
        QCOMPARE(pointCloudPreview.value(QStringLiteral("totalPoints")).toInt(), 65);
        const QJsonArray cloudSamples = pointCloudPreview.value(QStringLiteral("points")).toArray();
        QCOMPARE(cloudSamples.size(), 64);
        QVERIFY(!pointCloudPreview.contains(QStringLiteral("sourceImageCoordinates")));
        QVERIFY(!pointCloudPreview.contains(QStringLiteral("sourceCoordinates")));
        QVERIFY(!pointCloudPreviewValue.contains(QStringLiteral("sourceImageCoordinates")));
        QVERIFY(!pointCloudPreviewValue.contains(QStringLiteral("sourceCoordinates")));
        for (int i = 0; i < cloudSamples.size(); ++i)
            QCOMPARE(cloudSamples.at(i).toArray(), QJsonArray({i + 0.25, -i - 0.5, i + 0.75}));
        const QJsonObject sampledNonfinite = queryWithPreview(runId, QStringLiteral("nonfinite_point_cloud"));
        QCOMPARE(sampledNonfinite.value(QStringLiteral("previewUnavailable")).toString(),
                 QStringLiteral("non-finite point coordinate"));
        QVERIFY(!sampledNonfinite.contains(QStringLiteral("preview")));
        QCOMPARE(sampledNonfinite.value(QStringLiteral("value"))
                     .toObject()
                     .value(QStringLiteral("coordinateSystem"))
                     .toString(),
                 QStringLiteral("unspecified"));
        const QJsonObject unsampledNonfinite =
            queryWithPreview(runId, QStringLiteral("out_of_sample_nonfinite_point_cloud"));
        QCOMPARE(unsampledNonfinite.value(QStringLiteral("preview"))
                     .toObject()
                     .value(QStringLiteral("coordinateSystem"))
                     .toString(),
                 QStringLiteral("unspecified"));
        QCOMPARE(
            unsampledNonfinite.value(QStringLiteral("preview")).toObject().value(QStringLiteral("sampleCount")).toInt(),
            64);
        const QJsonObject circleResponse = query(runId, QStringLiteral("circle"));
        QCOMPARE(circleResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        const QJsonObject circle = circleResponse.value(QStringLiteral("value")).toObject();
        QCOMPARE(circle.value(QStringLiteral("centerX")).toDouble(), 3.0);
        QCOMPARE(circle.value(QStringLiteral("centerY")).toDouble(), 2.5);
        QCOMPARE(circle.value(QStringLiteral("radius")).toDouble(), 4.5);
        QCOMPARE(circle.value(QStringLiteral("score")).toDouble(), 0.875);
        QCOMPARE(circle.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        const auto expectedCenter = valid.mapLocalToSource(QPointF(3.0, 2.5));
        QVERIFY(expectedCenter);
        const QJsonObject sourceCenter = circle.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(sourceCenter.value(QStringLiteral("sourceImageId")).toString(), validId);
        QCOMPARE(sourceCenter.value(QStringLiteral("coordinateSystem")).toString(),
                 QStringLiteral("source_image_pixels"));
        QCOMPARE(sourceCenter.value(QStringLiteral("centerX")).toDouble(), expectedCenter->x());
        QCOMPARE(sourceCenter.value(QStringLiteral("centerY")).toDouble(), expectedCenter->y());

        const QJsonObject regionResponse = query(runId, QStringLiteral("region"));
        QCOMPARE(regionResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        const QJsonObject region = regionResponse.value(QStringLiteral("value")).toObject();
        const QJsonArray localBbox = region.value(QStringLiteral("bbox")).toArray();
        QCOMPARE(localBbox, QJsonArray({1.0, 2.0, 4.0, 3.0}));
        QCOMPARE(region.value(QStringLiteral("label")).toString(), QStringLiteral("sample"));
        QCOMPARE(region.value(QStringLiteral("polygonPointCount")).toInt(), 3);
        QCOMPARE(region.value(QStringLiteral("maskRleChars")).toInt(), 6);
        QCOMPARE(region.value(QStringLiteral("score")).toDouble(), 0.75);
        const auto expectedBounds = valid.mapRectToSourceBounds(QRectF(1.0, 2.0, 4.0, 3.0));
        QVERIFY(expectedBounds);
        const QJsonObject sourceBounds = region.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(sourceBounds.value(QStringLiteral("sourceImageId")).toString(), validId);
        QCOMPARE(sourceBounds.value(QStringLiteral("coordinateSystem")).toString(),
                 QStringLiteral("source_image_pixels"));
        const QJsonArray mappedBbox = sourceBounds.value(QStringLiteral("bbox")).toArray();
        QCOMPARE(mappedBbox.size(), 4);
        QCOMPARE(mappedBbox.at(0).toDouble(), expectedBounds->x());
        QCOMPARE(mappedBbox.at(1).toDouble(), expectedBounds->y());
        QCOMPARE(mappedBbox.at(2).toDouble(), expectedBounds->width());
        QCOMPARE(mappedBbox.at(3).toDouble(), expectedBounds->height());

        const QJsonObject firstPolygonPage = queryPolygonPage(runId, QStringLiteral("region"), 0, 2);
        const QJsonObject firstPolygon = firstPolygonPage.value(QStringLiteral("polygonPage")).toObject();
        QCOMPARE(firstPolygon.value(QStringLiteral("totalPoints")).toInt(), 3);
        QCOMPARE(firstPolygon.value(QStringLiteral("offset")).toInt(), 0);
        QCOMPARE(firstPolygon.value(QStringLiteral("limit")).toInt(), 2);
        QVERIFY(firstPolygon.value(QStringLiteral("truncated")).toBool());
        QCOMPARE(firstPolygon.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        QCOMPARE(firstPolygon.value(QStringLiteral("points")).toArray(),
                 QJsonArray({QJsonArray({1.0, 2.0}), QJsonArray({5.0, 2.0})}));
        const QJsonObject firstPolygonSource = firstPolygon.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(firstPolygonSource.value(QStringLiteral("sourceImageId")).toString(), validId);
        QCOMPARE(firstPolygonSource.value(QStringLiteral("coordinateSystem")).toString(),
                 QStringLiteral("source_image_pixels"));
        const QJsonArray firstPolygonSourcePoints = firstPolygonSource.value(QStringLiteral("points")).toArray();
        QCOMPARE(firstPolygonSourcePoints.size(), 2);
        for (int i = 0; i < firstPolygonSourcePoints.size(); ++i) {
            const QJsonArray localPoint = firstPolygon.value(QStringLiteral("points")).toArray().at(i).toArray();
            const auto expectedPoint =
                valid.mapLocalToSource(QPointF(localPoint.at(0).toDouble(), localPoint.at(1).toDouble()));
            QVERIFY(expectedPoint);
            QCOMPARE(firstPolygonSourcePoints.at(i).toArray(), QJsonArray({expectedPoint->x(), expectedPoint->y()}));
        }
        const QJsonObject secondPolygonPage = queryPolygonPage(runId, QStringLiteral("region"), 2, 2);
        const QJsonObject secondPolygon = secondPolygonPage.value(QStringLiteral("polygonPage")).toObject();
        QCOMPARE(secondPolygon.value(QStringLiteral("totalPoints")).toInt(), 3);
        QCOMPARE(secondPolygon.value(QStringLiteral("offset")).toInt(), 2);
        QCOMPARE(secondPolygon.value(QStringLiteral("limit")).toInt(), 2);
        QVERIFY(!secondPolygon.value(QStringLiteral("truncated")).toBool());
        QCOMPARE(secondPolygon.value(QStringLiteral("points")).toArray(),
                 QJsonArray({QJsonValue(QJsonArray({3.0, 5.0}))}));
        const QJsonObject emptyPolygonPage =
            queryPolygonPage(runId, QStringLiteral("region"), 3, 2).value(QStringLiteral("polygonPage")).toObject();
        QVERIFY(emptyPolygonPage.value(QStringLiteral("points")).toArray().isEmpty());
        QVERIFY(!emptyPolygonPage.contains(QStringLiteral("sourceImageCoordinates")));

        for (const QJsonObject& invalidParams :
             {QJsonObject{{"polygonOffset", -1}}, QJsonObject{{"polygonOffset", 0.5}}, QJsonObject{{"polygonLimit", 0}},
              QJsonObject{{"polygonLimit", 1001}}, QJsonObject{{"offset", 0}, {"polygonLimit", 1}}}) {
            QJsonObject args{
                {"runId", runId}, {"instanceId", QString::fromLatin1(instanceId)}, {"key", QStringLiteral("region")}};
            for (const QString& name : invalidParams.keys())
                args.insert(name, invalidParams.value(name));
            QCOMPARE(actor.executeTool(QStringLiteral("get_module_output"), args)
                         .value(QStringLiteral("errorCode"))
                         .toString(),
                     QStringLiteral("INVALID_PARAM"));
        }
        QCOMPARE(actor
                     .executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{"runId", runId},
                                              {"instanceId", QString::fromLatin1(instanceId)},
                                              {"key", QStringLiteral("circle")},
                                              {"polygonOffset", 0}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        const QJsonObject polygonParamsOmitted = query(runId, QStringLiteral("region"));
        QVERIFY(!polygonParamsOmitted.contains(QStringLiteral("polygonPage")));

        const QJsonObject detectionPage = actor.executeTool(QStringLiteral("get_module_output"),
                                                            QJsonObject{{"runId", runId},
                                                                        {"instanceId", QString::fromLatin1(instanceId)},
                                                                        {"key", QStringLiteral("detections")},
                                                                        {"offset", 0},
                                                                        {"limit", 2}});
        QCOMPARE(detectionPage.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(detectionPage.value(QStringLiteral("totalRows")).toInt(), 3);
        QCOMPARE(detectionPage.value(QStringLiteral("offset")).toInt(), 0);
        QCOMPARE(detectionPage.value(QStringLiteral("limit")).toInt(), 2);
        QCOMPARE(detectionPage.value(QStringLiteral("value")).toArray().size(), 1);
        QVERIFY(detectionPage.value(QStringLiteral("truncated")).toBool());
        QVERIFY(QJsonDocument(detectionPage).toJson(QJsonDocument::Compact).size() <= 65536);
        const QJsonObject detection = detectionPage.value(QStringLiteral("value")).toArray().first().toObject();
        QCOMPARE(detection.value(QStringLiteral("x")).toDouble(), 1.0);
        QCOMPARE(detection.value(QStringLiteral("y")).toDouble(), 2.0);
        QCOMPARE(detection.value(QStringLiteral("width")).toDouble(), 4.0);
        QCOMPARE(detection.value(QStringLiteral("height")).toDouble(), 3.0);
        QCOMPARE(detection.value(QStringLiteral("score")).toDouble(), 0.625);
        QCOMPARE(detection.value(QStringLiteral("label")).toString().size(), 33000);
        QCOMPARE(detection.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        const QJsonObject detectionSource = detection.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(detectionSource.value(QStringLiteral("sourceImageId")).toString(), validId);
        QCOMPARE(detectionSource.value(QStringLiteral("coordinateSystem")).toString(),
                 QStringLiteral("source_image_pixels"));
        const QJsonArray detectionBbox = detectionSource.value(QStringLiteral("bbox")).toArray();
        QCOMPARE(detectionBbox.size(), 4);
        QCOMPARE(detectionBbox.at(0).toDouble(), expectedBounds->x());
        QCOMPARE(detectionBbox.at(1).toDouble(), expectedBounds->y());
        QCOMPARE(detectionBbox.at(2).toDouble(), expectedBounds->width());
        QCOMPARE(detectionBbox.at(3).toDouble(), expectedBounds->height());
        const QJsonObject oversizedRow = actor.executeTool(QStringLiteral("get_module_output"),
                                                           QJsonObject{{"runId", runId},
                                                                       {"instanceId", QString::fromLatin1(instanceId)},
                                                                       {"key", QStringLiteral("detections")},
                                                                       {"offset", 2},
                                                                       {"limit", 1}});
        QCOMPARE(oversizedRow.value(QStringLiteral("errorCode")).toString(), QStringLiteral("ROW_TOO_LARGE"));
        QVERIFY(QJsonDocument(oversizedRow).toJson(QJsonDocument::Compact).size() <= 65536);
        const QJsonArray nestedRows = query(runId, QStringLiteral("nested_detections"))
                                          .value(QStringLiteral("value"))
                                          .toObject()
                                          .value(QStringLiteral("items"))
                                          .toArray();
        QCOMPARE(nestedRows.size(), 1);
        const QJsonObject nestedDetection = nestedRows.first().toObject();
        QCOMPARE(nestedDetection.value(QStringLiteral("x")).toDouble(), 1.0);
        QCOMPARE(nestedDetection.value(QStringLiteral("y")).toDouble(), 2.0);
        QCOMPARE(nestedDetection.value(QStringLiteral("width")).toDouble(), 4.0);
        QCOMPARE(nestedDetection.value(QStringLiteral("height")).toDouble(), 3.0);
        QCOMPARE(nestedDetection.value(QStringLiteral("score")).toDouble(), 0.5);
        QCOMPARE(nestedDetection.value(QStringLiteral("label")).toString(), QStringLiteral("nested"));
        QCOMPARE(nestedDetection.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        const QJsonObject nestedSource = nestedDetection.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(nestedSource.value(QStringLiteral("sourceImageId")).toString(), validId);
        QCOMPARE(nestedSource.value(QStringLiteral("coordinateSystem")).toString(),
                 QStringLiteral("source_image_pixels"));
        const QJsonArray nestedBbox = nestedSource.value(QStringLiteral("bbox")).toArray();
        QCOMPARE(nestedBbox.size(), 4);
        QCOMPARE(nestedBbox.at(0).toDouble(), expectedBounds->x());
        QCOMPARE(nestedBbox.at(1).toDouble(), expectedBounds->y());
        QCOMPARE(nestedBbox.at(2).toDouble(), expectedBounds->width());
        QCOMPARE(nestedBbox.at(3).toDouble(), expectedBounds->height());

        const QJsonArray point2d =
            query(runId, QStringLiteral("point2d_compat")).value(QStringLiteral("value")).toArray();
        QCOMPARE(point2d, QJsonArray({1.25, -2.5}));
        const QJsonArray pointset2d =
            query(runId, QStringLiteral("pointset2d_compat")).value(QStringLiteral("value")).toArray();
        QCOMPARE(pointset2d, QJsonArray({QJsonArray({1.25, -2.5}), QJsonArray({3.75, 4.5})}));
        for (const QJsonValue& point : pointset2d)
            QVERIFY(!point.toObject().contains(QStringLiteral("sourceImageCoordinates")));
        const QJsonObject point3d = query(runId, QStringLiteral("point3d_compat"));
        QCOMPARE(point3d.value(QStringLiteral("value")).toArray(), QJsonArray({1.25, -2.5, 3.75}));
        QVERIFY(!point3d.contains(QStringLiteral("sourceImageCoordinates")));
        QVERIFY(!point3d.contains(QStringLiteral("sourceCoordinates")));
        QVERIFY(!point3d.contains(QStringLiteral("coordinateSystem")));
        const QJsonObject plane3d = query(runId, QStringLiteral("plane3d_compat"));
        QCOMPARE(plane3d.value(QStringLiteral("value")).toArray(),
                 QJsonArray({1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0}));
        QVERIFY(!plane3d.contains(QStringLiteral("sourceImageCoordinates")));
        QVERIFY(!plane3d.contains(QStringLiteral("sourceCoordinates")));
        QVERIFY(!plane3d.contains(QStringLiteral("coordinateSystem")));

        const QJsonObject qtPoint = query(runId, QStringLiteral("qt_point2d"));
        QCOMPARE(qtPoint.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(qtPoint.value(QStringLiteral("value")).toArray(), QJsonArray({6.25, -8.5}));
        QVERIFY(!qtPoint.contains(QStringLiteral("sourceImageCoordinates")));
        const auto queryPointPage = [&](int offset, int limit) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{"runId", runId},
                                                 {"instanceId", QString::fromLatin1(instanceId)},
                                                 {"key", QStringLiteral("qt_pointset2d")},
                                                 {"offset", offset},
                                                 {"limit", limit}});
        };
        const QJsonObject firstQtPointPage = queryPointPage(0, 1);
        QCOMPARE(firstQtPointPage.value(QStringLiteral("totalRows")).toInt(), 2);
        QCOMPARE(firstQtPointPage.value(QStringLiteral("offset")).toInt(), 0);
        QCOMPARE(firstQtPointPage.value(QStringLiteral("limit")).toInt(), 1);
        QVERIFY(firstQtPointPage.value(QStringLiteral("truncated")).toBool());
        QCOMPARE(firstQtPointPage.value(QStringLiteral("value")).toArray().first().toArray(), QJsonArray({6.25, -8.5}));
        const QJsonObject secondQtPointPage = queryPointPage(1, 1);
        QCOMPARE(secondQtPointPage.value(QStringLiteral("totalRows")).toInt(), 2);
        QCOMPARE(secondQtPointPage.value(QStringLiteral("offset")).toInt(), 1);
        QCOMPARE(secondQtPointPage.value(QStringLiteral("limit")).toInt(), 1);
        QVERIFY(!secondQtPointPage.value(QStringLiteral("truncated")).toBool());
        QCOMPARE(secondQtPointPage.value(QStringLiteral("value")).toArray().first().toArray(), QJsonArray({2.5, 9.75}));
        const QJsonObject nestedQtGeometry =
            query(runId, QStringLiteral("nested_qt_geometry")).value(QStringLiteral("value")).toObject();
        QCOMPARE(nestedQtGeometry.value(QStringLiteral("point")).toArray(), QJsonArray({-1.5, 4.25}));
        QCOMPARE(nestedQtGeometry.value(QStringLiteral("points")).toArray().first().toArray(), QJsonArray({3.5, 7.25}));
        QVERIFY(!nestedQtGeometry.contains(QStringLiteral("sourceImageCoordinates")));
        const QJsonArray nestedQtList =
            query(runId, QStringLiteral("nested_qt_list")).value(QStringLiteral("value")).toArray();
        QCOMPARE(nestedQtList.at(0).toArray(), QJsonArray({5.5, 6.75}));
        QCOMPARE(nestedQtList.at(1).toArray().first().toArray(), QJsonArray({-3.25, 8.5}));
        const QJsonObject nonfiniteQtPoint = query(runId, QStringLiteral("nonfinite_qt_point"));
        QCOMPARE(nonfiniteQtPoint.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!nonfiniteQtPoint.contains(QStringLiteral("value")));
        const QJsonObject nonfiniteQtPoints = query(runId, QStringLiteral("nonfinite_qt_points"));
        QCOMPARE(nonfiniteQtPoints.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!nonfiniteQtPoints.contains(QStringLiteral("value")));
        const QJsonObject nonfiniteFirstPage = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{"runId", runId},
                                                             {"instanceId", QString::fromLatin1(instanceId)},
                                                             {"key", QStringLiteral("nonfinite_qt_points")},
                                                             {"offset", 0},
                                                             {"limit", 1}});
        QCOMPARE(nonfiniteFirstPage.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!nonfiniteFirstPage.contains(QStringLiteral("value")));
        const QJsonObject nestedNonfiniteQtPointSet = query(runId, QStringLiteral("nested_nonfinite_qt_pointset"));
        QCOMPARE(nestedNonfiniteQtPointSet.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!nestedNonfiniteQtPointSet.contains(QStringLiteral("value")));
        const QJsonObject extremeQtPoints = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{"runId", runId},
                                                             {"instanceId", QString::fromLatin1(instanceId)},
                                                             {"key", QStringLiteral("extreme_qt_points")},
                                                             {"offset", 0},
                                                             {"limit", 1000}});
        QCOMPARE(extremeQtPoints.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(extremeQtPoints.value(QStringLiteral("limit")).toInt(), 1000);
        QVERIFY(QJsonDocument(extremeQtPoints).toJson(QJsonDocument::Compact).size() <= 65536);
        const QJsonArray extremePoint = extremeQtPoints.value(QStringLiteral("value")).toArray().first().toArray();
        QCOMPARE(extremePoint.at(0).toDouble(), std::numeric_limits<double>::max());
        QCOMPARE(extremePoint.at(1).toDouble(), std::numeric_limits<double>::lowest());

        for (const ImageCoordinateProvenance& unavailable :
             {ImageCoordinateProvenance(), provenanceFor(QStringLiteral("/private/source.tif"), projective)}) {
            runId.clear();
            QVERIFY(startRun(unavailable, runId));
            QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 3000);
            QVERIFY(engine.runObservation(runId).result.success);
            const QJsonObject localCircle =
                query(runId, QStringLiteral("circle")).value(QStringLiteral("value")).toObject();
            QCOMPARE(localCircle.value(QStringLiteral("centerX")).toDouble(), 3.0);
            QCOMPARE(localCircle.value(QStringLiteral("centerY")).toDouble(), 2.5);
            QVERIFY(!localCircle.contains(QStringLiteral("sourceImageCoordinates")));
            const QJsonArray localDetections =
                actor
                    .executeTool(QStringLiteral("get_module_output"),
                                 QJsonObject{{"runId", runId},
                                             {"instanceId", QString::fromLatin1(instanceId)},
                                             {"key", QStringLiteral("detections")},
                                             {"offset", 0},
                                             {"limit", 1}})
                    .value(QStringLiteral("value"))
                    .toArray();
            QCOMPARE(localDetections.size(), 1);
            QCOMPARE(localDetections.first().toObject().value(QStringLiteral("x")).toDouble(), 1.0);
            QCOMPARE(localDetections.first().toObject().value(QStringLiteral("coordinateSystem")).toString(),
                     QStringLiteral("image_pixels"));
            QVERIFY(!localDetections.first().toObject().contains(QStringLiteral("sourceImageCoordinates")));
            const QJsonObject localRegion =
                query(runId, QStringLiteral("region")).value(QStringLiteral("value")).toObject();
            QCOMPARE(localRegion.value(QStringLiteral("bbox")).toArray(), QJsonArray({1.0, 2.0, 4.0, 3.0}));
            QVERIFY(!localRegion.contains(QStringLiteral("sourceImageCoordinates")));
            const QJsonObject localPolygonPage =
                queryPolygonPage(runId, QStringLiteral("region"), 0, 2).value(QStringLiteral("polygonPage")).toObject();
            QCOMPARE(localPolygonPage.value(QStringLiteral("points")).toArray().size(), 2);
            QVERIFY(!localPolygonPage.contains(QStringLiteral("sourceImageCoordinates")));
        }

        const ImageCoordinateProvenance::Matrix3 horizon{{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, -3.0}};
        runId.clear();
        QVERIFY(startRun(provenanceFor(validId, horizon), runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 3000);
        QVERIFY(engine.runObservation(runId).result.success);
        const QJsonObject horizonCircle =
            query(runId, QStringLiteral("circle")).value(QStringLiteral("value")).toObject();
        QVERIFY(!horizonCircle.contains(QStringLiteral("sourceImageCoordinates")));
        QCOMPARE(horizonCircle.value(QStringLiteral("centerX")).toDouble(), 3.0);
        const QJsonObject horizonRegion =
            query(runId, QStringLiteral("region")).value(QStringLiteral("value")).toObject();
        QVERIFY(!horizonRegion.contains(QStringLiteral("sourceImageCoordinates")));
        QCOMPARE(horizonRegion.value(QStringLiteral("bbox")).toArray(), QJsonArray({1.0, 2.0, 4.0, 3.0}));
        const QJsonArray horizonDetections =
            actor
                .executeTool(QStringLiteral("get_module_output"),
                             QJsonObject{{"runId", runId},
                                         {"instanceId", QString::fromLatin1(instanceId)},
                                         {"key", QStringLiteral("detections")},
                                         {"offset", 0},
                                         {"limit", 1}})
                .value(QStringLiteral("value"))
                .toArray();
        QCOMPARE(horizonDetections.size(), 1);
        QVERIFY(!horizonDetections.first().toObject().contains(QStringLiteral("sourceImageCoordinates")));

        runId.clear();
        QVERIFY(startRun(provenanceFor(validId, projective), runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 3000);
        QVERIFY(engine.runObservation(runId).result.success);
        const QJsonObject mixedHorizonPage = queryPolygonPage(runId, QStringLiteral("horizon_region"), 0, 2)
                                                 .value(QStringLiteral("polygonPage"))
                                                 .toObject();
        QCOMPARE(mixedHorizonPage.value(QStringLiteral("points")).toArray(),
                 QJsonArray({QJsonArray({1.0, 2.0}), QJsonArray({1.0, 1002.0})}));
        QVERIFY(!mixedHorizonPage.contains(QStringLiteral("sourceImageCoordinates")));
        const QJsonObject nonfinitePolygon = queryPolygonPage(runId, QStringLiteral("nonfinite_region"), 0, 2);
        QCOMPARE(nonfinitePolygon.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!nonfinitePolygon.contains(QStringLiteral("polygonPage")));

        const ImageCoordinateProvenance::Matrix3 largeScale{{0.5, 0.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 1.0}};
        runId.clear();
        QVERIFY(startRun(provenanceFor(validId, largeScale), runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 3000);
        QVERIFY(engine.runObservation(runId).result.success);
        const QJsonObject largePolygonResponse = queryPolygonPage(runId, QStringLiteral("large_region"), 0, 1000);
        const QJsonObject largePolygonPage = largePolygonResponse.value(QStringLiteral("polygonPage")).toObject();
        const QJsonArray largeLocalPoints = largePolygonPage.value(QStringLiteral("points")).toArray();
        const QJsonArray largeSourcePoints = largePolygonPage.value(QStringLiteral("sourceImageCoordinates"))
                                                 .toObject()
                                                 .value(QStringLiteral("points"))
                                                 .toArray();
        QVERIFY(largePolygonPage.value(QStringLiteral("truncated")).toBool());
        QVERIFY(!largeLocalPoints.isEmpty());
        QVERIFY(largeLocalPoints.size() < 1000);
        QCOMPARE(largeSourcePoints.size(), largeLocalPoints.size());
        QVERIFY(QJsonDocument(largePolygonResponse).toJson(QJsonDocument::Compact).size() <= 65536);
        const ImageCoordinateProvenance scaleProvenance = provenanceFor(validId, largeScale);
        for (int i = 0; i < largeLocalPoints.size(); ++i) {
            const QJsonArray localPoint = largeLocalPoints.at(i).toArray();
            const auto expectedPoint =
                scaleProvenance.mapLocalToSource(QPointF(localPoint.at(0).toDouble(), localPoint.at(1).toDouble()));
            QVERIFY(expectedPoint);
            QCOMPARE(largeSourcePoints.at(i).toArray(), QJsonArray({expectedPoint->x(), expectedPoint->y()}));
        }
    }

    void testStepFailureAfterExecutionEstablishesUndoBarrier() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("step_failure");
        instance.moduleId = QStringLiteral("com.deeplux.test.agentstepfailure");
        instance.name = QStringLiteral("Before step");
        project->addModule(instance);

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "After edit"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString sideEffectPath = tempDir.filePath(QStringLiteral("side-effect.txt"));
        auto* module = new StepFailureAfterFileModule(sideEffectPath);
        QVERIFY(module->initialize());
        RunEngine::instance().addModule(module);
        actor.noteRunEngineSynchronized(project);

        const QJsonObject result = actor.executeTool(QStringLiteral("step_once"), {});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = result.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QCOMPARE(waitForRun(actor, runId).value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(module->sideEffectWritten());
        QVERIFY(QFileInfo::exists(sideEffectPath));
        QVERIFY(actor.undoBlockedByBarrier());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("After edit"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "After step"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        QVERIFY(actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("After edit"));
        QVERIFY(actor.undoBlockedByBarrier());
        QVERIFY(!actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("After edit"));
    }

    void testUncertainRunOutcomeIsVisibleAndNotRetryable() {
        RunEngine& engine = RunEngine::instance();
        UncertainOutcomeAgentModule module;
        QVERIFY(module.initialize());
        engine.addModule(&module);
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 1000);

        AgentActor actor;
        const QJsonObject results =
            actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(results.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(results.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!results.value(QStringLiteral("retryable")).toBool());
        QCOMPARE(results.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("code")).toInt(),
                 ExecError::SideEffectUncertain);

        const QJsonObject waited =
            actor.executeTool(QStringLiteral("wait_for_run"), QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(waited.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!waited.value(QStringLiteral("retryable")).toBool());

        const QJsonObject output = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), module.instanceName()}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(output.contains(QStringLiteral("lastError")));
        engine.clearModules();
    }

    void testAcceptedRunFlowEstablishesUndoBarrier() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("measurement_input");
        instance.moduleId = QStringLiteral("MeasurementInput");
        instance.name = QStringLiteral("Measurement input");
        project->addModule(instance);

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "Before run"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        const QJsonObject run = actor.executeTool(QStringLiteral("run_flow"), {{QStringLiteral("wait"), false}});
        QCOMPARE(run.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        QVERIFY(!run.value(QStringLiteral("runId")).toString().isEmpty());
        QVERIFY(actor.undoBlockedByBarrier());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Before run"));
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "After run"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        QVERIFY(actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Before run"));
        QVERIFY(actor.undoBlockedByBarrier());
        QVERIFY(!actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Before run"));
    }

    void testPreExecutionRunFailureDoesNotEstablishUndoBarrier() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("unavailable_run_plugin");
        instance.moduleId = QStringLiteral("MissingAgentRunPlugin");
        instance.name = QStringLiteral("Before run");
        project->addModule(instance);

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "Edited"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        const QJsonObject run = actor.executeTool(QStringLiteral("run_flow"), {});
        QVERIFY(run.contains(QStringLiteral("error")));
        QVERIFY(!actor.undoBlockedByBarrier());
        QVERIFY(actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Before run"));
    }

    void testExecutedStepFailureBatchKeepsCallIdsAndBarrier() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("step_failure");
        instance.moduleId = QStringLiteral("com.deeplux.test.agentstepfailure");
        instance.name = QStringLiteral("Before step");
        project->addModule(instance);

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString sideEffectPath = tempDir.filePath(QStringLiteral("batch-side-effect.txt"));
        auto* module = new StepFailureAfterFileModule(sideEffectPath);
        QVERIFY(module->initialize());
        RunEngine::instance().addModule(module);
        AgentActor actor;
        actor.noteRunEngineSynchronized(project);

        const QJsonObject batch =
            actor.executeTools({{QStringLiteral("step"), QStringLiteral("step_once"), QJsonObject()},
                                {QStringLiteral("after"), QStringLiteral("rename_module"),
                                 QJsonObject{{"instanceId", instance.id}, {"name", "Should be skipped"}}}},
                               QStringLiteral("step-barrier-test"));
        QCOMPARE(batch.value(QStringLiteral("status")).toString(), QStringLiteral("partial_failed"));
        const QJsonArray results = batch.value(QStringLiteral("results")).toArray();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.at(0).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("step"));
        const QJsonObject stepResult = results.at(0).toObject().value(QStringLiteral("result")).toObject();
        QCOMPARE(stepResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        QVERIFY(!stepResult.value(QStringLiteral("runId")).toString().isEmpty());
        QCOMPARE(waitForRun(actor, stepResult.value(QStringLiteral("runId")).toString())
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("failed"));
        QCOMPARE(results.at(1).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("after"));
        QCOMPARE(results.at(1)
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("ENGINE_BUSY"));
        QVERIFY(QFileInfo::exists(sideEffectPath));
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Before step"));
        QVERIFY(actor.undoBlockedByBarrier());
        QVERIFY(!actor.undoLastAction());
    }

    void testSuccessfulStepOnceEstablishesUndoBarrier() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("successful_step_barrier");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("original");
        instance.params.insert(QStringLiteral("grabSource"), QStringLiteral("Demo"));
        project->addModule(instance);

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "edit A"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        const QJsonObject step = actor.executeTool(QStringLiteral("step_once"), {});
        QVERIFY2(!step.contains(QStringLiteral("error")), qPrintable(step.value(QStringLiteral("error")).toString()));
        QCOMPARE(step.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        QCOMPARE(waitForRun(actor, step.value(QStringLiteral("runId")).toString())
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("completed"));
        QVERIFY(actor.undoBlockedByBarrier());

        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "edit B"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        QVERIFY(actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("edit A"));
        QVERIFY(actor.undoBlockedByBarrier());
        QVERIFY(!actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("edit A"));
    }

    void testPreExecutionStepFailureDoesNotEstablishUndoBarrier() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("unavailable_plugin");
        instance.moduleId = QStringLiteral("MissingAgentTestPlugin");
        instance.name = QStringLiteral("Before step");
        project->addModule(instance);

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "After edit"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        const QJsonObject result = actor.executeTool(QStringLiteral("step_once"), {});
        QVERIFY(result.contains(QStringLiteral("error")));
        QVERIFY(
            !result.value(QStringLiteral("details")).toObject().value(QStringLiteral("executionAttempted")).toBool());
        QVERIFY(!actor.undoBlockedByBarrier());
        QVERIFY(actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Before step"));
    }

    void testReadOnlyQueryBetweenEditsDoesNotEstablishUndoBarrier() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("readonly_barrier_node");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("original");
        project->addModule(instance);

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "edit A"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        const QJsonObject state = actor.executeTool(QStringLiteral("get_flow_state"), {});
        QVERIFY(state.contains(QStringLiteral("modules")));
        QVERIFY(!actor.undoBlockedByBarrier());
        QCOMPARE(actor
                     .executeTool(QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "edit B"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));

        QVERIFY(actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("edit A"));
        QVERIFY(!actor.undoBlockedByBarrier());
        QVERIFY(actor.undoLastAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("original"));
    }

    void testUnknownTool() {
        AgentActor actor;
        QJsonObject result = actor.executeTool("unknown_tool", QJsonObject());
        QVERIFY(result.contains("error"));
    }

    void testGetAvailablePlugins() {
        AgentActor actor;
        PluginManager& manager = PluginManager::instance();
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);
        const int undoCountBefore = actor.undoStack()->count();
        const int undoIndexBefore = actor.undoStack()->index();
        CameraManager& cameraManager = CameraManager::instance();
        const QList<ICameraPlugin*> cameraPluginsBefore = cameraManager.plugins();
        const QList<ICamera*> camerasBefore = cameraManager.allCameras();
        const QList<CameraStatus> cameraStatusesBefore = cameraManager.knownCameras();
        const int logCountBefore = Logger::instance().logs().size();
        const QList<PluginInfo> infos = manager.moduleInfos();
        const QList<PluginInfo> cameraInfos = manager.cameraInfos();
        QList<PluginInfo> allPluginInfos = infos;
        allPluginInfos.append(cameraInfos);
        QStringList loadedBefore;
        for (const PluginInfo& info : allPluginInfos) {
            if (manager.isPluginLoaded(info.name))
                loadedBefore.append(info.name);
        }
        QJsonObject result = actor.executeTool("get_available_plugins", QJsonObject());
        QVERIFY(result.contains("plugins"));
        const QJsonArray plugins = result.value(QStringLiteral("plugins")).toArray();
        const QJsonArray items = result.value(QStringLiteral("items")).toArray();
        const QJsonArray cameras = result.value(QStringLiteral("cameras")).toArray();
        QCOMPARE(plugins.size(), infos.size());
        QCOMPARE(items.size(), infos.size());
        QCOMPARE(cameras.size(), cameraInfos.size());
        for (int i = 0; i < infos.size(); ++i) {
            QCOMPARE(plugins.at(i).toString(), infos.at(i).name);
            const QJsonObject item = items.at(i).toObject();
            QCOMPARE(item.value(QStringLiteral("name")).toString(), infos.at(i).name);
            QVERIFY(!item.value(QStringLiteral("id")).toString().isEmpty());
            QCOMPARE(item.value(QStringLiteral("loaded")).toBool(), manager.isPluginLoaded(infos.at(i).name));
            QVERIFY(item.value(QStringLiteral("requirements")).toObject().contains(QStringLiteral("abi")));
            QVERIFY(item.value(QStringLiteral("executableActions")).isArray());
            QVERIFY(item.value(QStringLiteral("blockedActions")).isArray());
            const bool nameCollision = std::any_of(
                cameraInfos.cbegin(), cameraInfos.cend(),
                [name = infos.at(i).name](const PluginInfo& cameraInfo) { return cameraInfo.name == name; });
            if (nameCollision) {
                QVERIFY(item.value(QStringLiteral("executableActions")).toArray().isEmpty());
                const QJsonArray blocked = item.value(QStringLiteral("blockedActions")).toArray();
                for (const QString& action : {QStringLiteral("load"), QStringLiteral("unload")}) {
                    QVERIFY(blocked.contains(
                        QJsonObject{{QStringLiteral("action"), action},
                                    {QStringLiteral("reasonCode"), QStringLiteral("PLUGIN_NAME_AMBIGUOUS")}}));
                }
            }
        }
        for (int i = 0; i < cameraInfos.size(); ++i) {
            const QJsonObject camera = cameras.at(i).toObject();
            QCOMPARE(camera.value(QStringLiteral("name")).toString(), cameraInfos.at(i).name);
            QVERIFY(camera.value(QStringLiteral("category"))
                        .toString()
                        .contains(QStringLiteral("camera"), Qt::CaseInsensitive));
            QCOMPARE(camera.value(QStringLiteral("loaded")).toBool(), manager.isPluginLoaded(cameraInfos.at(i).name));
            QVERIFY(camera.value(QStringLiteral("platformCompatibility")).toString() == QStringLiteral("supported") ||
                    camera.value(QStringLiteral("platformCompatibility")).toString() == QStringLiteral("unsupported") ||
                    camera.value(QStringLiteral("platformCompatibility")).toString() == QStringLiteral("not_declared"));
            QVERIFY(camera.value(QStringLiteral("executableActions")).toArray().isEmpty());
            const QJsonArray blocked = camera.value(QStringLiteral("blockedActions")).toArray();
            const bool nameCollision = std::any_of(infos.cbegin(), infos.cend(), [&camera](const PluginInfo& info) {
                return info.name == camera.value(QStringLiteral("name")).toString();
            });
            const QString blockedReason = nameCollision ? QStringLiteral("PLUGIN_NAME_AMBIGUOUS")
                                                        : QStringLiteral("PLUGIN_LIFECYCLE_UNSUPPORTED");
            for (const QString& action : {QStringLiteral("load"), QStringLiteral("unload")}) {
                QVERIFY(blocked.contains(
                    QJsonObject{{QStringLiteral("action"), action}, {QStringLiteral("reasonCode"), blockedReason}}));
            }
        }
        QStringList loadedAfter;
        QList<PluginInfo> allPluginInfosAfter = manager.moduleInfos();
        allPluginInfosAfter.append(manager.cameraInfos());
        for (const PluginInfo& info : allPluginInfosAfter) {
            if (manager.isPluginLoaded(info.name))
                loadedAfter.append(info.name);
        }
        QCOMPARE(loadedAfter, loadedBefore);
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);
        QCOMPARE(actor.undoStack()->count(), undoCountBefore);
        QCOMPARE(actor.undoStack()->index(), undoIndexBefore);
        QCOMPARE(Logger::instance().logs().size(), logCountBefore);
        QCOMPARE(cameraManager.plugins(), cameraPluginsBefore);
        QCOMPARE(cameraManager.allCameras(), camerasBefore);
        const QList<CameraStatus> cameraStatusesAfter = cameraManager.knownCameras();
        QCOMPARE(cameraStatusesAfter.size(), cameraStatusesBefore.size());
        for (int i = 0; i < cameraStatusesBefore.size(); ++i) {
            QCOMPARE(cameraStatusesAfter.at(i).deviceId, cameraStatusesBefore.at(i).deviceId);
            QCOMPARE(cameraStatusesAfter.at(i).name, cameraStatusesBefore.at(i).name);
            QCOMPARE(cameraStatusesAfter.at(i).state, cameraStatusesBefore.at(i).state);
            QCOMPARE(cameraStatusesAfter.at(i).lastGrabTime, cameraStatusesBefore.at(i).lastGrabTime);
            QCOMPARE(cameraStatusesAfter.at(i).lastError, cameraStatusesBefore.at(i).lastError);
        }
    }

    // 知识库：read_documentation 同时检索内置（qrc）与用户目录文档
    void testReadDocumentationSearchesBuiltInAndUserKnowledge() {
        const QStringList builtInPaths = AgentActor::knowledgeDocumentPaths();
        QVERIFY(builtInPaths.contains(QStringLiteral(":/agent/knowledge/DEEPLUX.md")));
        QVERIFY(builtInPaths.contains(QStringLiteral(":/agent/knowledge/PLUGINS.md")));
        QVERIFY(builtInPaths.contains(QStringLiteral(":/agent/knowledge/USER_MANUAL.md")));

        // 三级模块标题必须独立检索，避免长分类在 2000 字截断后丢失靠后的模块。
        AgentActor actor;
        const QJsonObject pluginDoc = actor.executeTool("read_documentation", QJsonObject{{"topic", "TableOutPut"}});
        const QString pluginContent = pluginDoc.value(QStringLiteral("content")).toString();
        QVERIFY(pluginContent.contains(QStringLiteral("[来源: PLUGINS.md]")));
        QVERIFY(pluginContent.contains(QStringLiteral("以表格形式组织流程数据")));
        QVERIFY(pluginContent.contains(QStringLiteral("### `TableOutPut`")));

        const QJsonObject manualDoc =
            actor.executeTool("read_documentation", QJsonObject{{"topic", QStringLiteral("算法验收")}});
        const QString manualContent = manualDoc.value(QStringLiteral("content")).toString();
        QVERIFY(manualContent.contains(QStringLiteral("[来源: USER_MANUAL.md]")));
        QVERIFY(manualContent.contains(QStringLiteral("漏检和误检对比")));

        const QJsonObject defectDoc =
            actor.executeTool("read_documentation", QJsonObject{{"topic", "DefectDetection"}});
        const QString defectContent = defectDoc.value(QStringLiteral("content")).toString();
        QVERIFY(defectContent.contains(QStringLiteral("[来源: DEEPLUX.md]")));
        QVERIFY(defectContent.contains(QStringLiteral("[来源: USER_MANUAL.md]")));
        QVERIFY(defectContent.contains(QStringLiteral("[来源: PLUGINS.md]")));
        QVERIFY(defectContent.contains(QStringLiteral("HymsonVision3D SDK 未启用，缺陷检测不可用")));
        QVERIFY(defectContent.contains(QStringLiteral("AND FALSE")));
        QVERIFY(defectContent.contains(QStringLiteral("defect_point_cloud")));

        // 用户知识库目录由 DEEPLUX_APP_DATA_DIR 隔离（initTestCase 已设置临时目录）
        const QString userDir = AgentActor::userKnowledgeDir();
        QVERIFY(userDir.startsWith(m_appDataDir.path()));
        const QString docPath = QDir(userDir).filePath(QStringLiteral("SiteRecipe.md"));
        {
            QFile doc(docPath);
            QVERIFY(doc.open(QIODevice::WriteOnly | QIODevice::Text));
            doc.write("# Site\n\n## UniqueTopic-XYZ Calibration Recipe\n\nuser knowledge content 42\n");
            doc.close();
        }
        QVERIFY(AgentActor::knowledgeDocumentPaths().contains(docPath));

        const QJsonObject result = actor.executeTool("read_documentation", QJsonObject{{"topic", "uniquetopic-xyz"}});
        QVERIFY(result.value(QStringLiteral("error")).isUndefined());
        QCOMPARE(result.value(QStringLiteral("sections")).toInt(), 1);
        const QString content = result.value(QStringLiteral("content")).toString();
        QVERIFY(content.contains(QStringLiteral("user knowledge content 42")));
        QVERIFY(content.contains(QStringLiteral("SiteRecipe.md"))); // [来源: xxx] 标注

        // 未命中 topic 时不泄漏用户文档内容
        const QJsonObject miss = actor.executeTool("read_documentation", QJsonObject{{"topic", "no-such-topic-404"}});
        QVERIFY(
            !miss.value(QStringLiteral("content")).toString().contains(QStringLiteral("user knowledge content 42")));

        QVERIFY(QFile::remove(docPath));
    }

    void testKnowledgeDocumentToolsManageOnlyCopiedUserDocumentsAndPageSearch() {
        AgentActor actor;
        const QString userDir = AgentActor::userKnowledgeDir();
        const QString sourceDir = QDir(m_appDataDir.path()).filePath(QStringLiteral("knowledge-source"));
        QVERIFY(QDir().mkpath(sourceDir));
        const QString sourcePath = QDir(sourceDir).filePath(QStringLiteral("AgentRecipe.md"));
        const QByteArray sourceBytes =
            "# Recipe\n\n## UniqueSlice6Alpha\n\nalpha body\n\n## UniqueSlice6Beta\n\nbeta body\n\n"
            "## UniqueSlice6Gamma\n\ngamma body\n";
        {
            QFile source(sourcePath);
            QVERIFY(source.open(QIODevice::WriteOnly));
            QCOMPARE(source.write(sourceBytes), sourceBytes.size());
        }

        QJsonObject add = actor.executeTool(QStringLiteral("add_knowledge_document"),
                                            QJsonObject{{QStringLiteral("sourcePath"), sourcePath}});
        QCOMPARE(add.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString documentId = add.value(QStringLiteral("documentId")).toString();
        QCOMPARE(documentId, QStringLiteral("user:AgentRecipe.md"));
        QFile sourceAfter(sourcePath);
        QVERIFY(sourceAfter.open(QIODevice::ReadOnly));
        QCOMPARE(sourceAfter.readAll(), sourceBytes);
        sourceAfter.close();
        const QJsonObject duplicate = AgentActor::addKnowledgeDocument(sourcePath);
        QCOMPARE(duplicate.value(QStringLiteral("errorCode")).toString(), QStringLiteral("KNOWLEDGE_DOCUMENT_EXISTS"));
        QFile preserved(QDir(userDir).filePath(QStringLiteral("AgentRecipe.md")));
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), sourceBytes);
        preserved.close();

        const QJsonObject listing = actor.executeTool(QStringLiteral("list_knowledge_documents"), {});
        QVERIFY(listing.value(QStringLiteral("total")).toInt() > 0);
        bool listed = false;
        for (const QJsonValue& value : listing.value(QStringLiteral("documents")).toArray())
            listed = listed || value.toObject().value(QStringLiteral("documentId")).toString() == documentId;
        QVERIFY(listed);

        const QJsonObject preview = actor.executeTool(QStringLiteral("preview_knowledge_document"),
                                                      QJsonObject{{QStringLiteral("documentId"), documentId}});
        QCOMPARE(preview.value(QStringLiteral("content")).toString(), QString::fromUtf8(sourceBytes));
        QVERIFY(!preview.value(QStringLiteral("truncated")).toBool());
        const QString largePath = QDir(userDir).filePath(QStringLiteral("LargePreview.txt"));
        const QByteArray largeContent(512 * 1024 + 32, 'x');
        {
            QFile large(largePath);
            QVERIFY(large.open(QIODevice::WriteOnly));
            QCOMPARE(large.write(largeContent), largeContent.size());
        }
        const QJsonObject largePreview =
            actor.executeTool(QStringLiteral("preview_knowledge_document"),
                              QJsonObject{{QStringLiteral("documentId"), QStringLiteral("user:LargePreview.txt")}});
        QVERIFY(largePreview.value(QStringLiteral("truncated")).toBool());
        QCOMPARE(largePreview.value(QStringLiteral("content")).toString().size(), 512 * 1024);

        const QJsonObject first = actor.executeTool(
            QStringLiteral("read_documentation"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("UniqueSlice6")}, {QStringLiteral("limit"), 1}});
        QCOMPARE(first.value(QStringLiteral("sections")).toInt(), 1);
        QCOMPARE(first.value(QStringLiteral("totalSections")).toInt(), 3);
        QVERIFY(first.value(QStringLiteral("hasMore")).toBool());
        const QJsonObject second = actor.executeTool(
            QStringLiteral("read_documentation"), QJsonObject{{QStringLiteral("topic"), QStringLiteral("UniqueSlice6")},
                                                              {QStringLiteral("offset"), 1},
                                                              {QStringLiteral("limit"), 1}});
        QCOMPARE(second.value(QStringLiteral("sections")).toInt(), 1);
        QVERIFY(second.value(QStringLiteral("content")).toString().contains(QStringLiteral("UniqueSlice6Beta")));
        QVERIFY(!second.value(QStringLiteral("content")).toString().contains(QStringLiteral("UniqueSlice6Alpha")));
        const QJsonObject pastEnd = actor.executeTool(
            QStringLiteral("read_documentation"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("UniqueSlice6")}, {QStringLiteral("offset"), 20}});
        QCOMPARE(pastEnd.value(QStringLiteral("sections")).toInt(), 0);
        QCOMPARE(pastEnd.value(QStringLiteral("totalSections")).toInt(), 3);
        QVERIFY(pastEnd.value(QStringLiteral("content")).toString().isEmpty());
        QVERIFY(!pastEnd.value(QStringLiteral("hasMore")).toBool());
        const QJsonObject fractional = actor.executeTool(
            QStringLiteral("read_documentation"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("UniqueSlice6")}, {QStringLiteral("offset"), 1.5}});
        QCOMPARE(fractional.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        const QJsonObject negativeOffset = actor.executeTool(
            QStringLiteral("read_documentation"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("UniqueSlice6")}, {QStringLiteral("offset"), -1}});
        QCOMPARE(negativeOffset.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        const QJsonObject invalidLimit = actor.executeTool(
            QStringLiteral("read_documentation"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("UniqueSlice6")}, {QStringLiteral("limit"), 0}});
        QCOMPARE(invalidLimit.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        const QJsonObject oversizedLimit = actor.executeTool(
            QStringLiteral("read_documentation"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("UniqueSlice6")}, {QStringLiteral("limit"), 51}});
        QCOMPARE(oversizedLimit.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        const QJsonObject fractionalLimit = actor.executeTool(
            QStringLiteral("read_documentation"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("UniqueSlice6")}, {QStringLiteral("limit"), 1.5}});
        QCOMPARE(fractionalLimit.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));

        const QString externalPath = QDir(sourceDir).filePath(QStringLiteral("outside.txt"));
        {
            QFile external(externalPath);
            QVERIFY(external.open(QIODevice::WriteOnly));
            external.write("outside secret UniqueSymlinkSentinel");
        }
        const QString symlinkSourcePath = QDir(sourceDir).filePath(QStringLiteral("SourceLink.md"));
        QVERIFY(QFile::link(sourcePath, symlinkSourcePath));
        if (QFileInfo(symlinkSourcePath).isSymLink()) {
            const QJsonObject rejectedSourceSymlink = AgentActor::addKnowledgeDocument(symlinkSourcePath);
            QCOMPARE(rejectedSourceSymlink.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("KNOWLEDGE_DOCUMENT_SOURCE_INVALID"));
            QVERIFY(!QFileInfo::exists(QDir(userDir).filePath(QStringLiteral("SourceLink.md"))));
        } else {
            QWARN("Link creation yields a hard link on this platform; source symlink rejection is not observable");
        }
        QFile::remove(symlinkSourcePath);
        const QString linkPath = QDir(userDir).filePath(QStringLiteral("OutsideLink.txt"));
        QVERIFY(QFile::link(externalPath, linkPath));
        if (QFileInfo(linkPath).isSymLink()) {
            bool symlinkListed = false;
            const QJsonArray listedDocuments = actor.executeTool(QStringLiteral("list_knowledge_documents"), {})
                                                   .value(QStringLiteral("documents"))
                                                   .toArray();
            for (const QJsonValue& value : listedDocuments) {
                symlinkListed = symlinkListed || value.toObject().value(QStringLiteral("documentId")).toString() ==
                                                     QStringLiteral("user:OutsideLink.txt");
            }
            QVERIFY(!symlinkListed);
            const QJsonObject symlinkPreview =
                actor.executeTool(QStringLiteral("preview_knowledge_document"),
                                  QJsonObject{{QStringLiteral("documentId"), QStringLiteral("user:OutsideLink.txt")}});
            QCOMPARE(symlinkPreview.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("KNOWLEDGE_DOCUMENT_NOT_FOUND"));
            const QJsonObject symlinkSearch =
                actor.executeTool(QStringLiteral("read_documentation"),
                                  QJsonObject{{QStringLiteral("topic"), QStringLiteral("UniqueSymlinkSentinel")}});
            QVERIFY(!symlinkSearch.value(QStringLiteral("content")).toString().contains("outside secret"));
            QFile externalAfter(externalPath);
            QVERIFY(externalAfter.open(QIODevice::ReadOnly));
            QCOMPARE(externalAfter.readAll(), QByteArray("outside secret UniqueSymlinkSentinel"));
        } else {
            QWARN("Link creation yields a hard link on this platform; symlink exclusion is not observable");
        }
        QFile::remove(linkPath);

        const QJsonObject builtinRemove =
            actor.executeTool(QStringLiteral("remove_knowledge_document"),
                              QJsonObject{{QStringLiteral("documentId"), QStringLiteral("builtin:DEEPLUX.md")}});
        QCOMPARE(builtinRemove.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("KNOWLEDGE_DOCUMENT_READ_ONLY"));
        const QJsonObject pathTraversal =
            actor.executeTool(QStringLiteral("remove_knowledge_document"),
                              QJsonObject{{QStringLiteral("documentId"), QStringLiteral("user:../outside.txt")}});
        QCOMPARE(pathTraversal.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("KNOWLEDGE_DOCUMENT_NOT_FOUND"));
        const QJsonObject remove = actor.executeTool(QStringLiteral("remove_knowledge_document"),
                                                     QJsonObject{{QStringLiteral("documentId"), documentId}});
        QCOMPARE(remove.value(QStringLiteral("status")).toString(), QStringLiteral("removed"));
        QVERIFY(QFileInfo::exists(sourcePath));
        QVERIFY(
            !AgentActor::knowledgeDocumentPaths().contains(QDir(userDir).filePath(QStringLiteral("AgentRecipe.md"))));
    }

    void testKnowledgeDocumentConcurrentImportDoesNotOverwriteWinner() {
        const QString leftDir = QDir(m_appDataDir.path()).filePath(QStringLiteral("concurrent-left"));
        const QString rightDir = QDir(m_appDataDir.path()).filePath(QStringLiteral("concurrent-right"));
        QVERIFY(QDir().mkpath(leftDir));
        QVERIFY(QDir().mkpath(rightDir));
        const QString leftPath = QDir(leftDir).filePath(QStringLiteral("ConcurrentRecipe.txt"));
        const QString rightPath = QDir(rightDir).filePath(QStringLiteral("ConcurrentRecipe.txt"));
        for (const auto& pair :
             {qMakePair(leftPath, QByteArray("winner-left")), qMakePair(rightPath, QByteArray("winner-right"))}) {
            QFile file(pair.first);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(pair.second), pair.second.size());
        }

        QJsonObject leftResult;
        QJsonObject rightResult;
        std::thread left([&]() { leftResult = AgentActor::addKnowledgeDocument(leftPath); });
        std::thread right([&]() { rightResult = AgentActor::addKnowledgeDocument(rightPath); });
        left.join();
        right.join();
        const bool leftWon = leftResult.value(QStringLiteral("status")).toString() == QStringLiteral("added");
        const bool rightWon = rightResult.value(QStringLiteral("status")).toString() == QStringLiteral("added");
        QVERIFY(leftWon != rightWon);
        const QJsonObject& loser = leftWon ? rightResult : leftResult;
        QCOMPARE(loser.value(QStringLiteral("errorCode")).toString(), QStringLiteral("KNOWLEDGE_DOCUMENT_EXISTS"));
        QFile stored(QDir(AgentActor::userKnowledgeDir()).filePath(QStringLiteral("ConcurrentRecipe.txt")));
        QVERIFY(stored.open(QIODevice::ReadOnly));
        const QByteArray storedBytes = stored.readAll();
        QCOMPARE(storedBytes, leftWon ? QByteArray("winner-left") : QByteArray("winner-right"));
    }

    void testGetFlowStateNoProject() {
        AgentActor actor;
        QJsonObject result = actor.executeTool("get_flow_state", QJsonObject());
        QVERIFY(result.contains("error"));
    }

    void testG4FlowStateReflectsProjectModel() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        project->setName(QStringLiteral("Flow state fixture"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "source"}})
                     .contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "MeasureCircle"}, {"instanceName", "measure"}})
                     .contains("error"));
        ModuleInstance measure = *project->moduleById(QStringLiteral("measure"));
        measure.name = QStringLiteral("Renamed measure");
        measure.posX = 120;
        measure.posY = 45;
        measure.note = QStringLiteral("inspection note");
        measure.enabled = false;
        measure.breakpoint = true;
        project->updateModule(measure.id, measure);
        QVERIFY(project->moveModule(measure.id, 0));
        QVERIFY(
            !actor
                 .executeTool(
                     "connect_modules",
                     QJsonObject{{"fromId", "source"}, {"toId", "measure"}, {"fromPort", "image"}, {"toPort", "image"}})
                 .contains("error"));
        ModuleConnection control;
        control.fromModuleId = QStringLiteral("source");
        control.fromPort = QStringLiteral("next");
        control.toModuleId = QStringLiteral("measure");
        control.toPort = QStringLiteral("control");
        control.edgeType = QStringLiteral("control");
        project->addConnection(control); // 本片只验证已有控制边的查询，不宣称 Agent 可创建它

        const QJsonObject state = actor.executeTool("get_flow_state", {});
        const QJsonObject identity = state.value("project").toObject();
        QCOMPARE(identity.value("id").toString(), project->id());
        QCOMPARE(identity.value("name").toString(), project->name());
        QVERIFY(identity.value("isModified").toBool());
        QCOMPARE(state.value("executionScope").toString(), QStringLiteral("project_modules"));
        QVERIFY(!state.value("flowSwitchSupported").toBool());
        const QJsonArray implicitFlows = state.value("flows").toArray();
        QCOMPARE(implicitFlows.size(), 1);
        QCOMPARE(implicitFlows.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("main"));
        QCOMPARE(implicitFlows.first().toObject().value(QStringLiteral("nodeIds")).toArray().size(), 2);
        const QJsonArray modules = state.value("modules").toArray();
        QCOMPARE(modules.size(), 2);
        const QJsonObject first = modules.first().toObject();
        QCOMPARE(first.value("id").toString(), QStringLiteral("measure"));
        QCOMPARE(first.value("name").toString(), measure.name);
        QCOMPARE(first.value("order").toInt(), 0);
        QCOMPARE(first.value("posX").toInt(), 120);
        QCOMPARE(first.value("posY").toInt(), 45);
        QCOMPARE(first.value("note").toString(), measure.note);
        QVERIFY(!first.value("enabled").toBool());
        QVERIFY(first.value("breakpoint").toBool());
        QCOMPARE(first.value("ports").toObject().value("inputs").toArray().first().toObject().value("id").toString(),
                 QStringLiteral("image"));
        const QJsonObject connection = state.value("connections").toArray().first().toObject();
        QCOMPARE(connection.value("from").toString(), QStringLiteral("source"));
        QCOMPARE(connection.value("to").toString(), QStringLiteral("measure"));
        QCOMPARE(connection.value("fromPort").toString(), QStringLiteral("image"));
        QCOMPARE(connection.value("toPort").toString(), QStringLiteral("image"));
        QCOMPARE(connection.value("edgeType").toString(), QStringLiteral("data"));
        const QJsonObject controlState = state.value("connections").toArray().at(1).toObject();
        QCOMPARE(controlState.value("fromPort").toString(), QStringLiteral("next"));
        QCOMPARE(controlState.value("toPort").toString(), QStringLiteral("control"));
        QCOMPARE(controlState.value("edgeType").toString(), QStringLiteral("control"));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("flow-state.dproj"));
        QCOMPARE(actor.executeTool("save_project", QJsonObject{{"path", path}}).value("status").toString(),
                 QStringLiteral("saved"));
        QVERIFY(!actor.executeTool("open_project", QJsonObject{{"path", path}}).contains("error"));
        const QJsonObject reopened = actor.executeTool("get_flow_state", {});
        QCOMPARE(reopened.value("modules").toArray(), modules);
        QCOMPARE(reopened.value("connections").toArray(), state.value("connections").toArray());
        QCOMPARE(reopened.value("project").toObject().value("filePath").toString(), path);
        QVERIFY(!reopened.value("project").toObject().value("isModified").toBool());
    }

    void testG4FlowSelectorFailsClosedAndStateReportsStoredFlows() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance first;
        first.id = QStringLiteral("flow_node_a");
        first.moduleId = QStringLiteral("GrabImage");
        project->addModule(first);
        ModuleInstance second;
        second.id = QStringLiteral("flow_node_b");
        second.moduleId = QStringLiteral("MeasureCircle");
        project->addModule(second);
        ProjectFlow primary{QStringLiteral("primary"), QStringLiteral("Primary"), {first.id}};
        ProjectFlow secondary{
            QStringLiteral("secondary"), QStringLiteral("Secondary"), {first.id, second.id, QStringLiteral("orphan")}};
        project->setFlows({primary, secondary});

        const QJsonObject before = project->toJson();
        const int undoIndex = actor.undoStack()->index();
        RunEngine& engine = RunEngine::instance();
        const int runsBefore = engine.totalRuns();
        const QString lastRunIdBefore = engine.lastRunId();
        const QString cycleSessionBefore = engine.cycleSessionId();
        const RunState engineStateBefore = engine.state();
        const QList<ModuleBase*> engineModulesBefore = engine.modules();
        QSignalSpy runStartedSpy(&engine, &RunEngine::runStarted);
        QSignalSpy cycleStartedSpy(&engine, &RunEngine::cycleStarted);
        for (const QString& flowId : {QStringLiteral("main"), QStringLiteral("secondary")}) {
            for (const QString& tool :
                 {QStringLiteral("run_flow"), QStringLiteral("validate_flow"), QStringLiteral("get_flow_state"),
                  QStringLiteral("step_once"), QStringLiteral("resume_flow"), QStringLiteral("reset_step")}) {
                QJsonObject params{{QStringLiteral("flowId"), flowId}};
                if (tool == QLatin1String("run_flow"))
                    params.insert(QStringLiteral("mode"), QStringLiteral("once"));
                const QJsonObject result = actor.executeTool(tool, params);
                QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
                QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("FLOW_UNAVAILABLE"));
                QVERIFY(!result.value(QStringLiteral("error")).toString().isEmpty());
                QCOMPARE(result.value(QStringLiteral("retryable")).toBool(), false);
                QCOMPARE(result.value(QStringLiteral("flowId")).toString(), flowId);
                QCOMPARE(result.value(QStringLiteral("flowSwitchSupported")).toBool(), false);
            }
            for (const QString& mode : {QStringLiteral("once"), QStringLiteral("cycle")}) {
                const QJsonObject result =
                    actor.executeTool(QStringLiteral("run_flow"),
                                      QJsonObject{{QStringLiteral("flowId"), flowId}, {QStringLiteral("mode"), mode}});
                QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
                QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("FLOW_UNAVAILABLE"));
                QCOMPARE(result.value(QStringLiteral("flowId")).toString(), flowId);
            }
        }
        QCOMPARE(project->toJson(), before);
        QCOMPARE(actor.undoStack()->index(), undoIndex);
        QCOMPARE(engine.totalRuns(), runsBefore);
        QCOMPARE(engine.lastRunId(), lastRunIdBefore);
        QCOMPARE(engine.cycleSessionId(), cycleSessionBefore);
        QCOMPARE(engine.state(), engineStateBefore);
        QCOMPARE(engine.modules(), engineModulesBefore);
        QCOMPARE(runStartedSpy.count(), 0);
        QCOMPARE(cycleStartedSpy.count(), 0);

        const QJsonObject rejectedBatch = actor.executeTools(
            {{QStringLiteral("selector_call"), QStringLiteral("get_flow_state"),
              QJsonObject{{QStringLiteral("flowId"), QStringLiteral("secondary")}}},
             {QStringLiteral("rename_call"), QStringLiteral("rename_module"),
              QJsonObject{{QStringLiteral("instanceId"), first.id}, {QStringLiteral("name"), "Changed"}}}},
            QStringLiteral("flow selector preflight"));
        QCOMPARE(rejectedBatch.value(QStringLiteral("failedCallId")).toString(), QStringLiteral("selector_call"));
        const QJsonArray rejectedResults = rejectedBatch.value(QStringLiteral("results")).toArray();
        QCOMPARE(rejectedResults.size(), 2);
        QCOMPARE(rejectedResults.at(0)
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("FLOW_UNAVAILABLE"));
        QCOMPARE(rejectedResults.at(1)
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("failedCallId"))
                     .toString(),
                 QStringLiteral("selector_call"));
        QCOMPARE(project->moduleById(first.id)->name, QString());

        const QJsonObject invalidBeforeSelector =
            actor.executeTools({{QStringLiteral("invalid_flow_id"), QStringLiteral("get_flow_state"),
                                 QJsonObject{{QStringLiteral("flowId"), 7}}},
                                {QStringLiteral("valid_selector"), QStringLiteral("validate_flow"),
                                 QJsonObject{{QStringLiteral("flowId"), QStringLiteral("secondary")}}}},
                               QStringLiteral("flow selector invalid param priority"));
        QCOMPARE(invalidBeforeSelector.value(QStringLiteral("failedCallId")).toString(),
                 QStringLiteral("invalid_flow_id"));
        QCOMPARE(invalidBeforeSelector.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QCOMPARE(invalidBeforeSelector.value(QStringLiteral("results"))
                     .toArray()
                     .at(1)
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));

        const QJsonObject state = actor.executeTool(QStringLiteral("get_flow_state"), {});
        QCOMPARE(state.value(QStringLiteral("executionScope")).toString(), QStringLiteral("project_modules"));
        const QJsonArray flows = state.value(QStringLiteral("flows")).toArray();
        const QJsonArray expectedFlows{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("primary")},
                        {QStringLiteral("name"), QStringLiteral("Primary")},
                        {QStringLiteral("nodeIds"), QJsonArray{first.id}}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("secondary")},
                        {QStringLiteral("name"), QStringLiteral("Secondary")},
                        {QStringLiteral("nodeIds"), QJsonArray{first.id, second.id, QStringLiteral("orphan")}}}};
        QCOMPARE(flows, expectedFlows);

        for (const QJsonValue& invalid : {QJsonValue(7), QJsonValue(QString()), QJsonValue(QStringLiteral("  "))}) {
            const QJsonObject rejected =
                actor.executeTool(QStringLiteral("get_flow_state"), QJsonObject{{QStringLiteral("flowId"), invalid}});
            QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
            QCOMPARE(project->toJson(), before);
        }
        ProjectManager::instance().closeProject();
    }

    void testReadDocumentationUsesBundledKnowledge() {
        AgentActor actor;
        QJsonObject result = actor.executeTool("read_documentation", QJsonObject{{"topic", "all"}});
        QVERIFY(!result.contains("error"));
        QVERIFY(result.value("content").toString().contains("DeepLux"));
        QVERIFY(!result.value("content").toString().contains("Knowledge base not found"));
    }

    void testRequiredAndEnumParametersAreValidatedBeforeExecution() {
        AgentActor actor;
        QJsonObject missingName = actor.executeTool("create_project", QJsonObject());
        QVERIFY(missingName.contains("error"));
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);

        QJsonObject invalidMode = actor.executeTool("run_flow", QJsonObject{{"mode", "forever"}});
        QVERIFY(invalidMode.contains("error"));
    }

    void testConnectModulesAddsSingleConnection() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance first;
        first.id = "a";
        first.moduleId = "GrabImage";
        first.name = "A";
        project->addModule(first);

        ModuleInstance second;
        second.id = "b";
        second.moduleId = "MeasureCircle";
        second.name = "B";
        project->addModule(second);

        QJsonObject result = actor.executeTool("connect_modules", QJsonObject{{"fromId", "a"}, {"toId", "b"}});
        QCOMPARE(result["status"].toString(), QString("connected"));
        QCOMPARE(project->connections().size(), 1);
    }

    void testG4ParallelPortsDisconnectPreciselyAndUndo() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "MeasurementInput"}, {"instanceName", "input"}})
                     .contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "FitCircle"}, {"instanceName", "fit"}})
                     .contains("error"));
        QVERIFY(
            !actor.executeTool("add_module", QJsonObject{{"plugin", "MeasurementInput"}, {"instanceName", "input2"}})
                 .contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "FitCircle"}, {"instanceName", "fit2"}})
                     .contains("error"));
        const QJsonObject imageEdge{{"fromId", "input"}, {"fromPort", "image"}, {"toId", "fit"}, {"toPort", "image"}};
        const QJsonObject pointsEdge{
            {"fromId", "input"}, {"fromPort", "fit_points"}, {"toId", "fit"}, {"toPort", "fit_points"}};
        const QJsonObject trailingEdge{
            {"fromId", "input2"}, {"fromPort", "image"}, {"toId", "fit2"}, {"toPort", "image"}};
        QCOMPARE(actor.executeTool("connect_modules", imageEdge).value("status").toString(),
                 QStringLiteral("connected"));
        QCOMPARE(actor.executeTool("connect_modules", pointsEdge).value("status").toString(),
                 QStringLiteral("connected"));
        QCOMPARE(actor.executeTool("connect_modules", trailingEdge).value("status").toString(),
                 QStringLiteral("connected"));
        const QJsonArray connectionsBefore = project->toJson().value(QStringLiteral("connections")).toArray();
        QCOMPARE(connectionsBefore.size(), 3);
        QCOMPARE(connectionsBefore.at(1).toObject().value(QStringLiteral("fromPort")).toString(),
                 QStringLiteral("fit_points"));
        QCOMPARE(actor.executeTool("disconnect_modules", QJsonObject{{"fromId", "input"}, {"toId", "fit"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        QCOMPARE(project->connections().size(), 3);
        QCOMPARE(actor.executeTool("disconnect_modules", pointsEdge).value("status").toString(),
                 QStringLiteral("disconnected"));
        const QJsonArray connectionsDisconnected = project->toJson().value(QStringLiteral("connections")).toArray();
        QCOMPARE(connectionsDisconnected.size(), 2);
        QCOMPARE(connectionsDisconnected.at(0), connectionsBefore.at(0));
        QCOMPARE(connectionsDisconnected.at(1), connectionsBefore.at(2));
        actor.undoStack()->undo();
        QCOMPARE(project->toJson().value(QStringLiteral("connections")).toArray(), connectionsBefore);
        actor.undoStack()->redo();
        QCOMPARE(project->toJson().value(QStringLiteral("connections")).toArray(), connectionsDisconnected);
        actor.undoStack()->undo();
        QCOMPARE(project->toJson().value(QStringLiteral("connections")).toArray(), connectionsBefore);

        QTemporaryDir savedProjectDir;
        QVERIFY(savedProjectDir.isValid());
        const QString savedPath = savedProjectDir.filePath(QStringLiteral("disconnect-undo-order.dproj"));
        QVERIFY(project->save(savedPath));
        Project reopened;
        QVERIFY(reopened.load(savedPath));
        QCOMPARE(reopened.toJson().value(QStringLiteral("connections")).toArray(), connectionsBefore);
    }

    void testG4FlowStateResolvesLegacyEdgeTypesWithoutMutatingProject() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance source;
        source.id = QStringLiteral("legacy_source");
        source.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(source);
        ModuleInstance target;
        target.id = QStringLiteral("legacy_target");
        target.moduleId = QStringLiteral("FitCircle");
        project->addModule(target);
        ModuleInstance camera;
        camera.id = QStringLiteral("legacy_camera");
        camera.moduleId = QStringLiteral("LegacyCamera");
        project->addModule(camera);

        ModuleConnection legacyData;
        legacyData.fromModuleId = source.id;
        legacyData.toModuleId = target.id;
        legacyData.fromPort = QStringLiteral("fit_points");
        legacyData.toPort = QStringLiteral("fit_points");
        project->addConnection(legacyData);
        ModuleConnection legacyControl;
        legacyControl.fromModuleId = source.id;
        legacyControl.toModuleId = target.id;
        legacyControl.fromPort = QStringLiteral("next");
        legacyControl.toPort = QStringLiteral("control");
        project->addConnection(legacyControl);
        ModuleConnection legacyDefaultPorts;
        legacyDefaultPorts.fromModuleId = source.id;
        legacyDefaultPorts.toModuleId = target.id;
        project->addConnection(legacyDefaultPorts);
        ModuleConnection explicitControl;
        explicitControl.fromModuleId = source.id;
        explicitControl.toModuleId = target.id;
        explicitControl.fromPort = QStringLiteral("next");
        explicitControl.toPort = QStringLiteral("image");
        explicitControl.edgeType = QStringLiteral("data");
        project->addConnection(explicitControl);
        ModuleConnection missingCameraPorts;
        missingCameraPorts.fromModuleId = camera.id;
        missingCameraPorts.toModuleId = target.id;
        missingCameraPorts.fromPort = QStringLiteral("next");
        missingCameraPorts.toPort = QStringLiteral("control");
        project->addConnection(missingCameraPorts);
        ModuleConnection unresolved;
        unresolved.fromModuleId = QStringLiteral("missing_source");
        unresolved.toModuleId = QStringLiteral("missing_target");
        project->addConnection(unresolved);

        const QJsonArray edges = actor.executeTool("get_flow_state", {}).value("connections").toArray();
        QCOMPARE(edges.size(), 6);
        QCOMPARE(edges.at(0).toObject().value("edgeType").toString(), QString());
        QCOMPARE(edges.at(0).toObject().value("effectiveEdgeType").toString(), QStringLiteral("data"));
        QCOMPARE(edges.at(0).toObject().value("edgeTypeResolution").toString(), QStringLiteral("inferred"));
        QCOMPARE(edges.at(1).toObject().value("effectiveEdgeType").toString(), QStringLiteral("control"));
        QCOMPARE(edges.at(1).toObject().value("edgeTypeResolution").toString(), QStringLiteral("inferred"));
        QCOMPARE(edges.at(2).toObject().value("effectiveEdgeType").toString(), QStringLiteral("data"));
        QCOMPARE(edges.at(2).toObject().value("edgeTypeResolution").toString(), QStringLiteral("inferred"));
        QCOMPARE(edges.at(3).toObject().value("effectiveEdgeType").toString(), QStringLiteral("data"));
        QCOMPARE(edges.at(3).toObject().value("edgeTypeResolution").toString(), QStringLiteral("explicit"));
        QCOMPARE(edges.at(4).toObject().value("edgeTypeResolution").toString(), QStringLiteral("unavailable"));
        QCOMPARE(edges.at(5).toObject().value("edgeTypeResolution").toString(), QStringLiteral("unavailable"));
        const QList<ModuleConnection> unchanged = project->connections();
        QCOMPARE(unchanged.at(0).edgeType, QString());
        QCOMPARE(unchanged.at(1).edgeType, QString());
        QCOMPARE(unchanged.at(2).fromPort, QString());
        QCOMPARE(unchanged.at(2).toPort, QString());
        QCOMPARE(unchanged.at(3).edgeType, QStringLiteral("data"));
        QVERIFY(!PluginManager::instance().pluginInfo(QStringLiteral("LegacyCamera")).portsDeclared);
    }

    void testG4LegacyControlEdgeTypeAgreesAcrossAgentEngineAndCanvas() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        const PluginInfo sourceInfo = PluginManager::instance().pluginInfo(QStringLiteral("条件分支"));
        const PluginInfo targetInfo = PluginManager::instance().pluginInfo(QStringLiteral("Parallel"));
        QVERIFY(sourceInfo.portsDeclared);
        QVERIFY(targetInfo.portsDeclared);
        const auto hasControlPort = [](const QList<PortSpec>& ports, const QString& portId) {
            for (const PortSpec& port : ports) {
                if (port.id == portId && port.control)
                    return true;
            }
            return false;
        };
        QVERIFY(hasControlPort(sourceInfo.outputPorts, QStringLiteral("true")));
        QVERIFY(hasControlPort(targetInfo.inputPorts, QStringLiteral("control")));

        ModuleInstance source;
        source.id = QStringLiteral("same_source");
        source.moduleId = QStringLiteral("条件分支");
        source.name = source.id;
        project->addModule(source);
        ModuleInstance target;
        target.id = QStringLiteral("same_target");
        target.moduleId = QStringLiteral("Parallel");
        target.name = target.id;
        project->addModule(target);

        ModuleConnection legacyControl;
        legacyControl.fromModuleId = source.id;
        legacyControl.toModuleId = target.id;
        legacyControl.fromPort = QStringLiteral("true");
        legacyControl.toPort = QStringLiteral("control");
        project->addConnection(legacyControl);
        QVERIFY(project->connections().first().edgeType.isEmpty());
        const QJsonObject projectSnapshot = project->toJson();

        AgentActor actor;
        const QJsonArray queriedConnections = actor.executeTool("get_flow_state", {}).value("connections").toArray();
        QCOMPARE(queriedConnections.size(), 1);
        QCOMPARE(queriedConnections.first().toObject().value("effectiveEdgeType").toString(),
                 QStringLiteral("control"));
        QCOMPARE(queriedConnections.first().toObject().value("edgeTypeResolution").toString(),
                 QStringLiteral("inferred"));

        FlowCanvas canvas;
        canvas.loadFromProject(project);
        QCOMPARE(canvas.m_connections.size(), 1);
        QVERIFY(canvas.m_connections.first()->isControlEdge());
        QCOMPARE(canvas.m_connections.first()->edgeType(), QStringLiteral("control"));

        QVERIFY(RunEngine::instance().loadProject(project));
        QCOMPARE(RunEngine::instance().modules().size(), 2);
        RunEngine::instance().clearModules();
        QCOMPARE(project->toJson(), projectSnapshot);
        QVERIFY(project->connections().first().edgeType.isEmpty());
    }

    void testG4ControlAndDataEdgesStayDistinct() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        QVERIFY(!actor
                     .executeTool("add_module",
                                  QJsonObject{{"plugin", "com.deeplux.plugin.if"}, {"instanceName", "condition"}})
                     .contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "Parallel"}, {"instanceName", "parallel"}})
                     .contains("error"));

        const QJsonObject controlEdge{
            {"fromId", "condition"}, {"fromPort", "true"}, {"toId", "parallel"}, {"toPort", "control"}};
        const QJsonObject dataEdge{
            {"fromId", "condition"}, {"fromPort", "image"}, {"toId", "parallel"}, {"toPort", "image"}};
        QCOMPARE(actor.executeTool("connect_modules", controlEdge).value("edgeType").toString(),
                 QStringLiteral("control"));
        QCOMPARE(actor.executeTool("connect_modules", dataEdge).value("edgeType").toString(), QStringLiteral("data"));
        QCOMPARE(project->connections().size(), 2);
        const int undoCountBeforeDuplicate = actor.undoStack()->count();
        const int undoIndexBeforeDuplicate = actor.undoStack()->index();
        const QJsonObject duplicate = actor.executeTool("connect_modules", controlEdge);
        QCOMPARE(duplicate.value("status").toString(), QStringLiteral("already_connected"));
        QCOMPARE(duplicate.value("edgeType").toString(), QStringLiteral("control"));
        QCOMPARE(project->connections().size(), 2);
        QCOMPARE(actor.undoStack()->count(), undoCountBeforeDuplicate);
        QCOMPARE(actor.undoStack()->index(), undoIndexBeforeDuplicate);

        const QJsonObject state = actor.executeTool("get_flow_state", {});
        QCOMPARE(state.value("connections").toArray().size(), 2);
        const QJsonArray modules = state.value("modules").toArray();
        QCOMPARE(modules.size(), 2);
        const QJsonArray parallelInputs = modules.at(1).toObject().value("ports").toObject().value("inputs").toArray();
        bool foundControl = false;
        for (const QJsonValue& value : parallelInputs) {
            const QJsonObject port = value.toObject();
            if (port.value("id").toString() == QLatin1String("control")) {
                foundControl = true;
                QCOMPARE(port.value("joinPolicy").toString(), QStringLiteral("all"));
            }
        }
        QVERIFY(foundControl);

        QCOMPARE(actor
                     .executeTool("connect_modules", QJsonObject{{"fromId", "condition"},
                                                                 {"fromPort", "image"},
                                                                 {"toId", "parallel"},
                                                                 {"toPort", "control"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(project->connections().size(), 2);
        QCOMPARE(actor.executeTool("disconnect_modules", controlEdge).value("edgeType").toString(),
                 QStringLiteral("control"));
        QCOMPARE(project->connections().size(), 1);
        QCOMPARE(project->connections().first().edgeType, QStringLiteral("data"));
        actor.undoStack()->undo();
        QCOMPARE(project->connections().size(), 2);
        QTemporaryDir savedProjectDir;
        QVERIFY(savedProjectDir.isValid());
        const QString savedPath = savedProjectDir.filePath(QStringLiteral("control-data.dproj"));
        QVERIFY(project->save(savedPath));
        Project reopened;
        QVERIFY(reopened.load(savedPath));
        QCOMPARE(reopened.connections().size(), 2);
        QMap<QString, QString> reopenedEdgeTypes;
        for (const ModuleConnection& connection : reopened.connections())
            reopenedEdgeTypes.insert(connection.fromPort + QLatin1Char('/') + connection.toPort, connection.edgeType);
        QCOMPARE(reopenedEdgeTypes.value(QStringLiteral("true/control")), QStringLiteral("control"));
        QCOMPARE(reopenedEdgeTypes.value(QStringLiteral("image/image")), QStringLiteral("data"));
        actor.undoStack()->redo();
        QCOMPARE(project->connections().size(), 1);
        QCOMPARE(project->connections().first().edgeType, QStringLiteral("data"));
    }

    void testProjectSwitchClearsUndoHistory() {
        AgentActor actor;
        QJsonObject created = actor.executeTool("create_project", QJsonObject{{"name", "first"}});
        QCOMPARE(created.value("status").toString(), QStringLiteral("created"));
        QVERIFY(!actor.undoStack()->canUndo());

        QJsonObject added =
            actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "first_grab"}});
        QVERIFY(!added.contains("error"));
        QVERIFY(actor.undoStack()->canUndo());

        QVERIFY(actor.executeTool("open_project", QJsonObject{{"path", "/path/that/does/not/exist.dproj"}})
                    .contains("error"));
        QVERIFY(actor.undoStack()->canUndo());

        Project* second = ProjectManager::instance().newProject();
        QVERIFY(second != nullptr);
        QVERIFY(!actor.undoStack()->canUndo());
        actor.undoStack()->undo();
        QVERIFY(second->modules().isEmpty());
    }

    void testAddModuleRejectsUnknownAndDuplicateIds() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        QVERIFY(actor.executeTool("add_module", QJsonObject{{"plugin", "MissingPlugin"}}).contains("error"));
        QCOMPARE(project->modules().size(), 0);

        QJsonObject first =
            actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "duplicate"}});
        QVERIFY(!first.contains("error"));
        QVERIFY(first.value("instanceId").toString() == QStringLiteral("duplicate"));
        QVERIFY(actor.executeTool("add_module", QJsonObject{{"plugin", "MeasureCircle"}, {"instanceName", "duplicate"}})
                    .contains("error"));
        QCOMPARE(project->modules().size(), 1);

        QJsonObject generatedA = actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}});
        QJsonObject generatedB = actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}});
        QVERIFY(!generatedA.contains("error"));
        QVERIFY(!generatedB.contains("error"));
        QVERIFY(!generatedA.value("instanceId").toString().isEmpty());
        QVERIFY(generatedA.value("instanceId") != generatedB.value("instanceId"));
    }

    void testBatchExecutionStopsOnFirstError() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        QList<QPair<QString, QJsonObject>> tools;
        tools.append(
            qMakePair(QString("add_module"), QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "grab_1"}}));
        tools.append(qMakePair(QString("set_param"),
                               QJsonObject{{"instanceId", "missing"}, {"key", "threshold"}, {"value", 1}}));
        tools.append(
            qMakePair(QString("add_module"), QJsonObject{{"plugin", "SaveImage"}, {"instanceName", "save_1"}}));

        QJsonObject result = actor.executeTools(tools, "Partial failure");
        QCOMPARE(result["status"].toString(), QString("partial_failed"));
        QCOMPARE(result["executed"].toInt(), 1);
        QCOMPARE(result["failedTool"].toString(), QStringLiteral("set_param"));
        QCOMPARE(result["errorCode"].toString(), QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(project->modules().size(), 1);

        // B1：未执行调用必须补齐跳过结果，且每个 tool_call 恰好一条结果
        const QJsonArray results = result["results"].toArray();
        QCOMPARE(results.size(), tools.size());
        QCOMPARE(AgentActor::resultSucceeded(results[0].toObject()["result"].toObject()), true);
        QCOMPARE(AgentActor::resultSucceeded(results[1].toObject()["result"].toObject()), false);
        const QJsonObject skipped = results[2].toObject()["result"].toObject();
        QCOMPARE(skipped["errorCode"].toString(), QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(AgentActor::resultSucceeded(skipped), false);
    }

    // B2：批量执行前的逐项门禁（用于中止/工程切换/等待期间降权）
    void testBatchGateStopsBeforeExecution() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        QList<QPair<QString, QJsonObject>> tools;
        tools.append(
            qMakePair(QString("add_module"), QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "gate_first"}}));
        tools.append(
            qMakePair(QString("add_module"), QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "gate_second"}}));
        tools.append(
            qMakePair(QString("add_module"), QJsonObject{{"plugin", "SaveImage"}, {"instanceName", "gate_third"}}));

        int gateCalls = 0;
        const auto gate = [&gateCalls](int index, const QString&, const QJsonObject&) -> QString {
            ++gateCalls;
            return index == 1 ? QStringLiteral("PERMISSION_DENIED") : QString();
        };
        const QJsonObject result = actor.executeTools(tools, "Gate", gate);

        QCOMPARE(result["status"].toString(), QStringLiteral("partial_failed"));
        QCOMPARE(result["executed"].toInt(), 1);
        QCOMPARE(result["results"].toArray()[1].toObject()["result"].toObject()["errorCode"].toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(gateCalls, 2);
        QCOMPARE(project->modules().size(), 1);
        const QJsonObject blocked = result["results"].toArray()[1].toObject()["result"].toObject();
        QCOMPARE(blocked["errorCode"].toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(AgentActor::resultSucceeded(blocked), false);
        const QJsonObject skipped = result["results"].toArray()[2].toObject()["result"].toObject();
        QCOMPARE(skipped["errorCode"].toString(), QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(AgentActor::resultSucceeded(skipped), false);
    }

    void testExecuteToolsPrevalidationFailsClosedForAllCalls() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        QList<QPair<QString, QJsonObject>> tools;
        tools.append(
            qMakePair(QString("add_module"), QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "must_not_add"}}));
        // 非法 enum：参数预校验在本批任何调用执行前失败
        tools.append(qMakePair(QString("get_execution_logs"), QJsonObject{{"level", "not-a-level"}}));
        tools.append(
            qMakePair(QString("add_module"), QJsonObject{{"plugin", "SaveImage"}, {"instanceName", "must_not_add_2"}}));

        const QJsonObject result = actor.executeTools(tools, "Prevalidation failure");
        QCOMPARE(result["status"].toString(), QStringLiteral("partial_failed"));
        QCOMPARE(result["executed"].toInt(), 0);
        QCOMPARE(project->modules().size(), 0);

        const QJsonArray results = result["results"].toArray();
        QCOMPARE(results.size(), tools.size());
        for (int i = 0; i < results.size(); ++i) {
            QCOMPARE(results[i].toObject()["tool"].toString(), tools[i].first);
            QCOMPARE(AgentActor::resultSucceeded(results[i].toObject()["result"].toObject()), false);
        }
        QCOMPARE(results[1].toObject()["result"].toObject()["errorCode"].toString(), QStringLiteral("INVALID_PARAM"));
        QCOMPARE(results[0].toObject()["result"].toObject()["errorCode"].toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(results[2].toObject()["result"].toObject()["errorCode"].toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
    }

    void testResultSucceededUsesSuccessField() {
        // B1 单源判定：success=false 即使没有 error 也不能算成功
        QCOMPARE(AgentActor::resultSucceeded(QJsonObject{{"success", false}}), false);
        QCOMPARE(AgentActor::resultSucceeded(QJsonObject{{"success", true}, {"error", "diagnostic only"}}), true);
        QCOMPARE(AgentActor::resultSucceeded(QJsonObject{{"error", "boom"}}), false);
        QCOMPARE(AgentActor::resultSucceeded(QJsonObject{{"modules", QJsonArray()}}), true);
        QCOMPARE(AgentActor::resultFailureCode(QJsonObject{{"success", false}, {"code", "run_failed"}}),
                 QStringLiteral("run_failed"));
        QCOMPARE(AgentActor::resultFailureCode(QJsonObject{{"error", "boom"}}), QStringLiteral("tool_error"));
    }

    // 阶7 Agent 评审修复（P1-1）：schema 声明 value 为字符串且承诺转换——
    // 字符串数值按目标类型转换并经插件校验；非法值工具层直接报错（不假成功）
    void testSetParamConvertsStringAndValidates() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance inst;
        inst.id = QStringLiteral("mc1");
        inst.moduleId = QStringLiteral("MeasureCircle");
        project->addModule(inst);

        QJsonObject ok = actor.executeTool(
            "set_param",
            QJsonObject{{"instanceId", "mc1"}, {"key", "initialRadius"}, {"value", QStringLiteral("120")}});
        QCOMPARE(ok.value("status").toString(), QString("param_set"));
        QCOMPARE(project->moduleById(QStringLiteral("mc1"))->params.value("initialRadius"), QJsonValue(120.0));

        QJsonObject notNumber = actor.executeTool(
            "set_param",
            QJsonObject{{"instanceId", "mc1"}, {"key", "initialRadius"}, {"value", QStringLiteral("abc")}});
        QVERIFY2(notNumber.contains("error"), "non-numeric string must fail at tool layer");

        QJsonObject outOfRange = actor.executeTool(
            "set_param",
            QJsonObject{{"instanceId", "mc1"}, {"key", "initialRadius"}, {"value", QStringLiteral("9999999")}});
        QVERIFY2(outOfRange.contains("error"), "out-of-range value must fail plugin validation");
        QCOMPARE(project->moduleById(QStringLiteral("mc1"))->params.value("initialRadius"), QJsonValue(120.0));
    }

    void testSetParamRejectsUnknownAndUndoRemovesNewOverride() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance inst;
        inst.id = QStringLiteral("mc1");
        inst.moduleId = QStringLiteral("MeasureCircle");
        project->addModule(inst);

        QVERIFY(actor.executeTool("set_param", QJsonObject{{"instanceId", "mc1"}, {"key", "madeUp"}, {"value", 12}})
                    .contains("error"));
        QVERIFY(!project->moduleById(QStringLiteral("mc1"))->params.contains(QStringLiteral("madeUp")));

        QJsonObject changed =
            actor.executeTool("set_param", QJsonObject{{"instanceId", "mc1"}, {"key", "initialRadius"}, {"value", 80}});
        QVERIFY(!changed.contains("error"));
        QVERIFY(project->moduleById(QStringLiteral("mc1"))->params.contains(QStringLiteral("initialRadius")));
        actor.undoStack()->undo();
        QVERIFY(!project->moduleById(QStringLiteral("mc1"))->params.contains(QStringLiteral("initialRadius")));
    }

    void testAtomicTypedModuleParamsAndReset() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance inst;
        inst.id = QStringLiteral("measure");
        inst.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(inst);
        const int undoBefore = actor.undoStack()->count();

        const QJsonArray point1{1.0, 2.0};
        const QJsonArray point2{3.0, 4.0};
        const QJsonObject values{{"mode", "point_pair"}, {"point1", point1}, {"point2", point2}};
        const QJsonObject changed =
            actor.executeTool("set_module_params", QJsonObject{{"instanceId", "measure"}, {"values", values}});
        QCOMPARE(changed.value("status").toString(), QStringLiteral("params_set"));
        QCOMPARE(actor.undoStack()->count(), undoBefore + 1);
        QCOMPARE(project->moduleById(QStringLiteral("measure"))->params.value(QStringLiteral("point1")),
                 QJsonValue(point1));
        QCOMPARE(project->moduleById(QStringLiteral("measure"))->params.value(QStringLiteral("point2")),
                 QJsonValue(point2));

        const QJsonObject beforeFailure = project->moduleById(QStringLiteral("measure"))->params;
        const QJsonObject invalid = actor.executeTool(
            "set_module_params", QJsonObject{{"instanceId", "measure"},
                                             {"values", QJsonObject{{"point1", QJsonArray{8, 9}}, {"unknownKey", 1}}}});
        QCOMPARE(invalid.value("errorCode").toString(), QStringLiteral("INVALID_PARAM"));
        QCOMPARE(project->moduleById(QStringLiteral("measure"))->params, beforeFailure);
        QCOMPARE(actor.undoStack()->count(), undoBefore + 1);

        actor.undoStack()->undo();
        QVERIFY(project->moduleById(QStringLiteral("measure"))->params.isEmpty());
        actor.undoStack()->redo();
        QCOMPARE(project->moduleById(QStringLiteral("measure"))->params, beforeFailure);

        const QJsonObject reset = actor.executeTool(
            "reset_module_params", QJsonObject{{"instanceId", "measure"}, {"keys", QJsonArray{"point1", "point2"}}});
        QCOMPARE(reset.value("status").toString(), QStringLiteral("params_reset"));
        QCOMPARE(actor.undoStack()->count(), undoBefore + 2);
        QVERIFY(!project->moduleById(QStringLiteral("measure"))->params.contains(QStringLiteral("point1")));
        QVERIFY(!project->moduleById(QStringLiteral("measure"))->params.contains(QStringLiteral("point2")));
        actor.undoStack()->undo();
        QCOMPARE(project->moduleById(QStringLiteral("measure"))->params, beforeFailure);
    }

    void testNestedMeasurementValuesRejectWithoutPartialUpdate() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance inst;
        inst.id = QStringLiteral("measure");
        inst.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(inst);
        const int undoBefore = actor.undoStack()->count();

        const QJsonObject badPoint{{"mode", "point_set"}, {"points", QJsonArray{QJsonValue(QJsonArray{1, "bad"})}}};
        const QJsonObject rejected =
            actor.executeTool("set_module_params", QJsonObject{{"instanceId", "measure"}, {"values", badPoint}});
        QCOMPARE(rejected.value("errorCode").toString(), QStringLiteral("VALIDATION_FAILED"));
        QVERIFY(project->moduleById(QStringLiteral("measure"))->params.isEmpty());
        QCOMPARE(actor.undoStack()->count(), undoBefore);

        QJsonArray tooManyPoints;
        for (int i = 0; i < 1025; ++i)
            tooManyPoints.append(QJsonArray{i, i + 1});
        const QJsonObject tooLarge =
            actor.executeTool("set_module_params", QJsonObject{{"instanceId", "measure"},
                                                               {"values", QJsonObject{{"points", tooManyPoints}}}});
        QCOMPARE(tooLarge.value("errorCode").toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(actor.undoStack()->count(), undoBefore);
        const QJsonObject nonFinite = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", "measure"},
                        {"values", QJsonObject{{"point1", QJsonArray{std::numeric_limits<double>::infinity(), 2}}}}});
        QVERIFY(nonFinite.contains("error"));
        QCOMPARE(actor.undoStack()->count(), undoBefore);

        const QJsonObject valid{{"mode", "point_set"}, {"points", QJsonArray{QJsonValue(QJsonArray{1.0, 2.0})}}};
        const QJsonObject accepted =
            actor.executeTool("set_module_params", QJsonObject{{"instanceId", "measure"}, {"values", valid}});
        QCOMPARE(accepted.value("status").toString(), QStringLiteral("params_set"));
        QCOMPARE(actor.undoStack()->count(), undoBefore + 1);
        QCOMPARE(project->moduleById(QStringLiteral("measure"))->params.value(QStringLiteral("points")),
                 valid.value(QStringLiteral("points")));
    }

    void testCrossFieldRadiusUpdateIsAtomic() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance fit;
        fit.id = QStringLiteral("fit");
        fit.moduleId = QStringLiteral("FitCircle");
        project->addModule(fit);
        const int undoBefore = actor.undoStack()->count();
        const QJsonObject values{{"minRadius", 1200.0}, {"maxRadius", 1500.0}};
        QCOMPARE(actor.executeTool("set_module_params", QJsonObject{{"instanceId", fit.id}, {"values", values}})
                     .value("status")
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(project->moduleById(fit.id)->params, values);
        QCOMPARE(actor.undoStack()->count(), undoBefore + 1);
        const QJsonObject rejected = actor.executeTool(
            "set_module_params",
            QJsonObject{{"instanceId", fit.id}, {"values", QJsonObject{{"minRadius", 1600.0}, {"maxRadius", 1500.0}}}});
        QCOMPARE(rejected.value("errorCode").toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(project->moduleById(fit.id)->params, values);
        QCOMPARE(actor.undoStack()->count(), undoBefore + 1);
    }

    void testAgentParameterUndoUsesKeyDeltasAndExpiresAtomically() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance inst;
        inst.id = QStringLiteral("measure");
        inst.moduleId = QStringLiteral("FitCircle");
        inst.params = QJsonObject{{QStringLiteral("minRadius"), 40.0}, {QStringLiteral("maxRadius"), 100.0}};
        project->addModule(inst);

        QCOMPARE(
            actor
                .executeTool("set_param", QJsonObject{{"instanceId", "measure"}, {"key", "minRadius"}, {"value", 10.0}})
                .value("status")
                .toString(),
            QStringLiteral("param_set"));
        QVERIFY(
            project->setModuleParam("measure", QStringLiteral("maxRadius"), 20.0, Project::ParameterChangeSource::Gui));
        const QJsonObject beforeRejectedUndo = project->moduleById("measure")->params;
        project->setModified(false);
        QVERIFY(!actor.undoLastAction()); // complete candidate would violate minRadius <= maxRadius
        QCOMPARE(project->moduleById("measure")->params, beforeRejectedUndo);
        QVERIFY(!project->isModified());

        project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance multi;
        multi.id = QStringLiteral("measure_multi");
        multi.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(multi);
        AgentActor multiActor;
        const QJsonArray originalPoint{1.0, 2.0};
        const QJsonArray secondPoint{3.0, 4.0};
        QCOMPARE(multiActor
                     .executeTool("set_module_params", QJsonObject{{"instanceId", multi.id},
                                                                   {"values", QJsonObject{{"mode", "point_pair"},
                                                                                          {"point1", originalPoint},
                                                                                          {"point2", secondPoint}}}})
                     .value("status")
                     .toString(),
                 QStringLiteral("params_set"));
        QVERIFY(project->setModuleParam(multi.id, QStringLiteral("point1"), QJsonArray{5.0, 6.0},
                                        Project::ParameterChangeSource::Gui));
        const QJsonObject beforeMultiUndo = project->moduleById(multi.id)->params;
        QVERIFY(!multiActor.undoLastAction());
        QCOMPARE(project->moduleById(multi.id)->params, beforeMultiUndo);
        QCOMPARE(project->moduleById(multi.id)->params.value(QStringLiteral("point2")), QJsonValue(secondPoint));

        project = ProjectManager::instance().newProject();
        QVERIFY(project);
        inst.id = QStringLiteral("measure_disjoint");
        inst.moduleId = QStringLiteral("FitCircle");
        inst.params = QJsonObject{{QStringLiteral("minRadius"), 10.0}, {QStringLiteral("maxRadius"), 500.0}};
        project->addModule(inst);
        AgentActor disjointActor;
        QCOMPARE(
            disjointActor
                .executeTool("set_param", QJsonObject{{"instanceId", inst.id}, {"key", "threshold"}, {"value", 3.0}})
                .value("status")
                .toString(),
            QStringLiteral("param_set"));
        QVERIFY(
            project->setModuleParam(inst.id, QStringLiteral("iterations"), 200, Project::ParameterChangeSource::Gui));
        QVERIFY(disjointActor.undoLastAction());
        QVERIFY(!project->moduleById(inst.id)->params.contains(QStringLiteral("threshold")));
        QCOMPARE(project->moduleById(inst.id)->params.value(QStringLiteral("iterations")), QJsonValue(200));

        QCOMPARE(
            disjointActor
                .executeTool("set_param", QJsonObject{{"instanceId", inst.id}, {"key", "threshold"}, {"value", 4.0}})
                .value("status")
                .toString(),
            QStringLiteral("param_set"));
        QVERIFY(
            project->setModuleParam(inst.id, QStringLiteral("threshold"), 5.0, Project::ParameterChangeSource::Gui));
        QVERIFY(
            project->setModuleParam(inst.id, QStringLiteral("threshold"), 4.0,
                                    Project::ParameterChangeSource::Gui)); // ABA must still stale the Agent command.
        const QJsonObject beforeStaleUndo = project->moduleById(inst.id)->params;
        project->setModified(false);
        QVERIFY(!disjointActor.undoLastAction());
        QCOMPARE(project->moduleById(inst.id)->params, beforeStaleUndo);
        QVERIFY(!project->isModified());

        project = ProjectManager::instance().newProject();
        QVERIFY(project);
        inst.id = QStringLiteral("measure_lifo");
        inst.params = QJsonObject{{QStringLiteral("minRadius"), 10.0}, {QStringLiteral("maxRadius"), 500.0}};
        project->addModule(inst);
        AgentActor lifoActor;
        QCOMPARE(
            lifoActor
                .executeTool("set_param", QJsonObject{{"instanceId", inst.id}, {"key", "threshold"}, {"value", 3.0}})
                .value("status")
                .toString(),
            QStringLiteral("param_set"));
        QCOMPARE(
            lifoActor
                .executeTool("set_param", QJsonObject{{"instanceId", inst.id}, {"key", "threshold"}, {"value", 4.0}})
                .value("status")
                .toString(),
            QStringLiteral("param_set"));
        QVERIFY(lifoActor.undoLastAction());
        QCOMPARE(project->moduleById(inst.id)->params.value(QStringLiteral("threshold")), QJsonValue(3.0));
        QVERIFY(lifoActor.undoLastAction());
        QVERIFY(!project->moduleById(inst.id)->params.contains(QStringLiteral("threshold")));
        lifoActor.undoStack()->redo();
        QCOMPARE(project->moduleById(inst.id)->params.value(QStringLiteral("threshold")), QJsonValue(3.0));
        lifoActor.undoStack()->redo();
        QCOMPARE(project->moduleById(inst.id)->params.value(QStringLiteral("threshold")), QJsonValue(4.0));

        project = ProjectManager::instance().newProject();
        QVERIFY(project);
        inst.id = QStringLiteral("measure_replaced");
        AgentActor replacedActor;
        project->addModule(inst);
        QCOMPARE(
            replacedActor
                .executeTool("set_param", QJsonObject{{"instanceId", inst.id}, {"key", "threshold"}, {"value", 3.0}})
                .value("status")
                .toString(),
            QStringLiteral("param_set"));
        project->removeModule(inst.id);
        ModuleInstance replacement = inst;
        replacement.params.insert(QStringLiteral("threshold"), 6.0);
        project->addModule(replacement);
        const QJsonObject replacementState = project->moduleById(inst.id)->params;
        QVERIFY(!replacedActor.undoLastAction());
        QCOMPARE(project->moduleById(inst.id)->params, replacementState);

        project = ProjectManager::instance().newProject();
        QVERIFY(project);
        inst.id = QStringLiteral("measure_barrier");
        inst.params = QJsonObject{{QStringLiteral("minRadius"), 10.0}, {QStringLiteral("maxRadius"), 500.0}};
        project->addModule(inst);
        AgentActor barrierActor;
        QTemporaryDir saveDir;
        QVERIFY(saveDir.isValid());
        const QString savePath = saveDir.filePath(QStringLiteral("parameter-barrier.dproj"));
        QCOMPARE(barrierActor.executeTool("save_project", QJsonObject{{"path", savePath}}).value("status").toString(),
                 QStringLiteral("saved"));
        QCOMPARE(
            barrierActor
                .executeTool("set_param", QJsonObject{{"instanceId", inst.id}, {"key", "threshold"}, {"value", 3.0}})
                .value("status")
                .toString(),
            QStringLiteral("param_set"));
        QVERIFY(
            project->setModuleParam(inst.id, QStringLiteral("threshold"), 4.0, Project::ParameterChangeSource::Gui));
        const QJsonObject barrierConflict = project->moduleById(inst.id)->params;
        const int afterBarrierCommand = barrierActor.undoStack()->index();
        QVERIFY(!barrierActor.undoLastAction()); // this one call expires the post-barrier stale command
        QCOMPARE(barrierActor.undoStack()->index(), afterBarrierCommand - 1);
        QCOMPARE(project->moduleById(inst.id)->params, barrierConflict);
        const int atBarrierIndex = barrierActor.undoStack()->index();
        QVERIFY(!barrierActor.undoLastAction());
        QCOMPARE(barrierActor.undoStack()->index(), atBarrierIndex);
        barrierActor.undoStack()->redo(); // Redo navigates forward but the permanently stale command stays inert.
        QCOMPARE(barrierActor.undoStack()->index(), afterBarrierCommand);
        QCOMPARE(project->moduleById(inst.id)->params, barrierConflict);
    }

    void testAgentParameterUndoPreservesExplicitJsonNull() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance inst;
        inst.id = QStringLiteral("sam_null");
        inst.moduleId = QStringLiteral("SamPredict");
        inst.params.insert(QStringLiteral("box"), QJsonValue::Null);
        project->addModule(inst);

        const QJsonArray box{10.0, 20.0, 30.0, 40.0};
        const QJsonObject changed =
            actor.executeTool("set_param", QJsonObject{{"instanceId", inst.id}, {"key", "box"}, {"value", box}});
        QCOMPARE(changed.value("status").toString(), QStringLiteral("param_set"));
        QVERIFY(actor.undoLastAction());
        const QJsonObject restored = project->moduleById(inst.id)->params;
        QVERIFY(restored.contains(QStringLiteral("box")));
        QVERIFY(restored.value(QStringLiteral("box")).isNull());

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("explicit-null.dproj"));
        QVERIFY(project->save(path));
        Project reopened;
        QVERIFY(reopened.load(path));
        const QJsonObject reopenedParams = reopened.moduleById(inst.id)->params;
        QVERIFY(reopenedParams.contains(QStringLiteral("box")));
        QVERIFY(reopenedParams.value(QStringLiteral("box")).isNull());
    }

    void testParameterMissingNullAndEmptyStringAreDistinct() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance inst;
        inst.id = QStringLiteral("grab");
        inst.moduleId = QStringLiteral("GrabImage");
        project->addModule(inst);

        const QJsonObject missing =
            actor.executeTool("set_param", QJsonObject{{"instanceId", "grab"}, {"key", "filePath"}});
        QCOMPARE(missing.value("errorCode").toString(), QStringLiteral("MISSING_PARAM"));
        const QJsonObject invalidNull = actor.executeTool(
            "set_param", QJsonObject{{"instanceId", "grab"}, {"key", "filePath"}, {"value", QJsonValue::Null}});
        QCOMPARE(invalidNull.value("errorCode").toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(project->moduleById(QStringLiteral("grab"))->params.isEmpty());

        const QJsonObject empty =
            actor.executeTool("set_param", QJsonObject{{"instanceId", "grab"}, {"key", "filePath"}, {"value", ""}});
        QCOMPARE(empty.value("status").toString(), QStringLiteral("param_set"));
        QCOMPARE(project->moduleById(QStringLiteral("grab"))->params.value(QStringLiteral("filePath")),
                 QJsonValue(QStringLiteral("")));
        actor.undoStack()->undo();
        QVERIFY(!project->moduleById(QStringLiteral("grab"))->params.contains(QStringLiteral("filePath")));
    }

    void testSchemaCurrentParamsAreFreshPerInstance() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance first;
        first.id = QStringLiteral("mc1");
        first.moduleId = QStringLiteral("MeasureCircle");
        first.params.insert(QStringLiteral("initialRadius"), 40);
        project->addModule(first);
        ModuleInstance second = first;
        second.id = QStringLiteral("mc2");
        second.params.insert(QStringLiteral("initialRadius"), 90);
        project->addModule(second);

        QCOMPARE(actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", "mc1"}})
                     .value("currentParams")
                     .toObject()
                     .value("initialRadius"),
                 QJsonValue(40));
        QCOMPARE(actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", "mc2"}})
                     .value("currentParams")
                     .toObject()
                     .value("initialRadius"),
                 QJsonValue(90));

        QVERIFY(
            !actor.executeTool("set_param", QJsonObject{{"instanceId", "mc1"}, {"key", "initialRadius"}, {"value", 70}})
                 .contains("error"));
        QCOMPARE(actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", "mc1"}})
                     .value("currentParams")
                     .toObject()
                     .value("initialRadius"),
                 QJsonValue(70));
    }

    void testSchemaBeforeInstanceIncludesPluginIdentityAndPorts() {
        AgentActor actor;
        const QJsonObject schema =
            actor.executeTool("get_module_params_schema", QJsonObject{{"plugin", "MeasurementInput"}});
        QVERIFY(!schema.contains("error"));
        QCOMPARE(schema.value("pluginId").toString(), QStringLiteral("com.deeplux.plugin.measurementinput"));
        QVERIFY(schema.value("loaded").toBool());
        const QJsonObject parameters = schema.value("parameters").toObject();
        QCOMPARE(parameters.value("point1").toObject().value("type").toString(), QStringLiteral("array"));
        QCOMPARE(parameters.value("point1").toObject().value("effective"),
                 schema.value("defaultParams").toObject().value("point1"));
        QVERIFY(!parameters.value("point1").toObject().value("hasOverride").toBool());
        QVERIFY(schema.value("available").toBool());
        QVERIFY(parameters.value("mode").toObject().value("enum").toArray().size() >= 6);
        const QJsonObject ports = schema.value("ports").toObject();
        QVERIFY(ports.value("inputs").isArray());
        QVERIFY(ports.value("outputs").isArray());
        QVERIFY(schema.value("defaultParams").toObject().contains("point1"));
        QVERIFY(schema.value("currentParams").toObject().isEmpty());

        const QJsonObject ambiguous = actor.executeTool(
            "get_module_params_schema", QJsonObject{{"plugin", "MeasurementInput"}, {"instanceId", "x"}});
        QCOMPARE(ambiguous.value("errorCode").toString(), QStringLiteral("INVALID_PARAM"));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("live_measure");
        instance.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(instance);
        const QJsonArray point{7.0, 8.0};
        QVERIFY(!actor
                     .executeTool("set_module_params",
                                  QJsonObject{{"instanceId", instance.id}, {"values", QJsonObject{{"point1", point}}}})
                     .contains("error"));
        const QJsonObject live = actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", instance.id}})
                                     .value("parameters")
                                     .toObject()
                                     .value("point1")
                                     .toObject();
        QVERIFY(live.value("hasOverride").toBool());
        QCOMPARE(live.value("current"), QJsonValue(point));
        QCOMPARE(live.value("effective"), QJsonValue(point));
        actor.undoStack()->undo();
        const QJsonObject afterUndo =
            actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", instance.id}})
                .value("parameters")
                .toObject()
                .value("point1")
                .toObject();
        QVERIFY(!afterUndo.value("hasOverride").toBool());
        QCOMPARE(afterUndo.value("effective"), schema.value("defaultParams").toObject().value("point1"));

        const QJsonObject grabSchema =
            actor.executeTool("get_module_params_schema", QJsonObject{{"plugin", "GrabImage"}});
        const QJsonObject grabParameters = grabSchema.value("parameters").toObject();
        QCOMPARE(grabParameters.value("grabSource").toObject().value("enum").toArray().size(), 3);
        QCOMPARE(grabParameters.value("cameraId").toObject().value("optionsSource").toString(),
                 QStringLiteral("CameraManager.knownCameras"));
        QVERIFY(grabParameters.value("cameraId").toObject().value("dynamicOptions").isArray());
        const QJsonObject cameraField = grabParameters.value("cameraId").toObject();
        if (cameraField.value("dynamicOptions").toArray().isEmpty())
            QCOMPARE(cameraField.value("resourceStatus").toString(), QStringLiteral("unavailable"));
        QCOMPARE(grabParameters.value("filePath").toObject().value("resourceStatus").toString(),
                 QStringLiteral("unconfigured"));
        ModuleInstance grab;
        grab.id = QStringLiteral("live_grab");
        grab.moduleId = QStringLiteral("GrabImage");
        project->addModule(grab);
        const QString filePath = m_appDataDir.filePath(QStringLiteral("resource_status.png"));
        QVERIFY(!actor
                     .executeTool("set_module_params",
                                  QJsonObject{{"instanceId", grab.id}, {"values", QJsonObject{{"filePath", filePath}}}})
                     .contains("error"));
        const auto fileStatus = [&actor, &grab]() {
            return actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", grab.id}})
                .value("parameters")
                .toObject()
                .value("filePath")
                .toObject()
                .value("resourceStatus")
                .toString();
        };
        QCOMPARE(fileStatus(), QStringLiteral("missing"));
        QFile resource(filePath);
        QVERIFY(resource.open(QIODevice::WriteOnly));
        QVERIFY(resource.write("test") == 4);
        resource.close();
        QCOMPARE(fileStatus(), QStringLiteral("available"));
    }

    void testConnectModulesValidatesBothPortsAndTypes() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance input;
        input.id = QStringLiteral("input");
        input.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(input);
        ModuleInstance fit;
        fit.id = QStringLiteral("fit");
        fit.moduleId = QStringLiteral("FitCircle");
        project->addModule(fit);

        QJsonObject connected = actor.executeTool(
            "connect_modules",
            QJsonObject{{"fromId", "input"}, {"toId", "fit"}, {"fromPort", "fit_points"}, {"toPort", "fit_points"}});
        QVERIFY2(!connected.contains("error"), qPrintable(connected.value("error").toString()));
        QCOMPARE(project->connections().size(), 1);
        QCOMPARE(project->connections().first().fromPort, QStringLiteral("fit_points"));
        QCOMPARE(project->connections().first().toPort, QStringLiteral("fit_points"));
        QCOMPARE(project->connections().first().edgeType, QStringLiteral("data"));

        const int undoCount = actor.undoStack()->count();
        QCOMPARE(actor
                     .executeTool("connect_modules", QJsonObject{{"fromId", "input"},
                                                                 {"toId", "fit"},
                                                                 {"fromPort", "fit_points"},
                                                                 {"toPort", "fit_points"}})
                     .value("status")
                     .toString(),
                 QStringLiteral("already_connected"));
        QCOMPARE(actor.undoStack()->count(), undoCount);

        ModuleInstance secondInput = input;
        secondInput.id = QStringLiteral("input2");
        project->addModule(secondInput);
        QVERIFY(actor
                    .executeTool("connect_modules", QJsonObject{{"fromId", "input2"},
                                                                {"toId", "fit"},
                                                                {"fromPort", "fit_points"},
                                                                {"toPort", "fit_points"}})
                    .contains("error"));

        QVERIFY(actor
                    .executeTool("connect_modules", QJsonObject{{"fromId", "input"},
                                                                {"toId", "fit"},
                                                                {"fromPort", "fit_points"},
                                                                {"toPort", "missing"}})
                    .contains("error"));
        QVERIFY(
            actor
                .executeTool(
                    "connect_modules",
                    QJsonObject{{"fromId", "input"}, {"toId", "fit"}, {"fromPort", "point1"}, {"toPort", "fit_points"}})
                .contains("error"));
        QCOMPARE(project->connections().size(), 1);

        QVERIFY(!actor
                     .executeTool("disconnect_modules", QJsonObject{{"fromId", "input"},
                                                                    {"toId", "fit"},
                                                                    {"fromPort", "fit_points"},
                                                                    {"toPort", "fit_points"}})
                     .contains("error"));
        QVERIFY(project->connections().isEmpty());
        actor.undoStack()->undo();
        QCOMPARE(project->connections().size(), 1);
        QCOMPARE(project->connections().first().fromPort, QStringLiteral("fit_points"));
        QCOMPARE(project->connections().first().toPort, QStringLiteral("fit_points"));
        QCOMPARE(project->connections().first().edgeType, QStringLiteral("data"));
    }

    void testRunFlowKeepsEventLoopResponsive() {
        AgentActor actor;
        BlockingAgentModule module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        // 后台执行（设计 §6.2）：未注册探针默认 gui，会经 BlockingQueuedConnection 回到 GUI
        // 线程并阻塞事件循环；本用例验证 Agent 非阻塞返回语义，故显式标记 worker 亲和。
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("agent_blocking"),
                                                                ExecutionAffinity::Worker);

        QElapsedTimer elapsed;
        elapsed.start();
        bool stopCalled = false;
        QTimer::singleShot(30, this, [&stopCalled]() {
            stopCalled = true;
            RunEngine::instance().stop();
        });

        QJsonObject result = actor.executeTool("run_flow", QJsonObject{{"mode", "once"}, {"wait", false}});
        QCOMPARE(result.value("status").toString(), QStringLiteral("started"));
        QVERIFY(RunEngine::instance().isBusy());
        QVERIFY(actor.executeTool("run_flow", QJsonObject{{"mode", "once"}, {"wait", false}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(stopCalled, 2000);
        QVERIFY2(elapsed.elapsed() < 500,
                 qPrintable(QStringLiteral("Agent run blocked the event loop for %1 ms").arg(elapsed.elapsed())));
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 2000);
        QVERIFY(module.m_entered.load(std::memory_order_acquire));
    }

    void testG4AgentRunControlsReflectRunEngineLifecycle() {
        AgentActor actor;
        RunEngine& engine = RunEngine::instance();
        const RunState idleState = engine.state();
        QVERIFY(idleState == RunState::Idle || idleState == RunState::Stopped);
        QCOMPARE(actor.executeTool("pause_flow", {}).value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(actor.executeTool("resume_flow", {}).value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(actor.executeTool("stop_flow", {}).value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(engine.state(), idleState);

        BlockingAgentModule module;
        QVERIFY(module.initialize());
        engine.addModule(&module);
        // 未注册的测试模块默认 GUI 亲和；后台 Cycle 会在 GUI 事件中逐轮等待，
        // 本测试要检查服务线程的 pause/resume 控制，因此显式使用 worker。
        engine.setModuleExecutionAffinityForTest(QStringLiteral("agent_blocking"), ExecutionAffinity::Worker);
        const QJsonObject started = actor.executeTool("run_flow", QJsonObject{{"mode", "cycle"}, {"maxCycles", 10}});
        QCOMPARE(started.value("status").toString(), QStringLiteral("started"));
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Running, 2000);

        const QJsonObject paused = actor.executeTool("pause_flow", {});
        QCOMPARE(paused.value("status").toString(), QStringLiteral("paused"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Paused, 2000);
        QCOMPARE(actor.executeTool("pause_flow", {}).value("status").toString(), QStringLiteral("no_op"));

        const QJsonObject resumed = actor.executeTool("resume_flow", {});
        QCOMPARE(resumed.value("status").toString(), QStringLiteral("resumed"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Running, 2000);

        const QJsonObject stopped = actor.executeTool("stop_flow", {});
        QVERIFY(stopped.value("status").toString() == QLatin1String("stopped") ||
                stopped.value("status").toString() == QLatin1String("stopping"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 2000);
        QCOMPARE(actor.executeTool("resume_flow", {}).value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(actor.executeTool("stop_flow", {}).value("status").toString(), QStringLiteral("no_op"));
    }

    void testG4AgentStopReportsStoppingUntilNonCooperativeModuleSettles() {
        AgentActor actor;
        NonCooperativeAgentModule module;
        QSemaphore releaseGate;
        module.m_releaseGate = &releaseGate;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("agent_noncooperative"),
                                                                ExecutionAffinity::Worker);

        const QJsonObject started = actor.executeTool("run_flow", QJsonObject{{"mode", "once"}, {"wait", false}});
        QCOMPARE(started.value("status").toString(), QStringLiteral("started"));
        const QString runId = started.value("runId").toString();
        QVERIFY(!runId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(module.m_entered.load(std::memory_order_acquire), 2000);

        const QJsonObject stopped = actor.executeTool("stop_flow", {});
        releaseGate.release();
        QCOMPARE(stopped.value("status").toString(), QStringLiteral("stopping"));
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 2000);
        const QJsonObject results = actor.executeTool("get_run_results", QJsonObject{{"runId", runId}});
        QCOMPARE(results.value("status").toString(), QStringLiteral("cancelled"));
        QCOMPARE(results.value("isBusy").toBool(), false);
        QCOMPARE(actor.executeTool("stop_flow", {}).value("status").toString(), QStringLiteral("no_op"));
    }

    void testG4AgentStopIsNoOpDuringMaintenanceWithoutExecution() {
        AgentActor actor;
        RunEngine& engine = RunEngine::instance();
        Project project;
        ModuleInstance instance;
        instance.id = QStringLiteral("maintenance_module");
        instance.moduleId = QStringLiteral("test_module");
        project.addModule(instance);

        QSemaphore factoryEntered;
        QSemaphore releaseFactory;
        std::atomic_bool loaded{false};
        std::thread loader([&]() {
            loaded.store(engine.loadProject(&project,
                                            [&](const ModuleInstance&) {
                                                factoryEntered.release();
                                                releaseFactory.acquire();
                                                return new NonCooperativeAgentModule();
                                            }),
                         std::memory_order_release);
        });

        const bool entered = factoryEntered.tryAcquire(1, 2000);
        QJsonObject stopResult;
        bool maintenanceBusy = false;
        bool noExecution = false;
        if (entered) {
            maintenanceBusy = engine.isBusy();
            noExecution = !engine.isExecuting();
            stopResult = actor.executeTool("stop_flow", {});
        }
        releaseFactory.release();
        loader.join();

        QVERIFY(entered);
        QVERIFY(maintenanceBusy);
        QVERIFY(noExecution);
        QVERIFY(loaded.load(std::memory_order_acquire));
        QCOMPARE(stopResult.value("status").toString(), QStringLiteral("no_op"));
        QVERIFY(!engine.isBusy());
        QCOMPARE(engine.state(), RunState::Stopped);
    }

    void testG4StepOnceAdvancesOneModuleAndResetReturnsToEntry() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "single_step"}}).contains("error"));
        for (const QString& id : {QStringLiteral("step_a"), QStringLiteral("step_b")}) {
            QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", id}})
                         .contains("error"));
            QVERIFY(!actor
                         .executeTool("set_param",
                                      QJsonObject{{"instanceId", id}, {"key", "grabSource"}, {"value", "Demo"}})
                         .contains("error"));
        }

        QStringList executed;
        const QMetaObject::Connection finishedConn =
            QObject::connect(&RunEngine::instance(), &RunEngine::moduleFinished, this,
                             [&executed](const QString& id, bool success, int) {
                                 if (success)
                                     executed.append(id);
                             });
        QJsonObject result = actor.executeTool("step_once", {});
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("status").toString(), QStringLiteral("started"));
        QCOMPARE(waitForRun(actor, result.value("runId").toString()).value("status").toString(),
                 QStringLiteral("completed"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(executed, QStringList{QStringLiteral("step_a")}, 1000);
        QCOMPARE(executed, QStringList{QStringLiteral("step_a")});

        result = actor.executeTool("step_once", {});
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(waitForRun(actor, result.value("runId").toString()).value("status").toString(),
                 QStringLiteral("completed"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(executed, (QStringList{QStringLiteral("step_a"), QStringLiteral("step_b")}), 1000);
        QCOMPARE(executed, (QStringList{QStringLiteral("step_a"), QStringLiteral("step_b")}));

        QCOMPARE(actor.executeTool("reset_step", {}).value("status").toString(), QStringLiteral("reset"));
        result = actor.executeTool("step_once", {});
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(waitForRun(actor, result.value("runId").toString()).value("status").toString(),
                 QStringLiteral("completed"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(
            executed, (QStringList{QStringLiteral("step_a"), QStringLiteral("step_b"), QStringLiteral("step_a")}),
            1000);
        QCOMPARE(executed.last(), QStringLiteral("step_a"));

        RunEngine::instance().setOutput(QStringLiteral("step_a"), QStringLiteral("read_only_probe"), 42);
        const QJsonObject validation = actor.executeTool("validate_flow", {});
        QVERIFY2(validation.value(QStringLiteral("valid")).toBool(), "validate_flow unexpectedly rejected the flow");
        QCOMPARE(RunEngine::instance().getOutput(QStringLiteral("step_a"), QStringLiteral("read_only_probe")).toInt(),
                 42);
        result = actor.executeTool("step_once", {});
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(waitForRun(actor, result.value("runId").toString()).value("status").toString(),
                 QStringLiteral("completed"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(executed,
                                  (QStringList{QStringLiteral("step_a"), QStringLiteral("step_b"),
                                               QStringLiteral("step_a"), QStringLiteral("step_b")}),
                                  1000);
        QCOMPARE(executed.last(), QStringLiteral("step_b"));
        QObject::disconnect(finishedConn);
    }

    void testG4ValidateFlowReportsLocationsAndPreservesRuntimeModules() {
        AgentActor actor;
        QSignalSpy syncSpy(&actor, &AgentActor::runEngineSyncRequested);
        QCOMPARE(actor.executeTool("validate_flow", {}).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("NO_PROJECT"));
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance source;
        source.id = QStringLiteral("validate_source");
        source.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(source);
        ModuleInstance target;
        target.id = QStringLiteral("validate_target");
        target.moduleId = QStringLiteral("FitCircle");
        project->addModule(target);

        RunEngine& engine = RunEngine::instance();
        QVERIFY(engine.loadProject(project));
        const QList<ModuleBase*> beforeModules = engine.modules();

        std::optional<ModuleInstance> originalTarget = project->moduleById(target.id);
        QVERIFY(originalTarget.has_value());
        ModuleInstance changedTarget = *originalTarget;
        changedTarget.moduleId = QStringLiteral("MeasurementInput");
        project->updateModule(target.id, changedTarget);
        QCOMPARE(actor.executeTool("validate_flow", {}).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("ENGINE_OUT_OF_SYNC"));
        project->updateModule(target.id, *originalTarget);

        ModuleInstance unsynced;
        unsynced.id = QStringLiteral("unsynced_module");
        unsynced.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(unsynced);
        QCOMPARE(actor.executeTool("validate_flow", {}).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("ENGINE_OUT_OF_SYNC"));
        project->removeModule(unsynced.id);

        QJsonObject result = actor.executeTool("validate_flow", {});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("invalid"));
        QVERIFY(!result.value(QStringLiteral("valid")).toBool());
        const QJsonArray missingErrors = result.value(QStringLiteral("errors")).toArray();
        QCOMPARE(missingErrors.size(), 1);
        QCOMPARE(missingErrors.first().toObject().value(QStringLiteral("moduleId")).toString(), target.id);
        QCOMPARE(missingErrors.first().toObject().value(QStringLiteral("portId")).toString(),
                 QStringLiteral("fit_points"));
        QCOMPARE(engine.modules(), beforeModules);

        ModuleConnection compatible;
        compatible.fromModuleId = source.id;
        compatible.toModuleId = target.id;
        compatible.fromPort = QStringLiteral("fit_points");
        compatible.toPort = QStringLiteral("fit_points");
        compatible.edgeType = QStringLiteral("data");
        project->addConnection(compatible);
        result = actor.executeTool("validate_flow", {});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("valid"));
        QVERIFY(result.value(QStringLiteral("errors")).toArray().isEmpty());
        QCOMPARE(engine.modules(), beforeModules);

        project->removeConnectionWithPorts(source.id, compatible.fromPort, target.id, compatible.toPort);
        ModuleConnection incompatible = compatible;
        incompatible.fromPort = QStringLiteral("point1");
        project->addConnection(incompatible);
        result = actor.executeTool("validate_flow", {});
        // Any is structurally connectable; the runtime consumer gate checks the actual 2D/3D payload.
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("valid"));
        QVERIFY(result.value(QStringLiteral("errors")).toArray().isEmpty());
        QCOMPARE(engine.modules(), beforeModules);

        project->removeConnectionWithPorts(source.id, incompatible.fromPort, target.id, incompatible.toPort);
        ModuleConnection forwardControl;
        forwardControl.fromModuleId = source.id;
        forwardControl.toModuleId = target.id;
        forwardControl.fromPort = QStringLiteral("next");
        forwardControl.toPort = QStringLiteral("control");
        forwardControl.edgeType = QStringLiteral("control");
        ModuleConnection reverseControl = forwardControl;
        reverseControl.fromModuleId = target.id;
        reverseControl.toModuleId = source.id;
        project->addConnection(forwardControl);
        project->addConnection(reverseControl);
        result = actor.executeTool("validate_flow", {});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("invalid"));
        const QJsonArray controlErrors = result.value(QStringLiteral("errors")).toArray();
        bool sawControlCycle = false;
        for (const QJsonValue& error : controlErrors) {
            if (error.toObject()
                    .value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("unstructured cycle")))
                sawControlCycle = true;
        }
        QVERIFY(sawControlCycle);
        QCOMPARE(engine.modules(), beforeModules);
        QVERIFY(!engine.isBusy());
        QCOMPARE(syncSpy.count(), 0);
    }

    void testG4ValidateFlowDoesNotRequestGuiSyncWhileBusy() {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance instance;
        instance.id = QStringLiteral("agent_blocking");
        instance.moduleId = QStringLiteral("com.deeplux.test.agentblocking");
        project->addModule(instance);

        RunEngine& engine = RunEngine::instance();
        QVERIFY(engine.loadProject(project, [](const ModuleInstance&) { return new BlockingAgentModule(); }));
        engine.setModuleExecutionAffinityForTest(QStringLiteral("agent_blocking"), ExecutionAffinity::Worker);
        const QList<ModuleBase*> beforeModules = engine.modules();
        QVERIFY(engine.runOnceAsync());
        QTRY_VERIFY_WITH_TIMEOUT(engine.isBusy(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(
            static_cast<BlockingAgentModule*>(beforeModules.first())->m_entered.load(std::memory_order_acquire), 1000);

        AgentActor actor;
        QSignalSpy syncSpy(&actor, &AgentActor::runEngineSyncRequested);
        const QJsonObject result = actor.executeTool("validate_flow", {});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("ENGINE_BUSY"));
        QCOMPARE(syncSpy.count(), 0);
        QCOMPARE(engine.modules(), beforeModules);

        QString syncError;
        QVERIFY(!engine.synchronizeProjectModules(project, beforeModules, syncError));
        QVERIFY(syncError.contains(QStringLiteral("running"), Qt::CaseInsensitive));
        QCOMPARE(engine.modules(), beforeModules);
        engine.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 2000);
    }

    // 阶7 Agent 评审修复（P1-3）：remove_module 的 undo 恢复模块与关联连接；
    // add_module 的 instance id 在 undo/redo 间固定（连接不悬空）
    void testRemoveModuleUndoRestoresConnectionsAndStableIds() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        QJsonObject addG = actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "g"}});
        QVERIFY(!addG.contains("error"));
        QJsonObject addM =
            actor.executeTool("add_module", QJsonObject{{"plugin", "MeasureCircle"}, {"instanceName", "m"}});
        QVERIFY(!addM.contains("error"));
        QJsonObject conn = actor.executeTool("connect_modules", QJsonObject{{"fromId", "g"}, {"toId", "m"}});
        QCOMPARE(conn.value("status").toString(), QString("connected"));
        QCOMPARE(project->connections().size(), 1);

        QJsonObject rm = actor.executeTool("remove_module", QJsonObject{{"instanceId", "m"}});
        QCOMPARE(rm.value("status").toString(), QString("removed"));
        QCOMPARE(project->connections().size(), 0);
        QVERIFY(!project->moduleById(QStringLiteral("m")).has_value());

        QVERIFY(actor.undoStack()->canUndo());
        actor.undoStack()->undo(); // 撤销 remove_module
        QVERIFY(project->moduleById(QStringLiteral("m")).has_value());
        QCOMPARE(project->connections().size(), 1);

        actor.undoStack()->undo(); // 撤销 connect
        QCOMPARE(project->connections().size(), 0);
        actor.undoStack()->redo(); // 重做 connect：id 固定，连接恢复
        QCOMPARE(project->connections().size(), 1);
        QVERIFY(project->moduleById(QStringLiteral("m")).has_value());
    }

    void testG4RemoveUndoRestoresExactModuleAndConnectionOrder() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        for (const QString& id : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}) {
            ModuleInstance instance;
            instance.id = id;
            instance.moduleId = QStringLiteral("GrabImage");
            instance.name = id + QStringLiteral(" display");
            instance.note = id + QStringLiteral(" note");
            instance.params.insert(QStringLiteral("marker"), id);
            project->addModule(instance);
        }
        for (const auto& edge :
             {qMakePair(QStringLiteral("a"), QStringLiteral("b")), qMakePair(QStringLiteral("a"), QStringLiteral("c")),
              qMakePair(QStringLiteral("b"), QStringLiteral("c")),
              qMakePair(QStringLiteral("c"), QStringLiteral("a"))}) {
            ModuleConnection connection;
            connection.fromModuleId = edge.first;
            connection.toModuleId = edge.second;
            const bool control = edge.first == QLatin1String("b") && edge.second == QLatin1String("c");
            connection.fromPort = control ? QStringLiteral("true") : QStringLiteral("image");
            connection.toPort = control ? QStringLiteral("control") : QStringLiteral("image");
            connection.edgeType = control ? QStringLiteral("control") : QStringLiteral("data");
            project->addConnection(connection);
        }
        const QJsonArray modulesBefore = project->toJson().value("modules").toArray();
        const QJsonArray connectionsBefore = project->toJson().value("connections").toArray();

        QCOMPARE(actor.executeTool("remove_module", QJsonObject{{"instanceId", "b"}}).value("status").toString(),
                 QStringLiteral("removed"));
        QCOMPARE(project->modules().size(), 2);
        QCOMPARE(project->connections().size(), 2);
        actor.undoStack()->undo();
        QCOMPARE(project->toJson().value("modules").toArray(), modulesBefore);
        QCOMPARE(project->toJson().value("connections").toArray(), connectionsBefore);
        QTemporaryDir savedProjectDir;
        QVERIFY(savedProjectDir.isValid());
        const QString savedPath = savedProjectDir.filePath(QStringLiteral("restored-order.dproj"));
        QVERIFY(project->save(savedPath));
        Project reopened;
        QVERIFY(reopened.load(savedPath));
        QCOMPARE(reopened.toJson().value("modules").toArray(), modulesBefore);
        QCOMPARE(reopened.toJson().value("connections").toArray(), connectionsBefore);
        actor.undoStack()->redo();
        QCOMPARE(project->modules().size(), 2);
        QCOMPARE(project->connections().size(), 2);
        actor.undoStack()->undo();
        QCOMPARE(project->toJson().value("modules").toArray(), modulesBefore);
        QCOMPARE(project->toJson().value("connections").toArray(), connectionsBefore);
    }

    void testG4MoveModuleValidatesAndRestoresOrder() {
        AgentActor actor;
        QCOMPARE(actor.executeTool("move_module", QJsonObject{{"instanceId", "a"}, {"index", 0}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("NO_PROJECT"));
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        for (const QString& id : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}) {
            ModuleInstance instance;
            instance.id = id;
            instance.moduleId = QStringLiteral("GrabImage");
            instance.name = id;
            project->addModule(instance);
        }
        const QJsonArray original = project->toJson().value("modules").toArray();
        const int undoCount = actor.undoStack()->count();
        for (const QJsonValue& invalidIndex :
             {QJsonValue(-1), QJsonValue(3), QJsonValue(1.5), QJsonValue(QStringLiteral("1"))}) {
            QCOMPARE(actor.executeTool("move_module", QJsonObject{{"instanceId", "b"}, {"index", invalidIndex}})
                         .value("errorCode")
                         .toString(),
                     QStringLiteral("INVALID_PARAM"));
        }
        QCOMPARE(actor.executeTool("move_module", QJsonObject{{"instanceId", "missing"}, {"index", 0}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(actor.undoStack()->count(), undoCount);
        QCOMPARE(project->toJson().value("modules").toArray(), original);

        const QJsonObject moved = actor.executeTool("move_module", QJsonObject{{"instanceId", "b"}, {"index", 0}});
        QCOMPARE(moved.value("status").toString(), QStringLiteral("moved"));
        QCOMPARE(moved.value("oldIndex").toInt(), 1);
        QCOMPARE(project->modules().at(0).id, QStringLiteral("b"));
        QCOMPARE(actor.undoStack()->count(), undoCount + 1);
        QCOMPARE(
            actor.executeTool("move_module", QJsonObject{{"instanceId", "b"}, {"index", 0}}).value("status").toString(),
            QStringLiteral("no_op"));
        QCOMPARE(actor.undoStack()->count(), undoCount + 1);
        const QJsonArray reordered = project->toJson().value("modules").toArray();
        actor.undoStack()->undo();
        QCOMPARE(project->toJson().value("modules").toArray(), original);
        actor.undoStack()->redo();
        QCOMPARE(project->toJson().value("modules").toArray(), reordered);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Project reopened;
        QVERIFY(project->save(dir.filePath(QStringLiteral("ordered.dproj"))));
        QVERIFY(reopened.load(dir.filePath(QStringLiteral("ordered.dproj"))));
        QCOMPARE(reopened.toJson().value("modules").toArray(), reordered);

        QJsonObject duplicateIdProject = project->toJson();
        QJsonArray duplicateIdModules = duplicateIdProject.value(QStringLiteral("modules")).toArray();
        QJsonObject duplicate = duplicateIdModules.at(1).toObject();
        duplicate.insert(QStringLiteral("id"), duplicateIdModules.at(0).toObject().value(QStringLiteral("id")));
        duplicateIdModules.replace(1, duplicate);
        duplicateIdProject.insert(QStringLiteral("modules"), duplicateIdModules);
        QTemporaryFile duplicateIdFile(dir.filePath(QStringLiteral("duplicate-id-XXXXXX.dproj")));
        QVERIFY(duplicateIdFile.open());
        const QByteArray duplicateIdBytes = QJsonDocument(duplicateIdProject).toJson(QJsonDocument::Compact);
        QCOMPARE(duplicateIdFile.write(duplicateIdBytes), duplicateIdBytes.size());
        QVERIFY(duplicateIdFile.flush());
        QVERIFY(project->load(duplicateIdFile.fileName())); // Existing files may contain duplicate IDs.
        QCOMPARE(project->modules().at(0).id, project->modules().at(1).id);

        actor.undoStack()->clear();
        const QJsonObject beforeAmbiguousMove = project->toJson();
        const quint64 generationBeforeAmbiguousMove = project->moduleOrderExternalGeneration();
        const QJsonObject ambiguousMove = actor.executeTool(
            QStringLiteral("move_module"),
            QJsonObject{{QStringLiteral("instanceId"), project->modules().at(0).id}, {QStringLiteral("index"), 1}});
        QCOMPARE(ambiguousMove.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_STATE"));
        QCOMPARE(actor.undoStack()->count(), 0);
        QCOMPARE(project->toJson(), beforeAmbiguousMove);
        QCOMPARE(project->moduleOrderExternalGeneration(), generationBeforeAmbiguousMove);
    }

    void testG4MoveModulePreservesAgentStructuralLifo() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        actor.undoStack()->clear();
        for (const QString& id : {QStringLiteral("order_a"), QStringLiteral("order_b"), QStringLiteral("order_c")}) {
            ModuleInstance instance;
            instance.id = id;
            instance.moduleId = QStringLiteral("GrabImage");
            instance.name = id;
            project->addModule(instance);
        }
        const quint64 externalGeneration = project->moduleOrderExternalGeneration();
        auto assertOrder = [project](const QStringList& expected) {
            QStringList actual;
            for (const ModuleInstance& module : project->modules())
                actual.append(module.id);
            QCOMPARE(actual, expected);
        };
        auto move = [&actor](const QString& id, int index) {
            QCOMPARE(actor
                         .executeTool(QStringLiteral("move_module"),
                                      QJsonObject{{QStringLiteral("instanceId"), id}, {QStringLiteral("index"), index}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("moved"));
        };

        move(QStringLiteral("order_c"), 0);
        move(QStringLiteral("order_a"), 2);
        QCOMPARE(actor
                     .executeTool(QStringLiteral("add_module"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("GrabImage")},
                                              {QStringLiteral("instanceName"), QStringLiteral("order_d")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("added"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("remove_module"),
                                  QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("order_b")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("removed"));
        assertOrder({QStringLiteral("order_c"), QStringLiteral("order_a"), QStringLiteral("order_d")});
        QCOMPARE(project->moduleOrderExternalGeneration(), externalGeneration);

        actor.undoStack()->undo(); // Restore B at its saved index.
        assertOrder({QStringLiteral("order_c"), QStringLiteral("order_b"), QStringLiteral("order_a"),
                     QStringLiteral("order_d")});
        actor.undoStack()->undo(); // Undo Agent add D.
        assertOrder({QStringLiteral("order_c"), QStringLiteral("order_b"), QStringLiteral("order_a")});
        actor.undoStack()->undo(); // Undo the second move.
        assertOrder({QStringLiteral("order_c"), QStringLiteral("order_a"), QStringLiteral("order_b")});
        actor.undoStack()->undo(); // Undo the first move.
        assertOrder({QStringLiteral("order_a"), QStringLiteral("order_b"), QStringLiteral("order_c")});
        QCOMPARE(project->moduleOrderExternalGeneration(), externalGeneration);

        actor.undoStack()->redo();
        actor.undoStack()->redo();
        actor.undoStack()->redo();
        actor.undoStack()->redo();
        assertOrder({QStringLiteral("order_c"), QStringLiteral("order_a"), QStringLiteral("order_d")});
        QCOMPARE(project->moduleOrderExternalGeneration(), externalGeneration);
    }

    void testG4SetModulePositionValidatesSignalsUndoAndPersistence() {
        qRegisterMetaType<ModuleInstance>();
        AgentActor actor;
        QCOMPARE(actor.executeTool("set_module_position", QJsonObject{{"instanceId", "node"}, {"x", 0}, {"y", 0}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("NO_PROJECT"));
        QCOMPARE(actor.executeTool("set_module_position", QJsonObject{{"instanceId", "node"}, {"x", 0}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("MISSING_PARAM"));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance first;
        first.id = QStringLiteral("node");
        first.moduleId = QStringLiteral("GrabImage");
        first.name = QStringLiteral("stable node");
        first.posX = 10;
        first.posY = 20;
        first.params = QJsonObject{{"keep", 7}};
        project->addModule(first);
        ModuleInstance other;
        other.id = QStringLiteral("other");
        other.moduleId = QStringLiteral("GrabImage");
        other.posX = -30;
        other.posY = 45;
        project->addModule(other);
        ModuleConnection edge;
        edge.fromModuleId = first.id;
        edge.toModuleId = other.id;
        edge.fromPort = QStringLiteral("image");
        edge.toPort = QStringLiteral("image");
        edge.edgeType = QStringLiteral("data");
        project->addConnection(edge);
        project->setModified(false);

        QSignalSpy updatedSpy(project, &Project::moduleUpdated);
        QSignalSpy positionSpy(project, &Project::modulePositionChanged);
        QSignalSpy syncSpy(&actor, &AgentActor::modulePositionSyncRequested);
        const int initialUndoCount = actor.undoStack()->count();
        const int initialUndoIndex = actor.undoStack()->index();
        const QJsonArray originalModules = project->toJson().value(QStringLiteral("modules")).toArray();
        const QJsonObject otherBefore = project->moduleById(QStringLiteral("other"))->toJson();
        const QJsonArray originalConnections = project->toJson().value(QStringLiteral("connections")).toArray();

        for (const QJsonObject& invalid : {QJsonObject{{"instanceId", "node"}, {"x", 1.5}, {"y", 2}},
                                           QJsonObject{{"instanceId", "node"}, {"x", "1"}, {"y", 2}},
                                           QJsonObject{{"instanceId", "node"}, {"x", ""}, {"y", 2}},
                                           QJsonObject{{"instanceId", "node"}, {"x", 1}, {"y", false}},
                                           QJsonObject{{"instanceId", 17}, {"x", 1}, {"y", 2}},
                                           QJsonObject{{"instanceId", true}, {"x", 1}, {"y", 2}},
                                           QJsonObject{{"instanceId", "  "}, {"x", 1}, {"y", 2}},
                                           QJsonObject{{"instanceId", "node"}, {"x", 2147483648.0}, {"y", 2}}}) {
            QCOMPARE(actor.executeTool("set_module_position", invalid).value("errorCode").toString(),
                     QStringLiteral("INVALID_PARAM"));
        }
        QCOMPARE(actor.executeTool("set_module_position", QJsonObject{{"instanceId", "missing"}, {"x", 1}, {"y", 2}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(updatedSpy.count(), 0);
        QCOMPARE(positionSpy.count(), 0);
        QCOMPARE(actor.undoStack()->count(), initialUndoCount);
        QCOMPARE(actor.undoStack()->index(), initialUndoIndex);
        QVERIFY(!project->isModified());

        const QJsonObject changed =
            actor.executeTool("set_module_position", QJsonObject{{"instanceId", "node"}, {"x", -100}, {"y", 0}});
        QCOMPARE(changed.value("status").toString(), QStringLiteral("updated"));
        QCOMPARE(changed.value("oldX").toInt(), 10);
        QCOMPARE(changed.value("oldY").toInt(), 20);
        QCOMPARE(updatedSpy.count(), 1);
        QCOMPARE(positionSpy.count(), 1);
        QCOMPARE(actor.undoStack()->count(), initialUndoCount + 1);
        QCOMPARE(actor.undoStack()->index(), initialUndoIndex + 1);
        QVERIFY(project->isModified());
        const ModuleInstance changedNode = *project->moduleById(QStringLiteral("node"));
        QCOMPARE(changedNode.posX, -100);
        QCOMPARE(changedNode.posY, 0);
        QCOMPARE(changedNode.id, first.id);
        QCOMPARE(changedNode.params, first.params);
        QCOMPARE(project->moduleById(QStringLiteral("other"))->toJson(), otherBefore);
        QCOMPARE(project->modules().at(0).id, QStringLiteral("node"));
        QCOMPARE(project->modules().at(1).id, QStringLiteral("other"));
        QCOMPARE(project->toJson().value(QStringLiteral("connections")).toArray(), originalConnections);

        actor.undoStack()->undo();
        QCOMPARE(project->moduleById(QStringLiteral("node"))->posX, 10);
        QCOMPARE(project->moduleById(QStringLiteral("node"))->posY, 20);
        QCOMPARE(updatedSpy.count(), 2);
        QCOMPARE(positionSpy.count(), 2);
        QCOMPARE(project->toJson().value(QStringLiteral("modules")).toArray(), originalModules);
        actor.undoStack()->redo();
        QCOMPARE(project->moduleById(QStringLiteral("node"))->posX, -100);
        QCOMPARE(project->moduleById(QStringLiteral("node"))->posY, 0);
        QCOMPARE(updatedSpy.count(), 3);
        QCOMPARE(positionSpy.count(), 3);

        project->setModified(false);
        const int modifiedUndoCount = actor.undoStack()->count();
        const int modifiedUndoIndex = actor.undoStack()->index();
        QCOMPARE(actor.executeTool("set_module_position", QJsonObject{{"instanceId", "node"}, {"x", -100}, {"y", 0}})
                     .value("status")
                     .toString(),
                 QStringLiteral("no_op"));
        QCOMPARE(updatedSpy.count(), 3);
        QCOMPARE(positionSpy.count(), 3);
        QCOMPARE(syncSpy.count(), 1);
        QCOMPARE(actor.undoStack()->count(), modifiedUndoCount);
        QCOMPARE(actor.undoStack()->index(), modifiedUndoIndex);
        QVERIFY(!project->isModified());

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Project reopened;
        const QString path = dir.filePath(QStringLiteral("position.dproj"));
        QVERIFY(project->save(path));
        QVERIFY(reopened.load(path));
        QCOMPARE(reopened.moduleById(QStringLiteral("node"))->posX, -100);
        QCOMPARE(reopened.moduleById(QStringLiteral("node"))->posY, 0);
        QCOMPARE(reopened.toJson().value(QStringLiteral("connections")).toArray(), originalConnections);
    }

    void testG4SetModulePositionRejectsBusyEngineAndInvalidValuesFirst() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("agent_blocking");
        instance.moduleId = QStringLiteral("com.deeplux.test.agentblocking");
        instance.posX = 10;
        instance.posY = 20;
        project->addModule(instance);

        RunEngine& engine = RunEngine::instance();
        QVERIFY(engine.loadProject(project, [](const ModuleInstance&) { return new BlockingAgentModule(); }));
        const QList<ModuleBase*> modules = engine.modules();
        QVERIFY(!modules.isEmpty());
        QVERIFY(engine.runOnceAsync());
        auto* blocking = static_cast<BlockingAgentModule*>(modules.first());
        QTRY_VERIFY_WITH_TIMEOUT(blocking->m_entered.load(std::memory_order_acquire), 1000);

        QSignalSpy updatedSpy(project, &Project::moduleUpdated);
        QSignalSpy positionSpy(project, &Project::modulePositionChanged);
        const int undoCount = actor.undoStack()->count();
        QCOMPARE(
            actor.executeTool("set_module_position", QJsonObject{{"instanceId", instance.id}, {"x", 10.5}, {"y", 20}})
                .value("errorCode")
                .toString(),
            QStringLiteral("INVALID_PARAM"));
        QCOMPARE(
            actor.executeTool("set_module_position", QJsonObject{{"instanceId", instance.id}, {"x", 30}, {"y", 40}})
                .value("errorCode")
                .toString(),
            QStringLiteral("ENGINE_BUSY"));
        QCOMPARE(updatedSpy.count(), 0);
        QCOMPARE(positionSpy.count(), 0);
        QCOMPARE(actor.undoStack()->count(), undoCount);
        QCOMPARE(project->moduleById(instance.id)->posX, 10);
        QCOMPARE(project->moduleById(instance.id)->posY, 20);

        engine.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 2000);
    }

    void testG4SetModulePositionUndoLifoAndExpiresAcrossGuiEdits() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("position_lifo");
        instance.moduleId = QStringLiteral("GrabImage");
        project->addModule(instance);
        QSignalSpy expiredSpy(&actor, &AgentActor::positionUndoExpired);

        QCOMPARE(
            actor.executeTool("set_module_position", QJsonObject{{"instanceId", instance.id}, {"x", 10}, {"y", 20}})
                .value("status")
                .toString(),
            QStringLiteral("updated"));
        QCOMPARE(
            actor.executeTool("set_module_position", QJsonObject{{"instanceId", instance.id}, {"x", 30}, {"y", 40}})
                .value("status")
                .toString(),
            QStringLiteral("updated"));
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        actor.undoStack()->undo();
        QCOMPARE(project->moduleById(instance.id)->posX, 10);
        QCOMPARE(project->moduleById(instance.id)->posY, 20);
        actor.undoStack()->undo();
        QCOMPARE(project->moduleById(instance.id)->posX, 0);
        QCOMPARE(project->moduleById(instance.id)->posY, 0);
        actor.undoStack()->redo();
        QCOMPARE(project->moduleById(instance.id)->posX, 10);
        actor.undoStack()->redo();
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        QCOMPARE(project->positionGenerations(instance.id).agent, quint64(0));

        ModuleInstance guiUpdated = *project->moduleById(instance.id);
        guiUpdated.posX = 0;
        guiUpdated.posY = 0;
        project->updateModule(instance.id, guiUpdated, Project::PositionChangeSource::Gui);
        QCOMPARE(project->positionGenerations(instance.id).agent, quint64(1));
        guiUpdated.posX = 30;
        guiUpdated.posY = 40;
        project->updateModule(instance.id, guiUpdated, Project::PositionChangeSource::Gui);
        QCOMPARE(project->positionGenerations(instance.id).agent, quint64(2));
        const int count = actor.undoStack()->count();
        const int index = actor.undoStack()->index();
        actor.undoStack()->undo();
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        QCOMPARE(project->moduleById(instance.id)->posY, 40);
        QCOMPARE(actor.undoStack()->count(), count);
        QCOMPARE(actor.undoStack()->index(), index - 1);
        QCOMPARE(expiredSpy.count(), 1);
        actor.undoStack()->redo();
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        QCOMPARE(project->moduleById(instance.id)->posY, 40);
        QCOMPARE(actor.undoStack()->index(), index);
        QCOMPARE(expiredSpy.count(), 2); // Re-navigating a permanently stale command reports expired again.
    }

    void testG4MixedBatchIsolatesStalePositionUndoFromSiblingCommands() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("batch_position");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("Before");
        project->addModule(instance);

        const QList<AgentActor::ToolInvocation> calls{
            {QStringLiteral("rename-before-call"), QStringLiteral("rename_module"),
             QJsonObject{{"instanceId", instance.id}, {"name", QStringLiteral("BeforePosition")}}},
            {QStringLiteral("position-call"), QStringLiteral("set_module_position"),
             QJsonObject{{"instanceId", instance.id}, {"x", 10}, {"y", 20}}},
            {QStringLiteral("rename-after-call"), QStringLiteral("rename_module"),
             QJsonObject{{"instanceId", instance.id}, {"name", QStringLiteral("AfterPosition")}}},
        };
        const QJsonObject result = actor.executeTools(calls, QStringLiteral("mixed position batch"));
        QCOMPARE(result.value("status").toString(), QStringLiteral("completed"));
        QCOMPARE(result.value("executed").toInt(), 3);
        const QJsonArray results = result.value("results").toArray();
        QCOMPARE(results.size(), 3);
        QCOMPARE(results[0].toObject().value("callId").toString(), QStringLiteral("rename-before-call"));
        QCOMPARE(results[1].toObject().value("callId").toString(), QStringLiteral("position-call"));
        QCOMPARE(results[2].toObject().value("callId").toString(), QStringLiteral("rename-after-call"));
        QCOMPARE(actor.undoStack()->count(), 3);
        QCOMPARE(actor.undoStack()->index(), 3);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("AfterPosition"));

        ModuleInstance guiUpdated = *project->moduleById(instance.id);
        guiUpdated.posX = 30;
        guiUpdated.posY = 40;
        project->updateModule(instance.id, guiUpdated, Project::PositionChangeSource::Gui);
        QVERIFY(actor.undoLastAction()); // Undo the rename macro after the position command.
        QCOMPARE(actor.undoStack()->index(), 2);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("BeforePosition"));
        const int staleIndex = actor.undoStack()->index();
        QVERIFY(!actor.undoLastAction()); // The stale position command must not affect siblings.
        QCOMPARE(actor.undoStack()->index(), staleIndex - 1);
        QCOMPARE(actor.undoStack()->count(), 3);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("BeforePosition"));
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        QCOMPARE(project->moduleById(instance.id)->posY, 40);
        QVERIFY(actor.undoLastAction()); // The earlier rename remains independently undoable.
        QCOMPARE(actor.undoStack()->index(), 0);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Before"));

        actor.undoStack()->redo(); // Redo the rename before the stale position item.
        QCOMPARE(actor.undoStack()->index(), 1);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("BeforePosition"));
        actor.undoStack()->redo(); // Permanently expired position command remains a no-op.
        QCOMPARE(actor.undoStack()->index(), 2);
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("BeforePosition"));
        actor.undoStack()->redo();
        QCOMPARE(actor.undoStack()->index(), 3);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("AfterPosition"));
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
    }

    void testG4RenameKeepsIdentityConnectionsAndUndo() {
        AgentActor actor;
        QCOMPARE(actor.executeTool("rename_module", QJsonObject{{"instanceId", "b"}, {"name", "New label"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("NO_PROJECT"));
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        for (const QString& id : {QStringLiteral("a"), QStringLiteral("b")}) {
            ModuleInstance instance;
            instance.id = id;
            instance.moduleId = QStringLiteral("GrabImage");
            instance.name = id;
            instance.params.insert(QStringLiteral("marker"), id);
            project->addModule(instance);
        }
        ModuleConnection connection;
        connection.fromModuleId = QStringLiteral("a");
        connection.toModuleId = QStringLiteral("b");
        connection.fromPort = QStringLiteral("image");
        connection.toPort = QStringLiteral("image");
        project->addConnection(connection);
        const QJsonArray connections = project->toJson().value("connections").toArray();
        const QJsonArray before = project->toJson().value("modules").toArray();
        const int undoCount = actor.undoStack()->count();
        QCOMPARE(actor.executeTool("rename_module", QJsonObject{{"instanceId", "missing"}, {"name", "x"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(actor.executeTool("rename_module", QJsonObject{{"instanceId", "b"}, {"name", 42}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        QCOMPARE(actor.executeTool("rename_module", QJsonObject{{"instanceId", "b"}, {"name", "   "}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("MISSING_PARAM"));
        QCOMPARE(actor.undoStack()->count(), undoCount);
        QCOMPARE(project->toJson().value("modules").toArray(), before);

        const QJsonObject renamed =
            actor.executeTool("rename_module", QJsonObject{{"instanceId", "b"}, {"name", " New label "}});
        QCOMPARE(renamed.value("status").toString(), QStringLiteral("renamed"));
        QCOMPARE(renamed.value("oldName").toString(), QStringLiteral("b"));
        QCOMPARE(project->moduleById(QStringLiteral("b"))->name, QStringLiteral("New label"));
        QCOMPARE(project->moduleById(QStringLiteral("b"))->params.value("marker").toString(), QStringLiteral("b"));
        QCOMPARE(project->toJson().value("connections").toArray(), connections);
        QCOMPARE(actor.executeTool("rename_module", QJsonObject{{"instanceId", "b"}, {"name", "New label"}})
                     .value("status")
                     .toString(),
                 QStringLiteral("no_op"));
        QCOMPARE(actor.undoStack()->count(), undoCount + 1);
        const QJsonArray after = project->toJson().value("modules").toArray();
        actor.undoStack()->undo();
        QCOMPARE(project->toJson().value("modules").toArray(), before);
        actor.undoStack()->redo();
        QCOMPARE(project->toJson().value("modules").toArray(), after);
        QCOMPARE(project->toJson().value("connections").toArray(), connections);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("renamed.dproj"));
        QVERIFY(project->save(path));
        Project reopened;
        QVERIFY(reopened.load(path));
        QCOMPARE(reopened.toJson().value("modules").toArray(), after);
        QCOMPARE(reopened.toJson().value("connections").toArray(), connections);
    }

    void testG4SetModuleEnabledSyncsEngineAndUndo() {
        AgentActor actor;
        const QJsonObject request{{"instanceId", "a"}, {"enabled", false}};
        QCOMPARE(actor.executeTool("set_module_enabled", request).value("errorCode").toString(),
                 QStringLiteral("NO_PROJECT"));
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance instance;
        instance.id = QStringLiteral("a");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("Camera");
        instance.params.insert(QStringLiteral("marker"), 7);
        project->addModule(instance);
        const QJsonArray before = project->toJson().value("modules").toArray();
        const int undoCount = actor.undoStack()->count();
        QCOMPARE(actor.executeTool("set_module_enabled", QJsonObject{{"instanceId", "missing"}, {"enabled", false}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(actor.executeTool("set_module_enabled", QJsonObject{{"instanceId", "a"}, {"enabled", "false"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        QCOMPARE(
            actor.executeTool("set_module_enabled", QJsonObject{{"instanceId", "a"}}).value("errorCode").toString(),
            QStringLiteral("MISSING_PARAM"));
        QCOMPARE(actor.undoStack()->count(), undoCount);
        QCOMPARE(project->toJson().value("modules").toArray(), before);

        const QJsonObject updated = actor.executeTool("set_module_enabled", request);
        QCOMPARE(updated.value("status").toString(), QStringLiteral("updated"));
        QCOMPARE(updated.value("oldEnabled").toBool(), true);
        QCOMPARE(updated.value("enabled").toBool(), false);
        QVERIFY(!project->moduleById(QStringLiteral("a"))->enabled);
        QVERIFY(RunEngine::instance().isModuleDisabled(QStringLiteral("a")));
        QCOMPARE(project->moduleById(QStringLiteral("a"))->params.value("marker").toInt(), 7);
        QCOMPARE(actor.executeTool("set_module_enabled", request).value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(actor.undoStack()->count(), undoCount + 1);
        const QJsonArray after = project->toJson().value("modules").toArray();
        actor.undoStack()->undo();
        QCOMPARE(project->toJson().value("modules").toArray(), before);
        QVERIFY(!RunEngine::instance().isModuleDisabled(QStringLiteral("a")));
        actor.undoStack()->redo();
        QCOMPARE(project->toJson().value("modules").toArray(), after);
        QVERIFY(RunEngine::instance().isModuleDisabled(QStringLiteral("a")));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("disabled.dproj"));
        QVERIFY(project->save(path));
        Project reopened;
        QVERIFY(reopened.load(path));
        QCOMPARE(reopened.toJson().value("modules").toArray(), after);
    }

    void testG4SetBreakpointSyncsEngineAndUndo() {
        AgentActor actor;
        const QJsonObject request{{"instanceId", "bp_id"}, {"enabled", true}};
        QCOMPARE(actor.executeTool("set_breakpoint", request).value("errorCode").toString(),
                 QStringLiteral("NO_PROJECT"));
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance instance;
        instance.id = QStringLiteral("bp_id");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("Camera");
        project->addModule(instance);
        const QJsonArray before = project->toJson().value("modules").toArray();
        const int undoCount = actor.undoStack()->count();
        QCOMPARE(actor.executeTool("set_breakpoint", QJsonObject{{"instanceId", "missing"}, {"enabled", true}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(actor.executeTool("set_breakpoint", QJsonObject{{"instanceId", "bp_id"}, {"enabled", "true"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        QCOMPARE(
            actor.executeTool("set_breakpoint", QJsonObject{{"instanceId", "bp_id"}}).value("errorCode").toString(),
            QStringLiteral("MISSING_PARAM"));
        QCOMPARE(actor.undoStack()->count(), undoCount);
        QCOMPARE(project->toJson().value("modules").toArray(), before);

        const QJsonObject updated = actor.executeTool("set_breakpoint", request);
        QCOMPARE(updated.value("status").toString(), QStringLiteral("updated"));
        QCOMPARE(updated.value("oldBreakpoint").toBool(), false);
        QCOMPARE(updated.value("breakpoint").toBool(), true);
        QVERIFY(project->moduleById(QStringLiteral("bp_id"))->breakpoint);
        QVERIFY(RunEngine::instance().hasBreakpoint(QStringLiteral("bp_id")));
        QCOMPARE(actor.executeTool("set_breakpoint", request).value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(actor.undoStack()->count(), undoCount + 1);
        const QJsonArray after = project->toJson().value("modules").toArray();
        actor.undoStack()->undo();
        QCOMPARE(project->toJson().value("modules").toArray(), before);
        QVERIFY(!RunEngine::instance().hasBreakpoint(QStringLiteral("bp_id")));
        actor.undoStack()->redo();
        QCOMPARE(project->toJson().value("modules").toArray(), after);
        QVERIFY(RunEngine::instance().hasBreakpoint(QStringLiteral("bp_id")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("breakpoint.dproj"));
        QVERIFY(project->save(path));
        Project reopened;
        QVERIFY(reopened.load(path));
        QCOMPARE(reopened.toJson().value("modules").toArray(), after);
    }

    void testG4SetModuleNoteSupportsClearAndUndo() {
        AgentActor actor;
        const QJsonObject request{{"instanceId", "note_id"}, {"note", "inspection target"}};
        QCOMPARE(actor.executeTool("set_module_note", request).value("errorCode").toString(),
                 QStringLiteral("NO_PROJECT"));
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance instance;
        instance.id = QStringLiteral("note_id");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("Camera");
        instance.note = QStringLiteral("old note");
        project->addModule(instance);
        const QJsonArray before = project->toJson().value("modules").toArray();
        const int undoCount = actor.undoStack()->count();
        QCOMPARE(actor.executeTool("set_module_note", QJsonObject{{"instanceId", "missing"}, {"note", "x"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(actor.executeTool("set_module_note", QJsonObject{{"instanceId", "note_id"}, {"note", 42}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        QCOMPARE(actor.undoStack()->count(), undoCount);
        QCOMPARE(project->toJson().value("modules").toArray(), before);

        QCOMPARE(actor.executeTool("set_module_note", request).value("status").toString(), QStringLiteral("updated"));
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("inspection target"));
        QCOMPARE(actor.executeTool("set_module_note", request).value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(actor.undoStack()->count(), undoCount + 1);
        actor.undoStack()->undo();
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("old note"));
        actor.undoStack()->redo();
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("inspection target"));

        const QJsonObject clear{{"instanceId", instance.id}, {"note", QString()}};
        QCOMPARE(actor.executeTool("set_module_note", clear).value("status").toString(), QStringLiteral("updated"));
        QVERIFY(project->moduleById(instance.id)->note.isEmpty());
        actor.undoStack()->undo();
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("inspection target"));
        actor.undoStack()->redo();
        QVERIFY(project->moduleById(instance.id)->note.isEmpty());
        const QJsonArray after = project->toJson().value("modules").toArray();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("module-note.dproj"));
        QVERIFY(project->save(path));
        Project reopened;
        QVERIFY(reopened.load(path));
        QCOMPARE(reopened.toJson().value("modules").toArray(), after);
    }

    // ===== 阶段 1（G1）：失败语义、工程保护、批次协议 =====

    // G1-1：无工程/无路径/不可写路径保存都返回结构化错误;失败不改变工程状态
    void testSaveProjectErrorPaths() {
        AgentActor actor;
        const QJsonObject noProj = actor.executeTool("save_project", QJsonObject());
        QCOMPARE(noProj.value("errorCode").toString(), QStringLiteral("NO_PROJECT"));
        QCOMPARE(noProj.value("status").toString(), QStringLiteral("failed")); // 兼容字段保留

        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "sp"}}).contains("error"));
        const QJsonObject noPath = actor.executeTool("save_project", QJsonObject());
        QCOMPARE(noPath.value("errorCode").toString(), QStringLiteral("SAVE_FAILED"));
        const QJsonObject badPath =
            actor.executeTool("save_project", QJsonObject{{"path", "/nonexistent_dir_dlx/x.dproj"}});
        QCOMPARE(badPath.value("errorCode").toString(), QStringLiteral("SAVE_FAILED"));
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("sp"));
    }

    // G1-3：未保存工程切换的 取消/放弃/保存 三态;保存失败不继续、默认保留当前工程
    void testUnsavedProjectSwitchPolicies() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "A"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "g"}})
                     .contains("error"));
        Project* a = ProjectManager::instance().currentProject();
        QVERIFY(a && a->isModified());

        // 默认（cancel）：拒绝切换,保留当前工程,零副作用
        const QJsonObject blocked = actor.executeTool("create_project", QJsonObject{{"name", "B"}});
        QCOMPARE(blocked.value("errorCode").toString(), QStringLiteral("PROJECT_UNSAVED"));
        QCOMPARE(blocked.value("details").toObject().value("options").toArray().size(), 3);
        QCOMPARE(ProjectManager::instance().currentProject(), a);
        // 显式 cancel 同样拒绝（open_project 共用同一守卫）
        const QJsonObject blockedOpen =
            actor.executeTool("open_project", QJsonObject{{"path", QDir(m_appDataDir.path()).filePath("nope.dproj")},
                                                          {"onUnsaved", "cancel"}});
        QCOMPARE(blockedOpen.value("errorCode").toString(), QStringLiteral("PROJECT_UNSAVED"));
        QCOMPARE(ProjectManager::instance().currentProject(), a);
        // save 但当前工程从未保存（无路径）→ SAVE_FAILED,不继续切换
        const QJsonObject saveFail =
            actor.executeTool("create_project", QJsonObject{{"name", "B"}, {"onUnsaved", "save"}});
        QCOMPARE(saveFail.value("errorCode").toString(), QStringLiteral("SAVE_FAILED"));
        QCOMPARE(ProjectManager::instance().currentProject(), a);
        // discard：显式放弃后继续（比较名字而非指针——旧对象析构后堆地址可能被复用）
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "B"}, {"onUnsaved", "discard"}})
                     .contains("error"));
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("B"));
        // save 成功路径：B 加改动→save 落盘→已保存工程切换无需 onUnsaved
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "g2"}})
                     .contains("error"));
        const QString pathB = QDir(m_appDataDir.path()).filePath("b.dproj");
        QVERIFY(!actor.executeTool("save_project", QJsonObject{{"path", pathB}}).contains("error"));
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "C"}}).contains("error"));
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("C"));
        QVERIFY(QFileInfo::exists(pathB));

        // onUnsaved=save 成功分支（审核整改 R1）：已有路径的工程被修改后,
        // save 策略应先落盘最新状态再切换。（C 经 setName 即处于已修改态,先显式 discard）
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "S"}, {"onUnsaved", "discard"}})
                     .contains("error"));
        const QString pathS = QDir(m_appDataDir.path()).filePath("s.dproj");
        QVERIFY(!actor.executeTool("save_project", QJsonObject{{"path", pathS}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "gs"}})
                     .contains("error"));
        QVERIFY(
            !actor.executeTool("create_project", QJsonObject{{"name", "T"}, {"onUnsaved", "save"}}).contains("error"));
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("T"));
        QFile sf(pathS);
        QVERIFY(sf.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(sf.readAll()).contains(QStringLiteral("gs")),
                 "save policy must persist latest state before switching");
    }

    // G1（复审2 R3）：closeProjectChecked 返回值反映实际关闭结果——
    // 引擎忙时拒绝关闭（false + ENGINE_BUSY,工程保留）;停止后真实关闭。
    // 用循环运行模式制造可靠忙碌状态（单次快流程存在竞态）
    void testCloseProjectCheckedReflectsEngineBusy() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "busy_close"}}).contains("error"));
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "gb"}})
                     .contains("error"));
        QVERIFY(
            !actor.executeTool("set_param", QJsonObject{{"instanceId", "gb"}, {"key", "grabSource"}, {"value", "Demo"}})
                 .contains("error"));

        const QJsonObject run = actor.executeTool("run_flow", QJsonObject{{"mode", "cycle"}, {"maxCycles", 10}});
        QVERIFY2(!run.contains("error"), qPrintable(run.value("error").toString()));
        QTRY_VERIFY(RunEngine::instance().isBusy());

        QCOMPARE(actor.executeTool("move_module", QJsonObject{{"instanceId", "gb"}, {"index", 0}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("ENGINE_BUSY"));
        QCOMPARE(actor.executeTool("rename_module", QJsonObject{{"instanceId", "gb"}, {"name", "Busy"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("ENGINE_BUSY"));
        QCOMPARE(actor.executeTool("set_module_enabled", QJsonObject{{"instanceId", "gb"}, {"enabled", false}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("ENGINE_BUSY"));
        QCOMPARE(actor.executeTool("set_breakpoint", QJsonObject{{"instanceId", "gb"}, {"enabled", true}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("ENGINE_BUSY"));
        QCOMPARE(actor.executeTool("set_module_note", QJsonObject{{"instanceId", "gb"}, {"note", "busy"}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("ENGINE_BUSY"));

        // 运行中关闭 → false + ENGINE_BUSY,工程保留
        QString code, msg;
        QVERIFY(!ProjectManager::instance().closeProjectChecked(ProjectManager::UnsavedPolicy::Discard, &code, &msg));
        QCOMPARE(code, QStringLiteral("ENGINE_BUSY"));
        QVERIFY(ProjectManager::instance().hasProject());

        // 停止后关闭 → true,工程真实关闭
        RunEngine::instance().stop();
        QTRY_VERIFY(!RunEngine::instance().isBusy());
        QVERIFY(ProjectManager::instance().closeProjectChecked(ProjectManager::UnsavedPolicy::Discard, &code, &msg));
        QVERIFY(!ProjectManager::instance().hasProject());
    }

    // G1-7：批次协议——每个原始 callId 恰有一个结果;遇错停止后未执行项补
    // SKIPPED_DUE_TO_PREVIOUS_ERROR;预校验失败同样保留全部 callId 且零执行
    void testBatchProtocolKeepsCallIdIntegrity() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "batch"}}).contains("error"));

        QList<AgentActor::ToolInvocation> calls{
            {"call_1", "add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "b1"}}},
            {"call_2", "set_param", QJsonObject{{"instanceId", "ghost"}, {"key", "grabSource"}, {"value", "Demo"}}},
            {"call_3", "add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "b3"}}},
        };
        const QJsonObject out = actor.executeTools(calls, QStringLiteral("G1 batch"));
        QCOMPARE(out.value("status").toString(), QStringLiteral("partial_failed"));
        QCOMPARE(out.value("executed").toInt(), 1);
        QCOMPARE(out.value("failedTool").toString(), QStringLiteral("set_param"));
        QCOMPARE(out.value("failedCallId").toString(), QStringLiteral("call_2"));
        const QJsonArray results = out.value("results").toArray();
        QCOMPARE(results.size(), 3); // 每个原始 call 恰有一个结果
        const QJsonObject r0 = results[0].toObject();
        QCOMPARE(r0.value("callId").toString(), QStringLiteral("call_1"));
        QVERIFY(r0.value("executed").toBool());
        QVERIFY(!r0.value("result").toObject().contains("error"));
        const QJsonObject r1 = results[1].toObject();
        QCOMPARE(r1.value("callId").toString(), QStringLiteral("call_2"));
        QVERIFY(r1.value("executed").toBool()); // 尝试执行并失败
        QCOMPARE(r1.value("result").toObject().value("errorCode").toString(), QStringLiteral("INSTANCE_NOT_FOUND"));
        const QJsonObject r2 = results[2].toObject();
        QCOMPARE(r2.value("callId").toString(), QStringLiteral("call_3"));
        QVERIFY(!r2.value("executed").toBool()); // 未执行,零副作用
        QCOMPARE(r2.value("result").toObject().value("errorCode").toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(r2.value("result").toObject().value("details").toObject().value("failedCallId").toString(),
                 QStringLiteral("call_2"));
        // 未执行的 call_3 确实没有创建模块
        QVERIFY(!ProjectManager::instance().currentProject()->moduleById(QStringLiteral("b3")).has_value());

        // 预校验失败：整批执行前拒绝,全部 callId 保留,executed=false,零副作用
        QList<AgentActor::ToolInvocation> badCalls{
            {"call_a", "add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "ok1"}}},
            {"call_b", "add_module", QJsonObject{}}, // 缺 plugin → 预校验失败
        };
        const QJsonObject pre = actor.executeTools(badCalls, QStringLiteral("G1 prevalidation"));
        QCOMPARE(pre.value("status").toString(), QStringLiteral("partial_failed"));
        QCOMPARE(pre.value("executed").toInt(), 0);
        QCOMPARE(pre.value("failedCallId").toString(), QStringLiteral("call_b"));
        const QJsonArray preResults = pre.value("results").toArray();
        QCOMPARE(preResults.size(), 2);
        QCOMPARE(preResults[0].toObject().value("result").toObject().value("errorCode").toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QVERIFY(!preResults[0].toObject().value("executed").toBool());
        QCOMPARE(preResults[1].toObject().value("result").toObject().value("errorCode").toString(),
                 QStringLiteral("MISSING_PARAM"));
        QVERIFY(!ProjectManager::instance().currentProject()->moduleById(QStringLiteral("ok1")).has_value());
    }

    void testAgentModulePluginLifecycleRequiresConfirmationAndHonorsInstanceLease() {
        PluginManager& plugins = PluginManager::instance();
        QString pluginName;
        for (const PluginInfo& info : plugins.moduleInfos()) {
            if (!plugins.isPluginLoaded(info.name))
                continue;
            QString error;
            if (plugins.tryUnloadPlugin(info.name, &error)) {
                pluginName = info.name;
                break;
            }
            QCOMPARE(error, QStringLiteral("PLUGIN_IN_USE"));
        }
        QVERIFY2(!pluginName.isEmpty(), "Need a loaded, idle module plugin fixture");

        QSignalSpy loadedSpy(&plugins, &PluginManager::pluginLoaded);
        QSignalSpy failedSpy(&plugins, &PluginManager::pluginLoadFailed);
        QSignalSpy unloadedSpy(&plugins, &PluginManager::pluginUnloaded);
        AgentActor actor;
        auto moduleAvailability = [&actor, &pluginName]() {
            const QJsonArray items =
                actor.executeTool("get_available_plugins", QJsonObject()).value(QStringLiteral("items")).toArray();
            for (const QJsonValue& value : items) {
                const QJsonObject item = value.toObject();
                if (item.value(QStringLiteral("name")).toString() == pluginName)
                    return item;
            }
            return QJsonObject();
        };
        const QJsonObject initiallyLoaded = moduleAvailability();
        QVERIFY(initiallyLoaded.value(QStringLiteral("executableActions")).toArray().contains(QStringLiteral("load")));
        QVERIFY(initiallyLoaded.value(QStringLiteral("blockedActions"))
                    .toArray()
                    .contains(QJsonObject{{QStringLiteral("action"), QStringLiteral("unload")},
                                          {QStringLiteral("reasonCode"), QStringLiteral("PLUGIN_NOT_LOADED")}}));
        QJsonObject wrongThreadResult;
        QJsonObject wrongThreadAvailability;
        std::thread otherThread([&actor, &wrongThreadResult, &wrongThreadAvailability, &pluginName]() {
            wrongThreadResult = actor.executeTool("load_module_plugin", {{"plugin", pluginName}});
            wrongThreadAvailability = actor.executeTool("get_available_plugins", QJsonObject());
        });
        otherThread.join();
        QCOMPARE(wrongThreadResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("PLUGIN_THREAD_AFFINITY"));
        QJsonObject wrongThreadItem;
        for (const QJsonValue& value : wrongThreadAvailability.value(QStringLiteral("items")).toArray()) {
            if (value.toObject().value(QStringLiteral("name")).toString() == pluginName)
                wrongThreadItem = value.toObject();
        }
        QVERIFY(wrongThreadItem.value(QStringLiteral("blockedActions"))
                    .toArray()
                    .contains(QJsonObject{{QStringLiteral("action"), QStringLiteral("load")},
                                          {QStringLiteral("reasonCode"), QStringLiteral("PLUGIN_THREAD_AFFINITY")}}));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);

        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QJsonObject pending = controller.handleToolCall("load_module_plugin", {{"plugin", pluginName}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        const QJsonArray sideEffects = controller.pendingConfirmationPayload()
                                           .value(QStringLiteral("context"))
                                           .toObject()
                                           .value(QStringLiteral("sideEffects"))
                                           .toArray();
        QCOMPARE(sideEffects.size(), 1);
        const QString loadEffect = sideEffects.first().toString();
        QVERIFY(loadEffect.startsWith(QStringLiteral("plugin_load:%1 target=").arg(pluginName)));
        QVERIFY(loadEffect.contains(QStringLiteral("loaded=false")));
        QVERIFY(loadEffect.contains(QStringLiteral("sha256=")));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_VERIFY_WITH_TIMEOUT(plugins.isPluginLoaded(pluginName), 5000);
        QVERIFY(plugins.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 0);
        const QJsonObject loadedAvailability = moduleAvailability();
        QVERIFY(loadedAvailability.value(QStringLiteral("blockedActions"))
                    .toArray()
                    .contains(QJsonObject{{QStringLiteral("action"), QStringLiteral("load")},
                                          {QStringLiteral("reasonCode"), QStringLiteral("PLUGIN_ALREADY_LOADED")}}));
        QVERIFY(
            loadedAvailability.value(QStringLiteral("executableActions")).toArray().contains(QStringLiteral("unload")));

        const QJsonObject alreadyLoaded = actor.executeTool("load_module_plugin", {{"plugin", pluginName}});
        QCOMPARE(alreadyLoaded.value(QStringLiteral("status")).toString(), QStringLiteral("already_loaded"));
        QCOMPARE(loadedSpy.count(), 1);

        Project* busyProject = ProjectManager::instance().newProject();
        QVERIFY(busyProject);
        ModuleInstance blockingInstance;
        blockingInstance.id = QStringLiteral("agent_blocking");
        blockingInstance.moduleId = QStringLiteral("com.deeplux.test.agentblocking");
        busyProject->addModule(blockingInstance);
        RunEngine& engine = RunEngine::instance();
        QVERIFY(engine.loadProject(busyProject, [](const ModuleInstance&) { return new BlockingAgentModule(); }));
        QVERIFY(engine.runOnceAsync());
        QTRY_VERIFY_WITH_TIMEOUT(engine.isBusy(), 1000);
        const QJsonObject busyAvailability = moduleAvailability();
        QVERIFY(
            !busyAvailability.value(QStringLiteral("executableActions")).toArray().contains(QStringLiteral("unload")));
        QVERIFY(busyAvailability.value(QStringLiteral("blockedActions"))
                    .toArray()
                    .contains(QJsonObject{{QStringLiteral("action"), QStringLiteral("unload")},
                                          {QStringLiteral("reasonCode"), QStringLiteral("ENGINE_BUSY")}}));
        QCOMPARE(actor.executeTool("load_module_plugin", {{"plugin", pluginName}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("ENGINE_BUSY"));
        QCOMPARE(actor.executeTool("unload_module_plugin", {{"plugin", pluginName}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("ENGINE_BUSY"));
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(unloadedSpy.count(), 0);
        engine.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 2000);

        PluginManager::ModulePtr module = plugins.createManagedModule(pluginName);
        QVERIFY(module);
        const QJsonObject inUseAvailability = moduleAvailability();
        QVERIFY(
            !inUseAvailability.value(QStringLiteral("executableActions")).toArray().contains(QStringLiteral("unload")));
        QVERIFY(inUseAvailability.value(QStringLiteral("blockedActions"))
                    .toArray()
                    .contains(QJsonObject{{QStringLiteral("action"), QStringLiteral("unload")},
                                          {QStringLiteral("reasonCode"), QStringLiteral("PLUGIN_IN_USE")}}));
        const QJsonObject blockedUnload = actor.executeTool("unload_module_plugin", {{"plugin", pluginName}});
        QCOMPARE(blockedUnload.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PLUGIN_IN_USE"));
        QVERIFY(plugins.isPluginLoaded(pluginName));
        QCOMPARE(unloadedSpy.count(), 0);
        module.reset();

        const QJsonObject unloadPending = controller.handleToolCall("unload_module_plugin", {{"plugin", pluginName}});
        QCOMPARE(unloadPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(unloadedSpy.count(), 0);
        const QString unloadEffect = controller.pendingConfirmationPayload()
                                         .value(QStringLiteral("context"))
                                         .toObject()
                                         .value(QStringLiteral("sideEffects"))
                                         .toArray()
                                         .first()
                                         .toString();
        QVERIFY(unloadEffect.startsWith(QStringLiteral("plugin_unload:%1 target=").arg(pluginName)));
        QVERIFY(unloadEffect.contains(QStringLiteral("loaded=true")));
        QVERIFY(unloadEffect.contains(QStringLiteral("sha256=")));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_VERIFY_WITH_TIMEOUT(!plugins.isPluginLoaded(pluginName), 5000);
        QVERIFY(!plugins.isPluginLoaded(pluginName));
        QCOMPARE(unloadedSpy.count(), 1);
        const QJsonObject unloadedAvailability = moduleAvailability();
        QVERIFY(
            unloadedAvailability.value(QStringLiteral("executableActions")).toArray().contains(QStringLiteral("load")));
        QVERIFY(unloadedAvailability.value(QStringLiteral("blockedActions"))
                    .toArray()
                    .contains(QJsonObject{{QStringLiteral("action"), QStringLiteral("unload")},
                                          {QStringLiteral("reasonCode"), QStringLiteral("PLUGIN_NOT_LOADED")}}));
        const QJsonObject reloaded = actor.executeTool("load_module_plugin", {{"plugin", pluginName}});
        QCOMPARE(reloaded.value(QStringLiteral("status")).toString(), QStringLiteral("loaded"));
        QVERIFY(plugins.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 2);
        IModule* untrackedModule = plugins.createModule(pluginName);
        QVERIFY(untrackedModule);
        delete untrackedModule;
        const QJsonObject untrackedAvailability = moduleAvailability();
        QVERIFY(
            untrackedAvailability.value(QStringLiteral("blockedActions"))
                .toArray()
                .contains(QJsonObject{{QStringLiteral("action"), QStringLiteral("unload")},
                                      {QStringLiteral("reasonCode"), QStringLiteral("PLUGIN_LIFECYCLE_UNTRACKED")}}));
        const QJsonObject schema = actor.executeTool("get_module_params_schema", {{"plugin", pluginName}});
        QVERIFY(!schema.contains(QStringLiteral("error")));

        const QJsonObject unknown = actor.executeTool("load_module_plugin", {{"plugin", "missing-module-plugin"}});
        QCOMPARE(unknown.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PLUGIN_NOT_FOUND"));
        QCOMPARE(actor.executeTool("load_module_plugin", {{"plugin", QStringLiteral(" %1 ").arg(pluginName)}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        QCOMPARE(actor.executeTool("unload_module_plugin", {{"plugin", QStringLiteral(" %1 ").arg(pluginName)}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        QCOMPARE(loadedSpy.count(), 2);
        QCOMPARE(unloadedSpy.count(), 1);
        const QJsonObject missingBinary = actor.executeTool("load_module_plugin", {{"plugin", "MissingLibraryModule"}});
        QCOMPARE(missingBinary.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PLUGIN_LOAD_FAILED"));
        QVERIFY(missingBinary.value(QStringLiteral("details"))
                    .toObject()
                    .value(QStringLiteral("reason"))
                    .toString()
                    .contains(QStringLiteral("No library file found")));
        QCOMPARE(failedSpy.count(), 1);
        const QJsonObject ambiguous = actor.executeTool("load_module_plugin", {{"plugin", "SamPredict"}});
        QVERIFY2(ambiguous.value(QStringLiteral("errorCode")).toString() == QStringLiteral("PLUGIN_NAME_AMBIGUOUS"),
                 qPrintable(QString::fromUtf8(QJsonDocument(ambiguous).toJson(QJsonDocument::Compact))));
        const QList<PluginInfo> cameras = plugins.cameraInfos();
        if (!cameras.isEmpty()) {
            const QJsonObject unsupported =
                actor.executeTool("unload_module_plugin", {{"plugin", cameras.first().name}});
            QCOMPARE(unsupported.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("PLUGIN_LIFECYCLE_UNSUPPORTED"));
        }
        controller.clearConversation();
    }

    void testBatchValidationAndSkipKeepDisconnectHardwareStatus() {
        AgentActor actor;
        const QList<AgentActor::ToolInvocation> validationCalls{
            {"disconnect-invalid", "disconnect_camera", {}},
            {"disconnect-skipped", "disconnect_camera", {{"deviceId", "never-disconnected"}}},
        };
        const QJsonObject validation = actor.executeTools(validationCalls, QStringLiteral("disconnect validation"));
        QCOMPARE(validation.value(QStringLiteral("executed")).toInt(), 0);
        const QJsonArray validationResults = validation.value(QStringLiteral("results")).toArray();
        QCOMPARE(validationResults.size(), 2);
        const QJsonObject invalidResult = validationResults.at(0).toObject().value(QStringLiteral("result")).toObject();
        QCOMPARE(invalidResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("MISSING_PARAM"));
        QCOMPARE(invalidResult.value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("hardwareDisconnectConfirmed"))
                     .toBool(),
                 false);
        QVERIFY(invalidResult.value(QStringLiteral("retryable")).toBool());
        const QJsonObject preSkippedResult =
            validationResults.at(1).toObject().value(QStringLiteral("result")).toObject();
        QCOMPARE(preSkippedResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(preSkippedResult.value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("hardwareDisconnectConfirmed"))
                     .toBool(),
                 false);

        const QList<AgentActor::ToolInvocation> runtimeCalls{
            {"camera-not-found", "get_camera_details", {{"deviceId", "unavailable-for-batch-test"}}},
            {"disconnect-skipped-runtime", "disconnect_camera", {{"deviceId", "never-disconnected"}}},
        };
        const QJsonObject runtime = actor.executeTools(runtimeCalls, QStringLiteral("disconnect runtime skip"));
        const QJsonArray runtimeResults = runtime.value(QStringLiteral("results")).toArray();
        QCOMPARE(runtimeResults.size(), 2);
        QCOMPARE(runtimeResults.at(0)
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("CAMERA_NOT_FOUND"));
        const QJsonObject runtimeSkippedResult =
            runtimeResults.at(1).toObject().value(QStringLiteral("result")).toObject();
        QCOMPARE(runtimeSkippedResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(runtimeSkippedResult.value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("hardwareDisconnectConfirmed"))
                     .toBool(),
                 false);
    }

    // R1：remove_module 必须先在工程里校验实例存在；不存在时失败关闭——
    // 不得压撤销栈、不得改工程（否则 Agent 会误以为删除成功）
    void testRemoveModuleMissingInstanceFailsClosedWithoutUndoPush() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        QVERIFY(addDemoGrab(actor, QStringLiteral("keep")));

        const int undoIndexBefore = actor.undoStack()->index();
        const int moduleCountBefore = project->modules().size();

        const QJsonObject missing = actor.executeTool("remove_module", QJsonObject{{"instanceId", "does_not_exist"}});
        QCOMPARE(missing.value("errorCode").toString(), QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(missing.value("error").toString(), QStringLiteral("Module not found: does_not_exist"));
        QCOMPARE(actor.undoStack()->index(), undoIndexBefore);  // 未压栈
        QCOMPARE(project->modules().size(), moduleCountBefore); // 工程零修改
        QVERIFY(project->moduleById(QStringLiteral("keep")).has_value());

        // 无工程时同样失败关闭（不压栈）
        ProjectManager::instance().closeProject();
        const int undoIndexNoProject = actor.undoStack()->index();
        const QJsonObject noProject = actor.executeTool("remove_module", QJsonObject{{"instanceId", "keep"}});
        QCOMPARE(noProject.value("errorCode").toString(), QStringLiteral("NO_PROJECT"));
        QCOMPARE(actor.undoStack()->index(), undoIndexNoProject);
    }

    // R2：create_project 失败路径必须保留既有 error 文案并追加 code + message。
    // executeTool 的 mutatesProject 门禁会先返回 engine_busy，故直接调用 handler
    // 触发 ProjectManager::newProject() 的引擎忙失败分支（测试构建下可见）。
    void testCreateProjectFailureCarriesCode() {
        AgentActor actor;
        BlockingAgentModule blocking(1200);
        QVERIFY(blocking.initialize());
        RunEngine::instance().addModule(&blocking);
        QVERIFY(RunEngine::instance().runOnceAsync());
        QVERIFY(RunEngine::instance().isBusy());

        const QJsonObject failed = actor.executeTool("create_project", QJsonObject{{"name", "busy-create"}});
        QCOMPARE(failed.value("errorCode").toString(), QStringLiteral("ENGINE_BUSY"));
        QVERIFY(failed.value("retryable").toBool());

        RunEngine::instance().stop();
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
    }

    // --- 第一迭代：派发注册表 / 闭环 / 观测工具 ---

    void testSupportedToolsRegistryAndUnknownTool() {
        AgentActor actor;
        const QStringList supported = actor.supportedTools();
        QVERIFY(supported.size() >= 100);

        const QStringList newTools = {
            QStringLiteral("validate_workflow"),      QStringLiteral("get_node_runtime_outputs"),
            QStringLiteral("get_execution_logs"),     QStringLiteral("tune_node_and_verify"),
            QStringLiteral("capture_viewport_image"), QStringLiteral("export_node_output")};
        for (const QString& name : newTools)
            QVERIFY2(supported.contains(name), qPrintable(name));

        // schema ↔ 注册表双向一致（工具契约单源化）
        QStringList schemaNames;
        for (const ToolDefinition& def : ToolSchema::instance().allTools())
            schemaNames.append(def.name);
        schemaNames.sort();
        QCOMPARE(supported, schemaNames);

        // undoable 事实来源：写工程的工具为 true，其余 false
        QVERIFY(actor.toolUndoable(QStringLiteral("add_module")));
        QVERIFY(!actor.toolUndoable(QStringLiteral("tune_node_and_verify")));
        QVERIFY(!actor.toolUndoable(QStringLiteral("run_flow")));
        QVERIFY(!actor.toolUndoable(QStringLiteral("create_project")));
        QVERIFY(!actor.toolUndoable(QStringLiteral("save_project")));
        QVERIFY(!actor.toolUndoable(QStringLiteral("no_such_tool")));

        // 未知工具保持既有错误文案
        QJsonObject unknown = actor.executeTool("definitely_unknown_tool", QJsonObject());
        QCOMPARE(unknown.value("error").toString(), QStringLiteral("Unknown tool: definitely_unknown_tool"));
    }

    void testValidateWorkflow() {
        AgentActor actor;
        QCOMPARE(actor.executeTool("validate_workflow", QJsonObject()).value("code").toString(),
                 QStringLiteral("no_project"));

        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "validate"}}).contains("error"));
        QJsonObject empty = actor.executeTool("validate_workflow", QJsonObject());
        QCOMPARE(empty.value("ok").toBool(), false);
        QCOMPARE(empty.value("moduleCount").toInt(), 0);
        QCOMPARE(empty.value("issues").toArray().first().toObject().value("code").toString(),
                 QStringLiteral("no_modules"));

        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        QJsonObject ok = actor.executeTool("validate_workflow", QJsonObject());
        QCOMPARE(ok.value("ok").toBool(), true);
        QCOMPARE(ok.value("moduleCount").toInt(), 1);
        QVERIFY(ok.value("issues").toArray().isEmpty());

        // FitCircle 的必需输入 fit_points 未连接 → 结构化 issue
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "FitCircle"}, {"instanceName", "fit"}})
                     .contains("error"));
        QJsonObject missing = actor.executeTool("validate_workflow", QJsonObject());
        QCOMPARE(missing.value("ok").toBool(), false);
        bool found = false;
        for (const QJsonValue& value : missing.value("issues").toArray()) {
            const QJsonObject issue = value.toObject();
            if (issue.value("code").toString() == QStringLiteral("missing_required_input")) {
                QCOMPARE(issue.value("node").toString(), QStringLiteral("fit"));
                QCOMPARE(issue.value("port").toString(), QStringLiteral("fit_points"));
                found = true;
            }
        }
        QVERIFY2(found, "missing_required_input issue expected");

        // 引擎忙 → engine_busy（直接启动引擎，绕开工具的工程重载）
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params.insert(QStringLiteral("grabSource"), QStringLiteral("Demo"));
        project->addModule(grab);
        QCOMPARE(actor.executeTool("validate_workflow", QJsonObject()).value("ok").toBool(), true);

        BlockingAgentModule blocking(1500);
        QVERIFY(blocking.initialize());
        RunEngine::instance().addModule(&blocking);
        QVERIFY(RunEngine::instance().runOnceAsync());
        QCOMPARE(actor.executeTool("validate_workflow", QJsonObject()).value("code").toString(),
                 QStringLiteral("engine_busy"));
        RunEngine::instance().stop();
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
    }

    void testGetNodeRuntimeOutputs() {
        AgentActor actor;
        QCOMPARE(actor.executeTool("get_node_runtime_outputs", QJsonObject{{"nodeName", "x"}}).value("code").toString(),
                 QStringLiteral("no_project"));

        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "outputs"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));

        QCOMPARE(
            actor.executeTool("get_node_runtime_outputs", QJsonObject{{"nodeName", "nope"}}).value("code").toString(),
            QStringLiteral("node_not_found"));

        QJsonObject never = actor.executeTool("get_node_runtime_outputs", QJsonObject{{"nodeName", "grab"}});
        QCOMPARE(never.value("lastStatus").toString(), QStringLiteral("unknown"));
        QVERIFY(never.value("outputs").toObject().isEmpty());
        QVERIFY(never.contains("note"));

        QJsonObject run = actor.executeTool("run_flow", QJsonObject());
        QVERIFY2(run.value("success").toBool(),
                 qPrintable(QString::fromUtf8(QJsonDocument(run).toJson(QJsonDocument::Compact))));

        QJsonObject after = actor.executeTool("get_node_runtime_outputs", QJsonObject{{"nodeName", "grab"}});
        QCOMPARE(after.value("lastStatus").toString(), QStringLiteral("success"));
        QVERIFY(after.value("elapsedMs").toInt() >= 0);
        QVERIFY(after.value("frameId").toDouble() > 0);
        const QJsonObject imagePort = after.value("outputs").toObject().value("image").toObject();
        QCOMPARE(imagePort.value("type").toString(), QStringLiteral("image"));
        QCOMPARE(imagePort.value("present").toBool(), true);
        QVERIFY(imagePort.value("width").toInt() > 0);
        QVERIFY(imagePort.value("height").toInt() > 0);

        // 名称优先、其次实例 id：显示名与实例 id 不同也能解析
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);
        ModuleInstance second;
        second.id = QStringLiteral("grab-instance-1");
        second.moduleId = QStringLiteral("GrabImage");
        second.name = QStringLiteral("DisplayGrab");
        second.params.insert(QStringLiteral("grabSource"), QStringLiteral("Demo"));
        project->addModule(second);

        const QJsonObject byName =
            actor.executeTool("get_node_runtime_outputs", QJsonObject{{"nodeName", "DisplayGrab"}});
        QVERIFY2(byName.value("instanceId").toString() == QStringLiteral("grab-instance-1"),
                 qPrintable(QString::fromUtf8(QJsonDocument(byName).toJson(QJsonDocument::Compact))));
        QCOMPARE(byName.value("node").toString(), QStringLiteral("grab-instance-1"));
        QCOMPARE(actor.executeTool("get_node_runtime_outputs", QJsonObject{{"nodeName", "grab-instance-1"}})
                     .value("instanceId")
                     .toString(),
                 QStringLiteral("grab-instance-1"));

        // 端口值序列化：列表型端口（MeasurementInput 的 point1/point2 是数值列表）
        ModuleInstance measure;
        measure.id = QStringLiteral("measure-input");
        measure.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(measure);
        QJsonObject runWithMeasure = actor.executeTool("run_flow", QJsonObject());
        QVERIFY2(runWithMeasure.value("success").toBool(),
                 qPrintable(QString::fromUtf8(QJsonDocument(runWithMeasure).toJson(QJsonDocument::Compact))));
        QJsonObject listOutputs =
            actor.executeTool("get_node_runtime_outputs", QJsonObject{{"nodeName", "measure-input"}});
        QCOMPARE(listOutputs.value("lastStatus").toString(), QStringLiteral("success"));
        const QJsonObject point1 = listOutputs.value("outputs").toObject().value("point1").toObject();
        QCOMPARE(point1.value("type").toString(), QStringLiteral("list"));
        QCOMPARE(point1.value("count").toInt(), 2);
        QCOMPARE(point1.value("first").toDouble(), 0.0);
    }

    void testGetNodeRuntimeOutputsFailureStatus() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "failure"}}).contains("error"));
        // MeasureCircle 的必需输入 image 无上游 → 运行期失败（validateFlow 对 image 端口不做前置拦截）
        QVERIFY(!actor.executeTool("add_module", QJsonObject{{"plugin", "MeasureCircle"}, {"instanceName", "mc"}})
                     .contains("error"));

        QJsonObject run = actor.executeTool("run_flow", QJsonObject());
        QCOMPARE(run.value("success").toBool(), false);
        QCOMPARE(run.value("errorCode").toString(), QStringLiteral("RUN_FAILED"));
        QCOMPARE(run.value("failedModule").toString(), QStringLiteral("mc"));
        QVERIFY(!run.value("error").toString().isEmpty());

        QJsonObject outputs = actor.executeTool("get_node_runtime_outputs", QJsonObject{{"nodeName", "mc"}});
        QCOMPARE(outputs.value("lastStatus").toString(), QStringLiteral("failed"));
        QVERIFY(!outputs.value("errorMessage").toString().isEmpty());
    }

    void testGetExecutionLogsFilters() {
        AgentActor actor;
        Logger::instance().clearLogs();
        Logger::instance().info(QStringLiteral("alpha match"), QStringLiteral("Run"));
        Logger::instance().warning(QStringLiteral("BETA thing"), QStringLiteral("Run"));
        Logger::instance().error(QStringLiteral("gamma"), QStringLiteral("Run"));
        Logger::instance().debug(QStringLiteral("delta"), QStringLiteral("Run"));
        Logger::instance().info(QStringLiteral("epsilon"), QStringLiteral("Agent"));

        QJsonObject category = actor.executeTool("get_execution_logs", QJsonObject{{"category", "Run"}});
        QCOMPARE(category.value("total").toInt(), 4);
        QCOMPARE(category.value("entries").toArray().size(), 4);
        QCOMPARE(category.value("truncated").toBool(), false);
        for (const QJsonValue& value : category.value("entries").toArray())
            QCOMPARE(value.toObject().value("category").toString(), QStringLiteral("Run"));

        QJsonObject errors =
            actor.executeTool("get_execution_logs", QJsonObject{{"category", "Run"}, {"level", "error"}});
        QCOMPARE(errors.value("total").toInt(), 1);
        QCOMPARE(errors.value("entries").toArray().first().toObject().value("level").toString(),
                 QStringLiteral("error"));

        QJsonObject keyword = actor.executeTool("get_execution_logs", QJsonObject{{"keyword", "beta"}});
        QCOMPARE(keyword.value("total").toInt(), 1);
        QCOMPARE(keyword.value("entries").toArray().first().toObject().value("message").toString(),
                 QStringLiteral("BETA thing"));

        QJsonObject limited = actor.executeTool("get_execution_logs", QJsonObject{{"category", "Run"}, {"limit", 2}});
        QCOMPARE(limited.value("total").toInt(), 4);
        QCOMPARE(limited.value("truncated").toBool(), true);
        const QJsonArray limitedEntries = limited.value("entries").toArray();
        QCOMPARE(limitedEntries.size(), 2);
        QCOMPARE(limitedEntries.last().toObject().value("message").toString(), QStringLiteral("delta"));

        QJsonObject none = actor.executeTool("get_execution_logs", QJsonObject{{"keyword", "no-such-zzz"}});
        QCOMPARE(none.value("total").toInt(), 0);
        QVERIFY(none.value("entries").toArray().isEmpty());
        QCOMPARE(none.value("truncated").toBool(), false);
        Logger::instance().clearLogs();
    }

    void testRunFlowWaitCompletes() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "wait"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));

        QJsonObject result = actor.executeTool("run_flow", QJsonObject());
        QVERIFY2(result.value("success").toBool(),
                 qPrintable(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))));
        QVERIFY(result.value("elapsedMs").toInt() >= 0);
        QCOMPARE(result.value("failedModule").toString(), QString());
        QCOMPARE(result.value("errorMessage").toString(), QString());
        QCOMPARE(result.value("waitTimedOut").toBool(), false);
        QVERIFY(!RunEngine::instance().isBusy());

        // wait=false 保持旧语义
        QJsonObject async = actor.executeTool("run_flow", QJsonObject{{"mode", "once"}, {"wait", false}});
        QCOMPARE(async.value("status").toString(), QStringLiteral("started"));
        QCOMPARE(async.value("async").toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 5000);

        // cycle 没有"完成"概念：wait 被忽略并给出说明
        QJsonObject cycle = actor.executeTool("run_flow", QJsonObject{{"mode", "cycle"}, {"maxCycles", 10}});
        QCOMPARE(cycle.value("status").toString(), QStringLiteral("started"));
        QVERIFY(cycle.value("note").toString().contains(QStringLiteral("cycle")));
        RunEngine::instance().stop();
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
    }

    void testRunFlowWaitTimeoutStopsFlow() {
        AgentActor actor;
        BlockingAgentModule blocking(3000);
        QVERIFY(blocking.initialize());
        RunEngine::instance().addModule(&blocking);
        // 后台执行（设计 §8.2）：服务工作者线程执行探针须 worker 亲和内联，
        // 否则探针会阻塞 GUI 事件循环，等待超时无法按时触发。
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("agent_blocking"),
                                                                ExecutionAffinity::Worker);

        QElapsedTimer timer;
        timer.start();
        QJsonObject result = actor.executeTool("run_flow", QJsonObject{{"timeoutMs", 1000}});
        QCOMPARE(result.value("status").toString(), QStringLiteral("timeout"));
        QCOMPARE(result.value("errorCode").toString(), QStringLiteral("RUN_TIMEOUT"));
        QVERIFY(result.value("error").toString().contains(QStringLiteral("wait timeout")));
        QVERIFY(timer.elapsed() >= 900);
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 8000);
    }

    void testTuneNodeAndVerify() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "tune"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);
        QVERIFY(project->setModuleParam(QStringLiteral("grab"), QStringLiteral("gain"), 2.0));

        // 非法值：invalid_parameter + 工程参数零修改
        QJsonObject invalid = actor.executeTool(
            "tune_node_and_verify",
            QJsonObject{{"nodeName", "grab"}, {"parameters", QJsonObject{{"gain", "abc"}}}, {"runMode", "node"}});
        QCOMPARE(invalid.value("success").toBool(), false);
        QCOMPARE(invalid.value("code").toString(), QStringLiteral("invalid_parameter"));
        QCOMPARE(invalid.value("parameter").toString(), QStringLiteral("gain"));
        QCOMPARE(project->moduleById(QStringLiteral("grab"))->params.value(QStringLiteral("gain")).toDouble(), 2.0);

        // runMode=node：零副作用改参核对，不触发运行
        const int runsBefore = RunEngine::instance().totalRuns();
        QJsonObject nodeMode = actor.executeTool(
            "tune_node_and_verify",
            QJsonObject{{"nodeName", "grab"}, {"parameters", QJsonObject{{"gain", 3.0}}}, {"runMode", "node"}});
        QVERIFY2(nodeMode.value("success").toBool(),
                 qPrintable(QString::fromUtf8(QJsonDocument(nodeMode).toJson(QJsonDocument::Compact))));
        QCOMPARE(nodeMode.value("executed").toBool(), false);
        QCOMPARE(nodeMode.value("applied").toInt(), 1);
        QCOMPARE(nodeMode.value("before").toObject().value("gain").toString(), QStringLiteral("2"));
        QCOMPARE(nodeMode.value("after").toObject().value("gain").toString(), QStringLiteral("3"));
        QVERIFY(nodeMode.contains("note"));
        QCOMPARE(RunEngine::instance().totalRuns(), runsBefore);

        // 一次撤销回滚整组（宏）
        QVERIFY(actor.undoStack()->canUndo());
        actor.undoStack()->undo();
        QCOMPARE(project->moduleById(QStringLiteral("grab"))->params.value(QStringLiteral("gain")).toDouble(), 2.0);

        // runMode=flow：改 2 个键 → 执行 → 返回 before/after/outputs/success/elapsedMs
        QJsonObject flow = actor.executeTool(
            "tune_node_and_verify", QJsonObject{{"nodeName", "grab"},
                                                {"parameters", QJsonObject{{"gain", 3.0}, {"grabTimeout", 2000}}},
                                                {"runMode", "flow"}});
        QCOMPARE(flow.value("success").toBool(), true);
        QCOMPARE(flow.value("applied").toInt(), 2);
        QCOMPARE(flow.value("before").toObject().value("gain").toString(), QStringLiteral("2"));
        QCOMPARE(flow.value("after").toObject().value("gain").toString(), QStringLiteral("3"));
        QCOMPARE(flow.value("after").toObject().value("grabTimeout").toString(), QStringLiteral("2000"));
        QVERIFY(flow.value("elapsedMs").toInt() >= 0);
        QVERIFY(!flow.value("outputs").toObject().isEmpty());
        QCOMPARE(flow.value("lastStatus").toString(), QStringLiteral("success"));
        QCOMPARE(flow.value("undoable").toBool(), true);
        QCOMPARE(project->moduleById(QStringLiteral("grab"))->params.value(QStringLiteral("gain")).toDouble(), 3.0);

        // 一次撤销回滚两个键
        actor.undoStack()->undo();
        QCOMPARE(project->moduleById(QStringLiteral("grab"))->params.value(QStringLiteral("gain")).toDouble(), 2.0);
        QVERIFY(!project->moduleById(QStringLiteral("grab"))->params.contains(QStringLiteral("grabTimeout")));
    }

    void testTuneRejectsWhileEngineBusy() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "busy-tune"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        BlockingAgentModule blocking(1500);
        QVERIFY(blocking.initialize());
        RunEngine::instance().addModule(&blocking);
        QVERIFY(RunEngine::instance().runOnceAsync());

        QJsonObject result = actor.executeTool(
            "tune_node_and_verify",
            QJsonObject{{"nodeName", "grab"}, {"parameters", QJsonObject{{"gain", 1.0}}}, {"runMode", "node"}});
        QCOMPARE(result.value("code").toString(), QStringLiteral("engine_busy"));
        RunEngine::instance().stop();
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
    }

    void testCaptureViewportImage() {
        AgentActor actor;
        QCOMPARE(actor.executeTool("capture_viewport_image", QJsonObject()).value("code").toString(),
                 QStringLiteral("no_image"));

        actor.setViewportImageProvider([](QString*) {
            QImage image(100, 80, QImage::Format_RGB32);
            image.fill(Qt::red);
            return image;
        });
        QJsonObject captured = actor.executeTool("capture_viewport_image", QJsonObject());
        QVERIFY2(!captured.contains("error"), qPrintable(captured.value("error").toString()));
        QCOMPARE(captured.value("width").toInt(), 100);
        QCOMPARE(captured.value("height").toInt(), 80);
        QCOMPARE(captured.value("source").toString(), QStringLiteral("viewport"));
        const QString path = captured.value("imagePath").toString();
        QVERIFY(path.startsWith(QDir(AgentToolImage::imageDir()).absolutePath()));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(captured.value("bytes").toDouble() > 0);

        QJsonObject second = actor.executeTool("capture_viewport_image", QJsonObject());
        QVERIFY(second.value("imagePath").toString() != path);

        // 未注入 provider 时回退到上一帧输出
        actor.setViewportImageProvider(AgentActor::ViewportImageProvider());
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "capture"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        QCOMPARE(actor.executeTool("run_flow", QJsonObject()).value("success").toBool(), true);
        QJsonObject fallback = actor.executeTool("capture_viewport_image", QJsonObject());
        QCOMPARE(fallback.value("source").toString(), QStringLiteral("last_output"));
        QVERIFY(QFileInfo::exists(fallback.value("imagePath").toString()));
    }

    void testExportNodeOutput() {
        AgentActor actor;
        QCOMPARE(actor.executeTool("export_node_output", QJsonObject{{"nodeName", "x"}}).value("code").toString(),
                 QStringLiteral("no_project"));

        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "export"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        QCOMPARE(actor.executeTool("export_node_output", QJsonObject{{"nodeName", "nope"}}).value("code").toString(),
                 QStringLiteral("node_not_found"));
        QCOMPARE(actor.executeTool("export_node_output", QJsonObject{{"nodeName", "grab"}}).value("code").toString(),
                 QStringLiteral("no_image_output"));

        // R3（设计 §4.6）：export_node_output 只接受节点运行输出，不得回退视口——
        // 即使宿主注入了 provider，无节点图像输出也必须失败关闭
        actor.setViewportImageProvider([](QString*) {
            QImage image(64, 48, QImage::Format_RGB32);
            image.fill(Qt::blue);
            return image;
        });
        const QJsonObject viaProvider = actor.executeTool("export_node_output", QJsonObject{{"nodeName", "grab"}});
        QCOMPARE(viaProvider.value("code").toString(), QStringLiteral("no_image_output"));
        QVERIFY(viaProvider.contains("error"));
        QVERIFY(!viaProvider.contains("imagePath"));
        actor.setViewportImageProvider(AgentActor::ViewportImageProvider());

        QCOMPARE(actor.executeTool("run_flow", QJsonObject()).value("success").toBool(), true);
        QJsonObject png = actor.executeTool("export_node_output", QJsonObject{{"nodeName", "grab"}});
        QVERIFY2(!png.contains("error"), qPrintable(png.value("error").toString()));
        QCOMPARE(png.value("source").toString(), QStringLiteral("module_output"));
        QCOMPARE(png.value("format").toString(), QStringLiteral("png"));
        QVERIFY(png.value("imagePath").toString().endsWith(QStringLiteral(".png")));
        QVERIFY(png.value("width").toInt() > 0);
        QVERIFY(png.value("height").toInt() > 0);
        QVERIFY(QFileInfo::exists(png.value("imagePath").toString()));

        QJsonObject jpg = actor.executeTool("export_node_output", QJsonObject{{"nodeName", "grab"}, {"format", "jpg"}});
        QVERIFY2(!jpg.contains("error"), qPrintable(jpg.value("error").toString()));
        QVERIFY(jpg.value("imagePath").toString().endsWith(QStringLiteral(".jpg")));
    }

    void testExportNodeOutputRejectsWhileEngineBusy() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "busy-export"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        BlockingAgentModule blocking(1500);
        QVERIFY(blocking.initialize());
        RunEngine::instance().addModule(&blocking);
        QVERIFY(RunEngine::instance().runOnceAsync());

        QJsonObject result = actor.executeTool("export_node_output", QJsonObject{{"nodeName", "grab"}});
        QCOMPARE(result.value("code").toString(), QStringLiteral("engine_busy"));
        RunEngine::instance().stop();
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
    }

    // ---------- L1/L2 遗留收口 ----------

    void testTuneTimeoutReturnsStructuredError() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "tune-timeout"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        AgentRunWaiter::Outcome outcome;
        outcome.timedOut = true;
        AgentRunWaiter::setNextOutcomeForTest(outcome);
        const QJsonObject result = actor.executeTool(
            "tune_node_and_verify",
            QJsonObject{{"nodeName", "grab"}, {"parameters", QJsonObject{{"gain", 3.0}}}, {"runMode", "flow"}});
        AgentRunWaiter::clearOutcomeForTest();
        QCOMPARE(result.value("success").toBool(), false);
        QCOMPARE(result.value("code").toString(), QStringLiteral("timeout"));
        QVERIFY(result.contains("before"));
        QVERIFY(result.contains("after"));
        QCOMPARE(result.value("parametersRolledBack").toBool(), false);
        QVERIFY2(result.value("error").toString().contains(QStringLiteral("did not finish")),
                 qPrintable(result.value("error").toString()));
    }

    void testTuneAbortedReturnsCancelled() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "tune-abort"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        AgentRunWaiter::Outcome outcome;
        outcome.aborted = true;
        AgentRunWaiter::setNextOutcomeForTest(outcome);
        const QJsonObject result = actor.executeTool(
            "tune_node_and_verify",
            QJsonObject{{"nodeName", "grab"}, {"parameters", QJsonObject{{"gain", 3.0}}}, {"runMode", "flow"}});
        AgentRunWaiter::clearOutcomeForTest();
        QCOMPARE(result.value("code").toString(), QStringLiteral("cancelled"));
    }

    void testTuneReentrantReturnsReentrantWait() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "tune-reentrant"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        AgentRunWaiter::Outcome outcome;
        outcome.reentrantRejected = true;
        AgentRunWaiter::setNextOutcomeForTest(outcome);
        const QJsonObject result = actor.executeTool(
            "tune_node_and_verify",
            QJsonObject{{"nodeName", "grab"}, {"parameters", QJsonObject{{"gain", 3.0}}}, {"runMode", "flow"}});
        AgentRunWaiter::clearOutcomeForTest();
        QCOMPARE(result.value("code").toString(), QStringLiteral("reentrant_wait"));
    }

    void testTunePausedAtBreakpoint() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "tune-breakpoint"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        AgentRunWaiter::Outcome outcome;
        outcome.pausedAtBreakpoint = true;
        AgentRunWaiter::setNextOutcomeForTest(outcome);
        const QJsonObject result = actor.executeTool(
            "tune_node_and_verify",
            QJsonObject{{"nodeName", "grab"}, {"parameters", QJsonObject{{"gain", 3.0}}}, {"runMode", "flow"}});
        AgentRunWaiter::clearOutcomeForTest();
        QCOMPARE(result.value("code").toString(), QStringLiteral("paused_at_breakpoint"));
    }

    void testTuneRunFailedCarriesFailedModule() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "tune-failed"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        AgentRunWaiter::Outcome outcome;
        outcome.finished = true;
        outcome.result.success = false;
        outcome.result.errorMessage = QStringLiteral("module exploded");
        outcome.failedModule = QStringLiteral("MeasureCircle_1");
        AgentRunWaiter::setNextOutcomeForTest(outcome);
        const QJsonObject result = actor.executeTool(
            "tune_node_and_verify",
            QJsonObject{{"nodeName", "grab"}, {"parameters", QJsonObject{{"gain", 3.0}}}, {"runMode", "flow"}});
        AgentRunWaiter::clearOutcomeForTest();
        QCOMPARE(result.value("code").toString(), QStringLiteral("run_failed"));
        QCOMPARE(result.value("failedModule").toString(), QStringLiteral("MeasureCircle_1"));
    }

    void testGetNodeRuntimeOutputsUnsupportedPorts() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "unsupported-ports"}}).contains("error"));
        QVERIFY(addDemoGrab(actor, QStringLiteral("grab")));
        const QJsonObject run = actor.executeTool("run_flow", QJsonObject{{"wait", true}});
        QVERIFY2(!run.contains("error"), qPrintable(run.value("error").toString()));

        UnsupportedPortModule module;
        QVERIFY(module.initialize());
        RunEngine::instance().setNodePortOutputsForTest(QStringLiteral("grab"), module.customOutputs());
        const QJsonObject result = actor.executeTool("get_node_runtime_outputs", QJsonObject{{"nodeName", "grab"}});
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QVERIFY(result.contains("unsupportedPorts"));
        const QJsonArray unsupported = result.value("unsupportedPorts").toArray();
        QVERIFY2(unsupported.contains(QJsonValue(QStringLiteral("rect"))),
                 qPrintable(QString::fromUtf8(QJsonDocument(unsupported).toJson(QJsonDocument::Compact))));
    }

    // 后台执行（设计 §8.2）：run_flow 的执行落在 FlowExecutionService 工作线程。
    void testRunFlowUsesServiceWorkerThread() {
        AgentActor actor;
        CountingAgentModule probe(QStringLiteral("AgentWorker"));
        QVERIFY(probe.initialize());
        RunEngine::instance().addModule(&probe);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("AgentWorker"),
                                                                ExecutionAffinity::Worker);

        const QJsonObject run = actor.executeTool("run_flow", QJsonObject{{"mode", "once"}, {"wait", true}});
        QVERIFY2(run.value("success").toBool(),
                 qPrintable(QString::fromUtf8(QJsonDocument(run).toJson(QJsonDocument::Compact))));
        QCOMPARE(probe.runs.load(), 1);
        QVERIFY2(probe.executedOn.load() != QThread::currentThreadId(),
                 "run_flow must execute on the service worker thread");
        QVERIFY(!RunEngine::instance().isBusy());
        QCOMPARE(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle);
    }

    // 后台执行（设计 §8.2）：pause_flow/resume_flow 走 Service，A→B→C 各执行一次。
    void testPauseResumeFlowThroughService() {
        AgentActor actor;
        BlockingAgentModule blocking(800);
        QVERIFY(blocking.initialize());
        CountingAgentModule second(QStringLiteral("AgentB"));
        CountingAgentModule third(QStringLiteral("AgentC"));
        QVERIFY(second.initialize());
        QVERIFY(third.initialize());
        RunEngine& engine = RunEngine::instance();
        engine.addModule(&blocking);
        engine.addModule(&second);
        engine.addModule(&third);
        // 后台服务工作者线程执行探针须 worker 亲和内联（探针默认 gui）。
        engine.setModuleExecutionAffinityForTest(QStringLiteral("agent_blocking"), ExecutionAffinity::Worker);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("AgentB"), ExecutionAffinity::Worker);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("AgentC"), ExecutionAffinity::Worker);

        const QJsonObject started = actor.executeTool("run_flow", QJsonObject{{"mode", "once"}, {"wait", false}});
        QCOMPARE(started.value("status").toString(), QStringLiteral("started"));
        QVERIFY2(blocking.entered.tryAcquire(1, 5000), "blocking module must enter execution");
        const QJsonObject paused = actor.executeTool("pause_flow", QJsonObject());
        QCOMPARE(paused.value("status").toString(), QStringLiteral("paused"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Paused, 5000);
        QCOMPARE(second.runs.load(), 0);
        QCOMPARE(third.runs.load(), 0);
        // Paused 只接受 continueRun/stop：再次启动必须被拒绝。
        QVERIFY(actor.executeTool("run_flow", QJsonObject{{"mode", "once"}, {"wait", false}}).contains("error"));

        const QJsonObject resumed = actor.executeTool("resume_flow", QJsonObject());
        QCOMPARE(resumed.value("status").toString(), QStringLiteral("resumed"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 5000);
        QCOMPARE(second.runs.load(), 1);
        QCOMPARE(third.runs.load(), 1);
        QVERIFY(!engine.isBusy());
    }

    // 后台执行（设计 §8.2）：stop_flow 对 Service job 走 Service::stop()，立即取消并回到 Idle。
    void testStopFlowStopsServiceJob() {
        AgentActor actor;
        BlockingAgentModule blocking(3000);
        QVERIFY(blocking.initialize());
        RunEngine::instance().addModule(&blocking);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("agent_blocking"),
                                                                ExecutionAffinity::Worker);

        const QJsonObject started = actor.executeTool("run_flow", QJsonObject{{"mode", "once"}, {"wait", false}});
        QCOMPARE(started.value("status").toString(), QStringLiteral("started"));
        QVERIFY(blocking.entered.tryAcquire(1, 5000));
        const QJsonObject stopped = actor.executeTool("stop_flow", QJsonObject());
        QCOMPARE(stopped.value("status").toString(), QStringLiteral("stopping"));
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
    }

    // t11 F4：真实取消端到端——run_flow(wait=true) 等待中由定时器触发 abortRunningTool，
    // 返回 cancelled 且服务回 Idle、引擎不再 busy（不再只覆盖注入式 waiter 分支）。
    void testAbortRunningToolCancelsWaitedRun() {
        AgentActor actor;
        BlockingAgentModule blocking(3000);
        QVERIFY(blocking.initialize());
        RunEngine::instance().addModule(&blocking);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("agent_blocking"),
                                                                ExecutionAffinity::Worker);

        // 验收建议的定时器触发路径；等待本身由 AgentRunWaiter 的事件循环与信号屏障驱动。
        QTimer::singleShot(100, this, [&actor]() { actor.abortRunningTool(); });
        const QJsonObject run =
            actor.executeTool("run_flow", QJsonObject{{"mode", "once"}, {"wait", true}, {"timeoutMs", 30000}});
        QCOMPARE(run.value("status").toString(), QStringLiteral("cancelled"));
        QCOMPARE(run.value("errorCode").toString(), QStringLiteral("USER_CANCELLED"));
        QCOMPARE(run.value("success").toBool(), false);
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
    }

    // 兼容语义：cycle 立即 returned started 且 wait 说明保留；后台驱动由 Service 负责。
    void testRunFlowCycleThroughService() {
        AgentActor actor;
        CountingAgentModule probe(QStringLiteral("AgentCycle"));
        QVERIFY(probe.initialize());
        RunEngine::instance().addModule(&probe);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("AgentCycle"),
                                                                ExecutionAffinity::Worker);

        const QJsonObject cycle = actor.executeTool("run_flow", QJsonObject{{"mode", "cycle"}, {"maxCycles", 10}});
        QCOMPARE(cycle.value("status").toString(), QStringLiteral("started"));
        QVERIFY(cycle.value("note").toString().contains(QStringLiteral("cycle mode ignores wait")));
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() > 0, 5000);
        QCOMPARE(FlowExecutionService::instance().currentJob(), FlowExecutionService::JobType::Cycle);
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Idle, 5000);
        QCOMPARE(probe.runs.load(), 10);
        QVERIFY(!RunEngine::instance().isBusy());
    }
};

QTEST_MAIN(TestAgentActor)
#include "test_agentactor.moc"
