#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "core/model/Project.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace DeepLux;

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
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasureCircle")));
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void init() {
        ProjectManager::instance().closeProject();
    }

    void cleanup() {
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
        second.moduleId = "SaveImage";
        second.name = "B";
        project->addModule(second);

        QJsonObject result = actor.executeTool("connect_modules", QJsonObject{{"fromId", "a"}, {"toId", "b"}});
        QCOMPARE(result["status"].toString(), QString("connected"));
        QCOMPARE(project->connections().size(), 1);
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
