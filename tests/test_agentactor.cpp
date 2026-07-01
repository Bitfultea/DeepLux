#include <QtTest/QtTest>
#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "manager/ProjectManager.h"
#include "core/model/Project.h"

using namespace DeepLux;

class TestAgentActor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        ToolSchema::instance().registerDefaultTools();
    }

    void init()
    {
        ProjectManager::instance().closeProject();
    }

    void cleanup()
    {
        ProjectManager::instance().closeProject();
    }

    void testUndoStackExists()
    {
        AgentActor actor;
        QVERIFY(actor.undoStack() != nullptr);
    }

    void testUnknownTool()
    {
        AgentActor actor;
        QJsonObject result = actor.executeTool("unknown_tool", QJsonObject());
        QVERIFY(result.contains("error"));
    }

    void testGetAvailablePlugins()
    {
        AgentActor actor;
        QJsonObject result = actor.executeTool("get_available_plugins", QJsonObject());
        QVERIFY(result.contains("plugins"));
    }

    void testGetFlowStateNoProject()
    {
        AgentActor actor;
        QJsonObject result = actor.executeTool("get_flow_state", QJsonObject());
        QVERIFY(result.contains("error"));
    }

    void testRequiredAndEnumParametersAreValidatedBeforeExecution()
    {
        AgentActor actor;
        QJsonObject missingName = actor.executeTool("create_project", QJsonObject());
        QVERIFY(missingName.contains("error"));
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);

        QJsonObject invalidMode = actor.executeTool("run_flow", QJsonObject{{"mode", "forever"}});
        QVERIFY(invalidMode.contains("error"));
    }

    void testConnectModulesAddsSingleConnection()
    {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        project->addModule(ModuleInstance{"a", "GrabImage", "A"});
        project->addModule(ModuleInstance{"b", "SaveImage", "B"});

        QJsonObject result = actor.executeTool("connect_modules", QJsonObject{{"fromId", "a"}, {"toId", "b"}});
        QCOMPARE(result["status"].toString(), QString("connected"));
        QCOMPARE(project->connections().size(), 1);
    }

    void testBatchExecutionStopsOnFirstError()
    {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        QList<QPair<QString, QJsonObject>> tools;
        tools.append(qMakePair(QString("add_module"), QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "grab_1"}}));
        tools.append(qMakePair(QString("set_param"), QJsonObject{{"instanceId", "missing"}, {"key", "threshold"}, {"value", 1}}));
        tools.append(qMakePair(QString("add_module"), QJsonObject{{"plugin", "SaveImage"}, {"instanceName", "save_1"}}));

        QJsonObject result = actor.executeTools(tools, "Partial failure");
        QCOMPARE(result["status"].toString(), QString("partial_failed"));
        QCOMPARE(result["executed"].toInt(), 1);
        QCOMPARE(project->modules().size(), 1);
    }
};

QTEST_MAIN(TestAgentActor)
#include "test_agentactor.moc"
