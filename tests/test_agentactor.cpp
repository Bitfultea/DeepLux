#define DEEPLUX_BUILD_TESTS 1
#include "core/agent/AgentActor.h"
#include "core/agent/AgentRunWaiter.h"
#include "core/agent/AgentToolImage.h"
#include "core/agent/ToolSchema.h"
#include "core/base/ModuleBase.h"
#include "core/common/Logger.h"
#include "core/engine/RunEngine.h"
#include "core/model/Project.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QRect>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <atomic>

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

private:
    int m_sleepMs = 900;
};
} // namespace

class TestAgentActor : public QObject {
    Q_OBJECT

private:
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
        ToolSchema::instance().registerDefaultTools();
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
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasureCircle")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasurementInput")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("FitCircle")));
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void init() {
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void cleanup() {
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testUndoStackExists() {
        AgentActor actor;
        QVERIFY(actor.undoStack() != nullptr);
    }

    void testUnknownTool() {
        AgentActor actor;
        QJsonObject result = actor.executeTool("unknown_tool", QJsonObject());
        QVERIFY(result.contains("error"));
    }

    void testGetAvailablePlugins() {
        AgentActor actor;
        QJsonObject result = actor.executeTool("get_available_plugins", QJsonObject());
        QVERIFY(result.contains("plugins"));
    }

    // 知识库：read_documentation 同时检索内置（qrc）与用户目录文档
    void testReadDocumentationSearchesBuiltInAndUserKnowledge() {
        QVERIFY(AgentActor::knowledgeDocumentPaths().contains(QStringLiteral(":/agent/knowledge/DEEPLUX.md")));

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

        AgentActor actor;
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

    void testGetFlowStateNoProject() {
        AgentActor actor;
        QJsonObject result = actor.executeTool("get_flow_state", QJsonObject());
        QVERIFY(result.contains("error"));
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
        QCOMPARE(result["failedCode"].toString(), QStringLiteral("tool_error"));
        QCOMPARE(project->modules().size(), 1);

        // B1：未执行调用必须补齐跳过结果，且每个 tool_call 恰好一条结果
        const QJsonArray results = result["results"].toArray();
        QCOMPARE(results.size(), tools.size());
        QCOMPARE(AgentActor::resultSucceeded(results[0].toObject()["result"].toObject()), true);
        QCOMPARE(AgentActor::resultSucceeded(results[1].toObject()["result"].toObject()), false);
        const QJsonObject skipped = results[2].toObject()["result"].toObject();
        QCOMPARE(skipped["skipped"].toBool(), true);
        QCOMPARE(skipped["code"].toString(), QStringLiteral("skipped_after_failure"));
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

        int gateCalls = 0;
        const auto gate = [&gateCalls](int index, const QString&, const QJsonObject&) -> QString {
            ++gateCalls;
            return index == 1 ? QStringLiteral("PERMISSION_DENIED") : QString();
        };
        const QJsonObject result = actor.executeTools(tools, "Gate", gate);

        QCOMPARE(result["status"].toString(), QStringLiteral("partial_failed"));
        QCOMPARE(result["executed"].toInt(), 1);
        QCOMPARE(result["failedCode"].toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(gateCalls, 2);
        QCOMPARE(project->modules().size(), 1);
        const QJsonObject blocked = result["results"].toArray()[1].toObject()["result"].toObject();
        QCOMPARE(blocked["blocked"].toBool(), true);
        QCOMPARE(blocked["code"].toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(AgentActor::resultSucceeded(blocked), false);
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
        QCOMPARE(results[1].toObject()["result"].toObject()["code"].toString(), QStringLiteral("invalid_parameter"));
        QCOMPARE(results[0].toObject()["result"].toObject()["code"].toString(),
                 QStringLiteral("skipped_after_failure"));
        QCOMPARE(results[2].toObject()["result"].toObject()["code"].toString(),
                 QStringLiteral("skipped_after_failure"));
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
        QCOMPARE(missing.value("code").toString(), QStringLiteral("node_not_found"));
        QCOMPARE(missing.value("error").toString(), QStringLiteral("Module not found: does_not_exist"));
        QCOMPARE(actor.undoStack()->index(), undoIndexBefore);  // 未压栈
        QCOMPARE(project->modules().size(), moduleCountBefore); // 工程零修改
        QVERIFY(project->moduleById(QStringLiteral("keep")).has_value());

        // 无工程时同样失败关闭（不压栈）
        ProjectManager::instance().closeProject();
        const int undoIndexNoProject = actor.undoStack()->index();
        const QJsonObject noProject = actor.executeTool("remove_module", QJsonObject{{"instanceId", "keep"}});
        QCOMPARE(noProject.value("code").toString(), QStringLiteral("node_not_found"));
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

        const QJsonObject failed = actor.createProject(QJsonObject{{"name", "busy-create"}});
        QCOMPARE(failed.value("error").toString(), QStringLiteral("Failed to create project"));
        QCOMPARE(failed.value("code").toString(), QStringLiteral("project_create_failed"));
        QVERIFY2(failed.contains("message"), "project_create_failed must carry a diagnostic message");

        RunEngine::instance().stop();
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 5000);
    }

    // --- 第一迭代：派发注册表 / 闭环 / 观测工具 ---

    void testSupportedToolsRegistryAndUnknownTool() {
        AgentActor actor;
        const QStringList supported = actor.supportedTools();
        QCOMPARE(supported.size(), 25);

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
        QVERIFY(actor.toolUndoable(QStringLiteral("tune_node_and_verify")));
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
        QCOMPARE(run.value("code").toString(), QStringLiteral("run_failed"));
        QCOMPARE(run.value("failedModule").toString(), QStringLiteral("mc"));
        QVERIFY(!run.value("errorMessage").toString().isEmpty());

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

        // cycle 没有"完成"概念：wait 被忽略并给出说明
        QJsonObject cycle = actor.executeTool("run_flow", QJsonObject{{"mode", "cycle"}});
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

        QElapsedTimer timer;
        timer.start();
        QJsonObject result = actor.executeTool("run_flow", QJsonObject{{"timeoutMs", 1000}});
        QCOMPARE(result.value("code").toString(), QStringLiteral("timeout"));
        QCOMPARE(result.value("waitTimedOut").toBool(), true);
        QVERIFY(result.value("error").toString().contains(QStringLiteral("1000")));
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
        QCOMPARE(nodeMode.value("success").toBool(), true);
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
};

QTEST_MAIN(TestAgentActor)
#include "test_agentactor.moc"
