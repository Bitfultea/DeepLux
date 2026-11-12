#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "manager/ProjectManager.h"
#include "model/Project.h"

#include <QtTest/QtTest>

using namespace DeepLux;

class TestAgentUndo : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
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

    void testBatchRenameAndNoteMacroUndo() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("batch_macro_instance");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("Before");
        instance.note = QStringLiteral("original note");
        project->addModule(instance);

        const QList<QPair<QString, QJsonObject>> tools{
            qMakePair(QString("rename_module"), QJsonObject{{"instanceId", instance.id}, {"name", "Renamed"}}),
            qMakePair(QString("set_module_note"), QJsonObject{{"instanceId", instance.id}, {"note", "updated note"}}),
        };
        const int initialCount = actor.undoStack()->count();
        const int initialIndex = actor.undoStack()->index();
        const QJsonObject result = actor.executeTools(tools, "Test macro");
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(actor.undoStack()->count(), initialCount + 2);
        QCOMPARE(actor.undoStack()->index(), initialIndex + 2);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Renamed"));
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("updated note"));
        actor.undoStack()->undo();
        QCOMPARE(actor.undoStack()->index(), initialIndex + 1);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Renamed"));
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("original note"));
        actor.undoStack()->undo();
        QCOMPARE(actor.undoStack()->index(), initialIndex);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Before"));
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("original note"));
    }

    // B2：工程切换会终止旧批次，后续编辑不得落入旧撤销宏。
    void testProjectSwitchStopsBatchBeforeMacroEdits() {
        AgentActor actor;
        const QList<QPair<QString, QJsonObject>> tools{
            qMakePair(QString("create_project"), QJsonObject{{"name", "BatchSwitch"}}),
            qMakePair(QString("add_module"), QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "stale"}}),
        };
        const QJsonObject result = actor.executeTools(tools, "Test switch");
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PROJECT_SWITCHED"));
        QCOMPARE(result.value(QStringLiteral("failedTool")).toString(), QStringLiteral("create_project"));
        QCOMPARE(result.value(QStringLiteral("executed")).toInt(), 1);
        const QJsonArray results = result.value(QStringLiteral("results")).toArray();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.at(1)
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
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

        const QList<QPair<QString, QJsonObject>> tools{
            qMakePair(QString("remove_module"), QJsonObject{{"instanceId", "macro_a"}}),
            qMakePair(QString("remove_module"), QJsonObject{{"instanceId", "macro_b"}}),
        };
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
