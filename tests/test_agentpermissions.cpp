#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/ToolSchema.h"
#include "manager/ProjectManager.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestAgentPermissions : public QObject {
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

    void testObserverAllowsReadOnlyAndBlocksWriteTools() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        QJsonObject readResult = AgentController::instance().handleToolCall("get_available_plugins", QJsonObject());
        QVERIFY(!readResult.contains("error"));

        QJsonObject writeResult =
            AgentController::instance().handleToolCall("add_module", QJsonObject{{"plugin", "GrabImage"}});
        QVERIFY(writeResult.contains("error"));
        QVERIFY(writeResult["error"].toString().contains("Observer"));
    }

    void testAdvisorRequiresConfirmationForWriteTools() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QSignalSpy confirmSpy(&AgentController::instance(), &AgentController::toolsPendingConfirmation);

        QJsonObject readResult = AgentController::instance().handleToolCall("get_available_plugins", QJsonObject());
        QVERIFY(!readResult.contains("error"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);

        QJsonObject writeResult =
            AgentController::instance().handleToolCall("add_module", QJsonObject{{"plugin", "GrabImage"}});
        QCOMPARE(writeResult["status"].toString(), QString("pending_confirmation"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
        QCOMPARE(confirmSpy.count(), 1);
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);
    }

    void testAutopilotExecutesSafeWriteButConfirmsDangerousTools() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QJsonObject safeResult =
            AgentController::instance().handleToolCall("create_project", QJsonObject{{"name", "AgentSafe"}});
        QCOMPARE(safeResult["status"].toString(), QString("created"));
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);

        QJsonObject dangerousResult =
            AgentController::instance().handleToolCall("remove_module", QJsonObject{{"instanceId", "missing"}});
        QCOMPARE(dangerousResult["status"].toString(), QString("pending_confirmation"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
    }

    void testPermissionSignal() {
        QSignalSpy spy(&AgentController::instance(), &AgentController::permissionLevelChanged);
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(spy.count(), 1);
    }

    // SAM 批3：sam_predict 非 readOnly（最小权限：触发外部推理不属于观察应用状态，
    // Observer 拒绝、Advisor 需确认、Autopilot 直接执行）；
    // sam_annotate dangerous（写会话文件，Autopilot 也需确认且不落盘）
    void testSamToolsPermissionGating() {
        const QJsonObject predictParams{{"imagePath", "/nonexistent_sam_gate.png"},
                                        {"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}}};

        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        QJsonObject denied = AgentController::instance().handleToolCall("sam_predict", predictParams);
        QVERIFY(denied["error"].toString().contains("Observer"));

        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QJsonObject advisorPending = AgentController::instance().handleToolCall("sam_predict", predictParams);
        QCOMPARE(advisorPending["status"].toString(), QString("pending_confirmation"));
        AgentController::instance().rejectPendingTools();
        AgentController::instance().clearConversation();

        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        // 非 dangerous → 直接执行：图像不存在失败关闭（error 返回而非待确认，且不发网络请求）
        QJsonObject executed = AgentController::instance().handleToolCall("sam_predict", predictParams);
        QVERIFY(executed.value("status").toString() != QLatin1String("pending_confirmation"));
        QVERIFY(executed.contains("error"));
        QVERIFY(executed["error"].toString().contains("不存在"));

        // dangerous → Autopilot 也必须待确认，且确认前不写任何文件
        QJsonObject annotatePending = AgentController::instance().handleToolCall(
            "sam_annotate",
            QJsonObject{{"imagePath", "/nonexistent_sam_gate.png"},
                        {"sessionPath", "/tmp/deeplux_never_written_sam_gate.json"},
                        {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}}}}}});
        QCOMPARE(annotatePending["status"].toString(), QString("pending_confirmation"));
        QVERIFY(!QFileInfo::exists(QStringLiteral("/tmp/deeplux_never_written_sam_gate.json")));
        AgentController::instance().rejectPendingTools();
    }
};

QTEST_MAIN(TestAgentPermissions)
#include "test_agentpermissions.moc"
