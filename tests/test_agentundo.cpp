#include <QtTest/QtTest>
#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/ToolSchema.h"
#include "manager/ProjectManager.h"

using namespace DeepLux;

class TestAgentUndo : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        ToolSchema::instance().registerDefaultTools();
    }

    void init()
    {
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void cleanup()
    {
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void testUndoStackExists()
    {
        AgentActor actor;
        QVERIFY(actor.undoStack() != nullptr);
        QCOMPARE(actor.undoStack()->count(), 0);
    }

    void testCreateProjectUndo()
    {
        AgentActor actor;
        int undoCountBefore = actor.undoStack()->count();
        actor.executeTool("create_project", QJsonObject{{"name", "UndoTest"}});
        QCOMPARE(actor.undoStack()->count(), undoCountBefore + 1);

        actor.undoStack()->undo();
        // Project should be closed after undo
    }

    void testBatchExecutionMacro()
    {
        AgentActor actor;
        QList<QPair<QString, QJsonObject>> tools;
        tools.append(qMakePair(QString("create_project"), QJsonObject{{"name", "BatchTest"}}));
        tools.append(qMakePair(QString("add_module"), QJsonObject{{"plugin", "GrabImage"}}));

        actor.executeTools(tools, "Test macro");
        QVERIFY(actor.undoStack()->count() > 0);
    }

    void testControllerUndoLastAgentAction()
    {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QJsonObject result = AgentController::instance().handleToolCall(
            "create_project", QJsonObject{{"name", "UndoViaController"}});
        QCOMPARE(result["status"].toString(), QString("created"));
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);

        QVERIFY(AgentController::instance().undoLastAgentAction());
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);
    }
};

QTEST_MAIN(TestAgentUndo)
#include "test_agentundo.moc"
