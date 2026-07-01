#include <QtTest/QtTest>
#include "core/agent/AgentController.h"
#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "manager/ProjectManager.h"

using namespace DeepLux;

class TestAgentPermissions : public QObject
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

    void testObserverAllowsReadOnlyAndBlocksWriteTools()
    {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        QJsonObject readResult = AgentController::instance().handleToolCall("get_available_plugins", QJsonObject());
        QVERIFY(!readResult.contains("error"));

        QJsonObject writeResult = AgentController::instance().handleToolCall(
            "add_module", QJsonObject{{"plugin", "GrabImage"}});
        QVERIFY(writeResult.contains("error"));
        QVERIFY(writeResult["error"].toString().contains("Observer"));
    }

    void testAdvisorRequiresConfirmationForWriteTools()
    {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QSignalSpy confirmSpy(&AgentController::instance(), &AgentController::toolsPendingConfirmation);

        QJsonObject readResult = AgentController::instance().handleToolCall("get_available_plugins", QJsonObject());
        QVERIFY(!readResult.contains("error"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);

        QJsonObject writeResult = AgentController::instance().handleToolCall(
            "add_module", QJsonObject{{"plugin", "GrabImage"}});
        QCOMPARE(writeResult["status"].toString(), QString("pending_confirmation"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
        QCOMPARE(confirmSpy.count(), 1);
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);
    }

    void testAutopilotExecutesSafeWriteButConfirmsDangerousTools()
    {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QJsonObject safeResult = AgentController::instance().handleToolCall(
            "create_project", QJsonObject{{"name", "AgentSafe"}});
        QCOMPARE(safeResult["status"].toString(), QString("created"));
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);

        QJsonObject dangerousResult = AgentController::instance().handleToolCall(
            "remove_module", QJsonObject{{"instanceId", "missing"}});
        QCOMPARE(dangerousResult["status"].toString(), QString("pending_confirmation"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
    }

    void testPermissionSignal()
    {
        QSignalSpy spy(&AgentController::instance(), &AgentController::permissionLevelChanged);
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_MAIN(TestAgentPermissions)
#include "test_agentpermissions.moc"
