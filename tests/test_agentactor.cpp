#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "core/base/ModuleBase.h"
#include "core/engine/RunEngine.h"
#include "core/model/Project.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <atomic>

using namespace DeepLux;

namespace {
class BlockingAgentModule final : public ModuleBase {
public:
    BlockingAgentModule() {
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
        while (!isCancellationRequested() && timer.elapsed() < 900)
            QThread::msleep(5);
        output = input;
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

public:
    std::atomic_bool m_entered{false};
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
        QCOMPARE(project->modules().size(), 1);
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

        QJsonObject result = actor.executeTool("run_flow", QJsonObject{{"mode", "once"}});
        QCOMPARE(result.value("status").toString(), QStringLiteral("started"));
        QVERIFY(RunEngine::instance().isBusy());
        QVERIFY(actor.executeTool("run_flow", QJsonObject{{"mode", "once"}}).contains("error"));
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
};

QTEST_MAIN(TestAgentActor)
#include "test_agentactor.moc"
