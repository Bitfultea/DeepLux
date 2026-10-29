#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/ToolSchema.h"
#include "core/model/Project.h"
#include "manager/ProjectManager.h"

#include <QtTest/QtTest>

using namespace DeepLux;

class TestAgentUndo : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
    }

    void init() {
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void cleanup() {
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void testUndoStackExists() {
        AgentActor actor;
        QVERIFY(actor.undoStack() != nullptr);
        QCOMPARE(actor.undoStack()->count(), 0);
    }

    void testCreateProjectClearsUndoHistory() {
        AgentActor actor;
        actor.executeTool("create_project", QJsonObject{{"name", "UndoTest"}});
        QCOMPARE(actor.undoStack()->count(), 0);
        actor.undoStack()->undo();
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);
    }

    // B2：create/open 成功会保守终止旧批次；宏只覆盖同工程内的连续编辑
    void testProjectSwitchStopsBatchBeforeMacroEdits() {
        AgentActor actor;
        QList<QPair<QString, QJsonObject>> tools;
        tools.append(qMakePair(QString("create_project"), QJsonObject{{"name", "BatchSwitch"}}));
        tools.append(qMakePair(QString("add_module"), QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "stale"}}));

        const QJsonObject result = actor.executeTools(tools, "Test switch");
        QCOMPARE(result.value(QStringLiteral("failedCode")).toString(), QStringLiteral("project_switched"));
        QCOMPARE(actor.undoStack()->count(), 0);
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);
        QCOMPARE(ProjectManager::instance().currentProject()->modules().size(), 0);
    }

    void testBatchExecutionMacro() {
        AgentActor actor;
        QVERIFY(!actor.executeTool("create_project", QJsonObject{{"name", "BatchTest"}}).contains("error"));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);
        ModuleInstance first;
        first.id = QStringLiteral("macro_a");
        first.moduleId = QStringLiteral("GrabImage");
        project->addModule(first);
        ModuleInstance second;
        second.id = QStringLiteral("macro_b");
        second.moduleId = QStringLiteral("GrabImage");
        project->addModule(second);

        QList<QPair<QString, QJsonObject>> tools;
        tools.append(qMakePair(QString("remove_module"), QJsonObject{{"instanceId", "macro_a"}}));
        tools.append(qMakePair(QString("remove_module"), QJsonObject{{"instanceId", "macro_b"}}));

        const QJsonObject result = actor.executeTools(tools, "Test macro");
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QVERIFY(actor.undoStack()->count() > 0);
    }

    void testControllerUndoLastAgentAction() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QJsonObject result =
            AgentController::instance().handleToolCall("create_project", QJsonObject{{"name", "UndoViaController"}});
        QCOMPARE(result["status"].toString(), QString("created"));
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);

        QVERIFY(!AgentController::instance().undoLastAgentAction());
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);
    }
};

QTEST_MAIN(TestAgentUndo)
#include "test_agentundo.moc"
