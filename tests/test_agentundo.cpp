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

    void testBatchExecutionMacro() {
        AgentActor actor;
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("batch_macro_instance");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("Before");
        instance.note = QStringLiteral("original note");
        project->addModule(instance);

        QList<QPair<QString, QJsonObject>> tools;
        tools.append(
            qMakePair(QString("rename_module"), QJsonObject{{"instanceId", instance.id}, {"name", "Renamed"}}));
        tools.append(
            qMakePair(QString("set_module_note"), QJsonObject{{"instanceId", instance.id}, {"note", "updated note"}}));

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
