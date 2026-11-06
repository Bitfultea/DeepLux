#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "core/common/VarModel.h"
#include "core/communication/CommunicationManager.h"
#include "core/device/CameraManager.h"
#include "core/model/Annotation.h"
#include "core/model/Project.h"
#include "manager/GlobalVarManager.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestAgentPermissions : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
        UserManager::instance().login(QStringLiteral("developer"), QString());
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
    }

    void init() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void cleanup() {
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
        UserManager::instance().login(QStringLiteral("developer"), QString());
    }

    void testObserverAllowsReadOnlyAndBlocksWriteTools() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        QJsonObject readResult = AgentController::instance().handleToolCall("get_available_plugins", QJsonObject());
        QVERIFY(!readResult.contains("error"));
        QJsonObject cameraResult = AgentController::instance().handleToolCall("list_cameras", QJsonObject());
        QVERIFY(!cameraResult.contains("error"));
        const QJsonObject discoveryTargets =
            AgentController::instance().handleToolCall("get_camera_discovery_targets", QJsonObject());
        QCOMPARE(discoveryTargets.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
        const QJsonObject discoveryDenied = AgentController::instance().handleToolCall(
            "discover_cameras",
            {{"registryGeneration", "0"}, {"pluginIds", QJsonArray{}}, {"unsupportedPluginIds", QJsonArray{}}});
        QCOMPARE(discoveryDenied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        const QJsonObject discoveryResult =
            AgentController::instance().handleToolCall("get_camera_discovery_operation", {{"requestId", "not-an-id"}});
        QCOMPARE(discoveryResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        CommunicationConfig communication;
        communication.id = QStringLiteral("observer-communication");
        communication.name = QStringLiteral("Observer communication");
        communication.type = CommunicationType::SerialPort;
        communication.portName = QStringLiteral("unused");
        QVERIFY(CommunicationManager::instance().addOrUpdateConfig(communication));
        QJsonObject communicationResult =
            AgentController::instance().handleToolCall("list_communications", QJsonObject());
        QVERIFY(!communicationResult.contains("error"));
        QVERIFY(CommunicationManager::instance().removeConfig(communication.id));
        QJsonObject cameraDetails = AgentController::instance().handleToolCall(
            "get_camera_details", QJsonObject{{"deviceId", "observer-camera-not-found"}});
        QCOMPARE(cameraDetails.value("errorCode").toString(), QString("CAMERA_NOT_FOUND"));
        const QJsonObject operation =
            AgentController::instance().handleToolCall("get_camera_operation", QJsonObject{{"requestId", "invalid"}});
        QCOMPARE(operation.value("errorCode").toString(), QString("INVALID_PARAM"));

        QJsonObject writeResult =
            AgentController::instance().handleToolCall("add_module", QJsonObject{{"plugin", "GrabImage"}});
        QVERIFY(writeResult.contains("error"));
        QVERIFY(writeResult["error"].toString().contains("Observer"));
    }

    void testLoginDialogNavigationIsReadOnlyAtEveryPermissionLevel() {
        AgentController& controller = AgentController::instance();
        const QList<AgentController::PermissionLevel> levels = {AgentController::PermissionLevel::Observer,
                                                                AgentController::PermissionLevel::Advisor,
                                                                AgentController::PermissionLevel::Autopilot};
        for (const AgentController::PermissionLevel level : levels) {
            controller.setPermissionLevel(level);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_user_login_dialog")),
                     AgentController::ToolGate::Allow);
        }
    }

    void testAgentSettingsNavigationIsReadOnlyAtEveryPermissionLevel() {
        AgentController& controller = AgentController::instance();
        const QList<AgentController::PermissionLevel> levels = {AgentController::PermissionLevel::Observer,
                                                                AgentController::PermissionLevel::Advisor,
                                                                AgentController::PermissionLevel::Autopilot};
        for (const AgentController::PermissionLevel level : levels) {
            controller.setPermissionLevel(level);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_agent_settings_dialog")),
                     AgentController::ToolGate::Allow);
        }
    }

    void testAboutDialogNavigationIsReadOnlyAtEveryPermissionLevel() {
        AgentController& controller = AgentController::instance();
        const QList<AgentController::PermissionLevel> levels = {AgentController::PermissionLevel::Observer,
                                                                AgentController::PermissionLevel::Advisor,
                                                                AgentController::PermissionLevel::Autopilot};
        for (const AgentController::PermissionLevel level : levels) {
            controller.setPermissionLevel(level);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_about_dialog")),
                     AgentController::ToolGate::Allow);
        }
    }

    void testKnowledgeBaseDialogNavigationIsReadOnlyAtEveryPermissionLevel() {
        AgentController& controller = AgentController::instance();
        const QList<AgentController::PermissionLevel> levels = {AgentController::PermissionLevel::Observer,
                                                                AgentController::PermissionLevel::Advisor,
                                                                AgentController::PermissionLevel::Autopilot};
        for (const AgentController::PermissionLevel level : levels) {
            controller.setPermissionLevel(level);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_knowledge_base_dialog")),
                     AgentController::ToolGate::Allow);
        }
    }

    void testSamAnnotatorNavigationIsReadOnlyAtEveryPermissionLevel() {
        AgentController& controller = AgentController::instance();
        const QList<AgentController::PermissionLevel> levels = {AgentController::PermissionLevel::Observer,
                                                                AgentController::PermissionLevel::Advisor,
                                                                AgentController::PermissionLevel::Autopilot};
        for (const AgentController::PermissionLevel level : levels) {
            controller.setPermissionLevel(level);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_sam_annotator_dialog")),
                     AgentController::ToolGate::Allow);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_sam_image_picker_for_user")),
                     AgentController::ToolGate::Allow);
        }
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

    void testUserPermissionsGateProjectToolsAndAllowOpenSeparately() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentActor actor;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = directory.filePath(QStringLiteral("permission-project.json"));

        UserManager::instance().login(QStringLiteral("operator"), QString());
        const QJsonObject createDenied = controller.handleToolCall(
            QStringLiteral("create_project"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Denied")}});
        QCOMPARE(createDenied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(createDenied.value(QStringLiteral("status")).toString(), QStringLiteral("denied"));
        QCOMPARE(createDenied.value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("requiredPermission"))
                     .toString(),
                 QStringLiteral("NewSolution"));
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);

        UserManager::instance().login(QStringLiteral("developer"), QString());
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("Saved for permission test")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor.executeTool(QStringLiteral("save_project"), QJsonObject{{QStringLiteral("path"), projectPath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("saved"));
        QVERIFY(QFileInfo::exists(projectPath));
        ProjectManager::instance().closeProject();

        UserManager::instance().login(QStringLiteral("operator"), QString());
        QCOMPARE(actor.executeTool(QStringLiteral("open_project"), QJsonObject{{QStringLiteral("path"), projectPath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("opened"));
        QFile file(projectPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = file.readAll();
        file.close();
        const QJsonObject saveDenied =
            actor.executeTool(QStringLiteral("save_project"), QJsonObject{{QStringLiteral("path"), projectPath}});
        QCOMPARE(saveDenied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(saveDenied.value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("requiredPermission"))
                     .toString(),
                 QStringLiteral("Save"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), originalBytes);
        UserManager::instance().logout();
        const QJsonObject readAllowed = controller.handleToolCall(QStringLiteral("get_available_plugins"), {});
        QVERIFY(!readAllowed.contains(QStringLiteral("error")));
    }

    void testGlobalVariableToolsRequireProductGlobalVarPermission() {
        AgentController& controller = AgentController::instance();
        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        GlobalVarManager& variables = GlobalVarManager::instance();
        variables.clearAllVariables();
        variables.addVariable(
            new VarModel(QStringLiteral("permission_secret"), VarDataType::String, QStringLiteral("SECRET_VALUE")));

        const auto assertDenied = [&](const QString& tool, const QJsonObject& params) {
            const QJsonObject result = controller.handleToolCall(tool, params);
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(result.value(QStringLiteral("retryable")).toBool(), false);
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("denied"));
            QCOMPARE(result.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     QStringLiteral("GlobalVar"));
            QVERIFY(!result.contains(QStringLiteral("SECRET_VALUE")));
            QVERIFY(!result.contains(QStringLiteral("permission_secret")));
        };

        for (const QString& user : {QStringLiteral("administrator"), QStringLiteral("operator")}) {
            QVERIFY(UserManager::instance().login(user, QString()));
            assertDenied(QStringLiteral("list_global_variables"), {});
            assertDenied(QStringLiteral("get_global_variable"), {{QStringLiteral("name"), "permission_secret"}});
            const QJsonObject createDenied =
                actor->executeTool(QStringLiteral("create_global_variable"), {{QStringLiteral("name"), "new_secret"},
                                                                              {QStringLiteral("type"), "string"},
                                                                              {QStringLiteral("value"), "new value"}});
            QCOMPARE(createDenied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            const QJsonObject setDenied = actor->executeTool(
                QStringLiteral("set_global_variable"),
                {{QStringLiteral("name"), "permission_secret"}, {QStringLiteral("value"), "changed"}});
            QCOMPARE(setDenied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            const QJsonObject removeDenied = actor->executeTool(QStringLiteral("remove_global_variable"),
                                                                {{QStringLiteral("name"), "permission_secret"}});
            QCOMPARE(removeDenied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(variables.userVariableSnapshots().size(), 1);
            QCOMPARE(variables.userVariableSnapshot(QStringLiteral("permission_secret")).value.toString(),
                     QStringLiteral("SECRET_VALUE"));
        }

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("GlobalVar")));
        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        QCOMPARE(UserManager::instance().currentUser().type, UserType::Guest);
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("GlobalVar")));
        assertDenied(QStringLiteral("get_global_variable"), {{QStringLiteral("name"), "permission_secret"}});
        QCOMPARE(variables.userVariableSnapshot(QStringLiteral("permission_secret")).value.toString(),
                 QStringLiteral("SECRET_VALUE"));
        UserManager::instance().logout();
        assertDenied(QStringLiteral("list_global_variables"), {});
        assertDenied(QStringLiteral("get_global_variable"), {{QStringLiteral("name"), "permission_secret"}});
        QCOMPARE(variables.userVariableSnapshots().size(), 1);

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        QSignalSpy confirmationResponseSpy(&controller, &AgentController::llmResponseReceived);
        QSignalSpy confirmationAuditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject rolePending = controller.handleToolCall(QStringLiteral("create_global_variable"),
                                                                  {{QStringLiteral("name"), "role_change_secret"},
                                                                   {QStringLiteral("type"), "string"},
                                                                   {QStringLiteral("value"), "must not exist"}});
        QCOMPARE(rolePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!controller.pendingConfirmationId().isEmpty());
        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(confirmationResponseSpy.size(), 1);
        QVERIFY(confirmationResponseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(confirmationAuditSpy.size(), 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(confirmationAuditSpy.last().at(0)).result,
                 QStringLiteral("denied"));
        QVERIFY(!variables.userVariableSnapshot(QStringLiteral("role_change_secret")).exists);

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QJsonObject logoutPending = controller.handleToolCall(QStringLiteral("create_global_variable"),
                                                                    {{QStringLiteral("name"), "logout_secret"},
                                                                     {QStringLiteral("type"), "string"},
                                                                     {QStringLiteral("value"), "must not exist"}});
        QCOMPARE(logoutPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!controller.pendingConfirmationId().isEmpty());
        UserManager::instance().logout();
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(confirmationResponseSpy.size(), 2);
        QVERIFY(confirmationResponseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(confirmationAuditSpy.size(), 2);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(confirmationAuditSpy.last().at(0)).result,
                 QStringLiteral("denied"));
        QVERIFY(!variables.userVariableSnapshot(QStringLiteral("logout_secret")).exists);

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QJsonObject listed = controller.handleToolCall(QStringLiteral("list_global_variables"), {});
        QCOMPARE(listed.value(QStringLiteral("total")).toInt(), 1);
        const QJsonObject read = controller.handleToolCall(QStringLiteral("get_global_variable"),
                                                           {{QStringLiteral("name"), "permission_secret"}});
        QCOMPARE(read.value(QStringLiteral("value")).toString(), QStringLiteral("SECRET_VALUE"));
        const QJsonObject created =
            actor->executeTool(QStringLiteral("create_global_variable"), {{QStringLiteral("name"), "created_secret"},
                                                                          {QStringLiteral("type"), "string"},
                                                                          {QStringLiteral("value"), "created"}});
        QCOMPARE(created.value(QStringLiteral("status")).toString(), QStringLiteral("created"));
        const QJsonObject updated =
            actor->executeTool(QStringLiteral("set_global_variable"),
                               {{QStringLiteral("name"), "created_secret"}, {QStringLiteral("value"), "updated"}});
        QCOMPARE(updated.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
        const QJsonObject removed =
            actor->executeTool(QStringLiteral("remove_global_variable"), {{QStringLiteral("name"), "created_secret"}});
        QCOMPARE(removed.value(QStringLiteral("status")).toString(), QStringLiteral("removed"));
        QVERIFY(!variables.userVariableSnapshot(QStringLiteral("created_secret")).exists);
        variables.clearAllVariables();
    }

    void testCameraToolsRequireCameraAndCameraSetPermissions() {
        AgentController& controller = AgentController::instance();
        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);

        const auto assertDenied = [&controller](const QString& tool, const QJsonObject& params,
                                                const QString& permission) {
            const QJsonObject result = controller.handleToolCall(tool, params);
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(result.value(QStringLiteral("retryable")).toBool(), false);
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("denied"));
            QCOMPARE(result.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     permission);
        };

        for (const QString& user : {QStringLiteral("administrator"), QStringLiteral("operator")}) {
            QVERIFY(UserManager::instance().login(user, QString()));
            assertDenied(QStringLiteral("list_cameras"), {}, QStringLiteral("Camera"));
            assertDenied(QStringLiteral("get_camera_discovery_targets"), {}, QStringLiteral("Camera"));
            assertDenied(QStringLiteral("set_camera_parameter"),
                         {{QStringLiteral("deviceId"), "no-camera"},
                          {QStringLiteral("cameraSessionGeneration"), "1"},
                          {QStringLiteral("serialNumber"), "serial"},
                          {QStringLiteral("parameter"), "gain"},
                          {QStringLiteral("value"), 1.5}},
                         QStringLiteral("CameraSet"));
        }

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("Camera")));
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("CameraSet")));
        const QJsonObject devices = controller.handleToolCall(QStringLiteral("list_cameras"), {});
        QVERIFY(!devices.contains(QStringLiteral("error")));
        QCOMPARE(controller.evaluateToolGate(QStringLiteral("set_camera_parameter"),
                                             {{QStringLiteral("deviceId"), "no-camera"}}),
                 AgentController::ToolGate::RequireConfirmation);

        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        QCOMPARE(UserManager::instance().currentUser().type, UserType::Guest);
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("Camera")));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("CameraSet")));
        assertDenied(QStringLiteral("list_cameras"), {}, QStringLiteral("Camera"));
        assertDenied(QStringLiteral("set_camera_parameter"),
                     {{QStringLiteral("deviceId"), "no-camera"},
                      {QStringLiteral("cameraSessionGeneration"), "1"},
                      {QStringLiteral("serialNumber"), "serial"},
                      {QStringLiteral("parameter"), "gain"},
                      {QStringLiteral("value"), 1.5}},
                     QStringLiteral("CameraSet"));
        QCOMPARE(controller.evaluateToolGate(QStringLiteral("run_flow"), {}),
                 AgentController::ToolGate::RequireConfirmation);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        QSignalSpy confirmationResponseSpy(&controller, &AgentController::llmResponseReceived);
        QSignalSpy confirmationAuditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject pending =
            controller.handleToolCall(QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), "no-camera"}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!controller.pendingConfirmationId().isEmpty());
        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QStringList cameraIdsBefore;
        for (const CameraStatus& camera : CameraManager::instance().knownCameras())
            cameraIdsBefore.append(camera.deviceId);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QStringList cameraIdsAfter;
        for (const CameraStatus& camera : CameraManager::instance().knownCameras())
            cameraIdsAfter.append(camera.deviceId);
        QCOMPARE(cameraIdsAfter, cameraIdsBefore);
        QCOMPARE(confirmationResponseSpy.size(), 1);
        QVERIFY(confirmationResponseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(confirmationAuditSpy.size(), 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(confirmationAuditSpy.last().at(0)).result,
                 QStringLiteral("denied"));
        QCOMPARE(controller.conversationHistorySize(), 0);

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        UserManager::instance().logout();
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("RunOnce")));
        QCOMPARE(actor->userPermissionPreflight(QStringLiteral("run_flow"), {})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        assertDenied(QStringLiteral("list_cameras"), {}, QStringLiteral("Camera"));
        assertDenied(QStringLiteral("set_camera_parameter"),
                     {{QStringLiteral("deviceId"), "no-camera"},
                      {QStringLiteral("cameraSessionGeneration"), "1"},
                      {QStringLiteral("serialNumber"), "serial"},
                      {QStringLiteral("parameter"), "gain"},
                      {QStringLiteral("value"), 1.5}},
                     QStringLiteral("CameraSet"));
    }

    void testPluginLifecycleToolsRequireSystemConfigPermission() {
        AgentController& controller = AgentController::instance();
        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const auto assertDenied = [actor](const QString& tool) {
            const QJsonObject result =
                actor->executeTool(tool, {{QStringLiteral("plugin"), "AgentSchemaCacheFixture"}});
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(result.value(QStringLiteral("retryable")).toBool(), false);
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("denied"));
            QCOMPARE(result.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     QStringLiteral("SystemConfig"));
        };

        for (const QString& user : {QStringLiteral("operator")}) {
            QVERIFY(UserManager::instance().login(user, QString()));
            assertDenied(QStringLiteral("load_module_plugin"));
            assertDenied(QStringLiteral("unload_module_plugin"));
        }

        for (const QString& user : {QStringLiteral("developer"), QStringLiteral("administrator")}) {
            QVERIFY(UserManager::instance().login(user, QString()));
            QVERIFY(UserManager::instance().hasPermission(QStringLiteral("SystemConfig")));
            for (const QString& tool : {QStringLiteral("load_module_plugin"), QStringLiteral("unload_module_plugin")})
                QCOMPARE(controller.evaluateToolGate(tool, {{QStringLiteral("plugin"), "missing-plugin"}}),
                         AgentController::ToolGate::RequireConfirmation);
            QVERIFY(controller.handleToolCall(QStringLiteral("get_available_plugins"), {}).contains("plugins"));
        }

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("SystemConfig")));
        assertDenied(QStringLiteral("load_module_plugin"));
        assertDenied(QStringLiteral("unload_module_plugin"));

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        UserManager::instance().logout();
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("SystemConfig")));
        assertDenied(QStringLiteral("load_module_plugin"));
        assertDenied(QStringLiteral("unload_module_plugin"));
        QVERIFY(actor->executeTool(QStringLiteral("get_available_plugins"), {}).contains("plugins"));
    }

    void testAgentSystemSettingsToolRequiresSystemParamPermission() {
        AgentController& controller = AgentController::instance();
        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        const QJsonObject params;
        for (const QString& user : {QStringLiteral("developer"), QStringLiteral("administrator")}) {
            QVERIFY(UserManager::instance().login(user, QString()));
            QVERIFY(actor->userPermissionPreflight(QStringLiteral("open_system_settings"), params).isEmpty());
        }

        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QCOMPARE(actor->userPermissionPreflight(QStringLiteral("open_system_settings"), params)
                     .value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("requiredPermission"))
                     .toString(),
                 QStringLiteral("SystemParam"));
        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        const QJsonObject guestDenied = actor->userPermissionPreflight(QStringLiteral("open_system_settings"), params);
        QCOMPARE(guestDenied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(guestDenied.value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("requiredPermission"))
                     .toString(),
                 QStringLiteral("SystemParam"));
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QSignalSpy guiActionSpy(&controller, &AgentController::agentActionReceived);
        const QJsonObject guestToolDenied = controller.handleToolCall(QStringLiteral("open_system_settings"), params);
        QCOMPARE(guestToolDenied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(guestToolDenied.value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("requiredPermission"))
                     .toString(),
                 QStringLiteral("SystemParam"));
        QCOMPARE(guiActionSpy.count(), 0);
        UserManager::instance().logout();
        const QJsonObject loggedOutDenied = controller.handleToolCall(QStringLiteral("open_system_settings"), params);
        QCOMPARE(loggedOutDenied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(loggedOutDenied.value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("requiredPermission"))
                     .toString(),
                 QStringLiteral("SystemParam"));
    }

    void testPendingConfirmationRechecksLogoutAndRoleDowngrade() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("module");
        module.moduleId = QStringLiteral("GrabImage");
        module.name = QStringLiteral("Source");
        project->addModule(module);

        const auto requestNote = [&controller](const QString& note) {
            return controller.handleToolCall(
                QStringLiteral("set_module_note"),
                QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("module")}, {QStringLiteral("note"), note}});
        };
        const QJsonObject roleDowngradePending = requestNote(QStringLiteral("role downgrade"));
        QCOMPARE(roleDowngradePending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        const QString roleDowngradeConfirmation = controller.pendingConfirmationId();
        QVERIFY(!roleDowngradeConfirmation.isEmpty());
        UserManager::instance().login(QStringLiteral("operator"), QString());
        const QJsonObject downgradedPermission = AgentActor().userPermissionPreflight(
            QStringLiteral("set_module_note"), QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("module")},
                                                           {QStringLiteral("note"), QStringLiteral("role downgrade")}});
        QCOMPARE(downgradedPermission.value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("requiredPermission"))
                     .toString(),
                 QStringLiteral("Edit"));
        QVERIFY(controller.confirmPendingTools(roleDowngradeConfirmation));
        QCOMPARE(project->moduleById(QStringLiteral("module"))->note, QString());
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);

        UserManager::instance().login(QStringLiteral("developer"), QString());
        const QJsonObject logoutPending = requestNote(QStringLiteral("logout"));
        QCOMPARE(logoutPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString logoutConfirmation = controller.pendingConfirmationId();
        QVERIFY(!logoutConfirmation.isEmpty());
        UserManager::instance().logout();
        QVERIFY(controller.confirmPendingTools(logoutConfirmation));
        QCOMPARE(project->moduleById(QStringLiteral("module"))->note, QString());
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
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

    void testPluginLifecycleToolsRequireConfirmationAndDescribeTarget() {
        PluginManager& plugins = PluginManager::instance();
        QSignalSpy loadedSpy(&plugins, &PluginManager::pluginLoaded);
        QSignalSpy failedSpy(&plugins, &PluginManager::pluginLoadFailed);
        QSignalSpy unloadedSpy(&plugins, &PluginManager::pluginUnloaded);
        AgentController& controller = AgentController::instance();

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject denied = controller.handleToolCall("load_module_plugin", {{"plugin", "missing-module"}});
        QCOMPARE(denied.value("errorCode").toString(), QString("PERMISSION_DENIED"));
        const QJsonObject cameraDenied = controller.handleToolCall("connect_camera", {{"deviceId", "observer-camera"}});
        QCOMPARE(cameraDenied.value("errorCode").toString(), QString("PERMISSION_DENIED"));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject cameraPending =
            controller.handleToolCall("disconnect_camera", {{"deviceId", "confirmed-camera"}});
        QCOMPARE(cameraPending.value("status").toString(), QString("pending_confirmation"));
        const QJsonArray cameraEffects =
            controller.pendingConfirmationPayload().value("context").toObject().value("sideEffects").toArray();
        QCOMPARE(cameraEffects.size(), 1);
        QVERIFY(cameraEffects.first().toString().startsWith(QString("camera_disconnect:confirmed-camera")));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(unloadedSpy.count(), 0);

        const QJsonObject pending = controller.handleToolCall("unload_module_plugin", {{"plugin", "confirmed-target"}});
        QCOMPARE(pending.value("status").toString(), QString("pending_confirmation"));
        const QJsonObject payload = controller.pendingConfirmationPayload();
        QVERIFY(payload.value("context").toObject().value("summary").toString().contains("confirmed-target"));
        const QJsonArray effects = payload.value("context").toObject().value("sideEffects").toArray();
        QCOMPARE(effects.size(), 1);
        QCOMPARE(effects.first().toString(), QString("plugin_unload:confirmed-target"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(unloadedSpy.count(), 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject loadPending =
            controller.handleToolCall("load_module_plugin", {{"plugin", "confirmed-target"}});
        QCOMPARE(loadPending.value("status").toString(), QString("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value("context")
                     .toObject()
                     .value("sideEffects")
                     .toArray()
                     .first()
                     .toString(),
                 QString("plugin_load:confirmed-target"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
    }

    void testPermissionSignal() {
        QSignalSpy spy(&AgentController::instance(), &AgentController::permissionLevelChanged);
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(spy.count(), 1);
    }

    // SAM 批3：sam_predict 会触发外部推理，Observer 拒绝，Advisor/Autopilot 均需确认；
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
        AgentController::instance().rejectPendingTools(AgentController::instance().pendingConfirmationId());
        AgentController::instance().clearConversation();

        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        // Autopilot 也须先确认；拒绝后不执行外部推理。
        const QJsonObject autopilotPending = AgentController::instance().handleToolCall("sam_predict", predictParams);
        QCOMPARE(autopilotPending.value("status").toString(), QLatin1String("pending_confirmation"));
        QVERIFY(AgentController::instance().rejectPendingTools(AgentController::instance().pendingConfirmationId()));
        AgentController::instance().clearConversation();

        // dangerous → Autopilot 也必须待确认，且确认前不写任何文件
        QJsonObject annotatePending = AgentController::instance().handleToolCall(
            "sam_annotate",
            QJsonObject{{"imagePath", "/nonexistent_sam_gate.png"},
                        {"sessionPath", "/tmp/deeplux_never_written_sam_gate.json"},
                        {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}}}}}});
        QCOMPARE(annotatePending["status"].toString(), QString("pending_confirmation"));
        QVERIFY(!QFileInfo::exists(QStringLiteral("/tmp/deeplux_never_written_sam_gate.json")));
        AgentController::instance().rejectPendingTools(AgentController::instance().pendingConfirmationId());
    }

    void testSamAnnotateRoleDowngradeDoesNotCallBackendOrWriteSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString imagePath = dir.filePath(QStringLiteral("sam-source.png"));
        QImage image(12, 10, QImage::Format_RGB32);
        image.fill(QColor(0x31, 0x72, 0xA4));
        QVERIFY(image.save(imagePath, "PNG"));
        QFile imageFile(imagePath);
        QVERIFY(imageFile.open(QIODevice::ReadOnly));
        const QByteArray imageBytes = imageFile.readAll();
        imageFile.close();

        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        QSignalSpy toolSpy(actor, &AgentActor::toolExecuted);
        QSignalSpy responseSpy(&controller, &AgentController::llmResponseReceived);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QString sessionPath = dir.filePath(QStringLiteral("must-not-be-created.deeplux-anno.json"));
        const QJsonObject params{{QStringLiteral("imagePath"), imagePath},
                                 {QStringLiteral("sessionPath"), sessionPath},
                                 {QStringLiteral("serverUrl"), QStringLiteral("http://127.0.0.1:8990")},
                                 {QStringLiteral("timeoutMs"), 1000},
                                 {QStringLiteral("items"),
                                  QJsonArray{QJsonObject{{QStringLiteral("points"), QJsonArray{QJsonArray{2, 3}}}}}}};
        const QJsonObject pending = controller.handleToolCall(QStringLiteral("sam_annotate"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        QVERIFY(!QFileInfo::exists(sessionPath));

        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QCOMPARE(toolSpy.count(), 0); // no sam_annotate execution means no SAM HTTP request
        QVERIFY(!QFileInfo::exists(sessionPath));
        QVERIFY(imageFile.open(QIODevice::ReadOnly));
        QCOMPARE(imageFile.readAll(), imageBytes);
        QCOMPARE(responseSpy.size(), 1);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.size(), 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
        QCOMPARE(controller.conversationHistorySize(), 0);
    }

    void testSamPredictGuestRevocationDoesNotCallBackend() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString imagePath = dir.filePath(QStringLiteral("predict-source.png"));
        QImage image(12, 10, QImage::Format_RGB32);
        image.fill(QColor(0x54, 0x82, 0xA6));
        QVERIFY(image.save(imagePath, "PNG"));
        QFile imageFile(imagePath);
        QVERIFY(imageFile.open(QIODevice::ReadOnly));
        const QByteArray imageBytes = imageFile.readAll();
        imageFile.close();

        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        QSignalSpy toolSpy(actor, &AgentActor::toolExecuted);
        QSignalSpy responseSpy(&controller, &AgentController::llmResponseReceived);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject pending = controller.handleToolCall(
            QStringLiteral("sam_predict"), {{QStringLiteral("imagePath"), imagePath},
                                            {QStringLiteral("points"), QJsonArray{QJsonArray{2, 3}}},
                                            {QStringLiteral("serverUrl"), QStringLiteral("http://127.0.0.1:8990")},
                                            {QStringLiteral("timeoutMs"), 1000}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("OpenFile")));
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QCOMPARE(toolSpy.count(), 0);
        QVERIFY(imageFile.open(QIODevice::ReadOnly));
        QCOMPARE(imageFile.readAll(), imageBytes);
        QCOMPARE(responseSpy.size(), 1);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.size(), 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
        QCOMPARE(controller.conversationHistorySize(), 0);
    }

    void testAnnotationSessionQueryIsReadOnlyForEveryPermissionLevel() {
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sessionPath = dir.filePath(QStringLiteral("read-only.anno.json"));
        AnnotationSession session;
        session.imagePath = QStringLiteral("/user/provided/source.png");
        session.imageWidth = 20;
        session.imageHeight = 10;
        AnnotationObject object;
        object.id = QStringLiteral("query-only-object");
        object.label = QStringLiteral("scratch");
        object.bbox = QRectF(1, 2, 3, 4);
        object.maskRle = QStringLiteral("not-for-audit");
        session.annotations.append(object);
        QString saveError;
        QVERIFY(session.save(sessionPath, &saveError));

        QFile file(sessionPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = file.readAll();
        file.close();
        const QByteArray digest = QCryptographicHash::hash(originalBytes, QCryptographicHash::Sha256);

        AgentController& controller = AgentController::instance();
        QSignalSpy actionLogSpy(&controller, &AgentController::actionLogEntryAdded);
        const QList<AgentController::PermissionLevel> levels = {AgentController::PermissionLevel::Observer,
                                                                AgentController::PermissionLevel::Advisor,
                                                                AgentController::PermissionLevel::Autopilot};
        for (const AgentController::PermissionLevel level : levels) {
            controller.clearConversation();
            controller.setPermissionLevel(level);
            const QJsonObject result = controller.handleToolCall(
                QStringLiteral("get_annotation_session"), QJsonObject{{QStringLiteral("sessionPath"), sessionPath}});
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("session_loaded"));
            QVERIFY(controller.pendingConfirmationId().isEmpty());
            QCOMPARE(actionLogSpy.count(), levels.indexOf(level) + 1);
            const AgentActionLogEntry audit = qvariant_cast<AgentActionLogEntry>(actionLogSpy.last().at(0));
            QCOMPARE(audit.action, QStringLiteral("get_annotation_session"));
            QCOMPARE(audit.result, QStringLiteral("success"));
            QVERIFY(audit.params.contains(sessionPath));
            QVERIFY(!audit.params.contains(QStringLiteral("polygon")));
            QVERIFY(!audit.params.contains(QStringLiteral("not-for-audit")));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray after = file.readAll();
            file.close();
            QCOMPARE(QCryptographicHash::hash(after, QCryptographicHash::Sha256), digest);
        }
    }

    void testAnnotationSessionFileToolsRequireOpenAndSavePermissions() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sessionPath = dir.filePath(QStringLiteral("permissions.deeplux-anno.json"));
        AnnotationSession session;
        session.imagePath = QStringLiteral("source.png");
        session.imageWidth = 10;
        session.imageHeight = 8;
        AnnotationObject annotation;
        annotation.id = QStringLiteral("permission-object");
        annotation.label = QStringLiteral("scratch");
        annotation.bbox = QRectF(1, 1, 3, 3);
        session.annotations.append(annotation);
        QString saveError;
        QVERIFY(session.save(sessionPath, &saveError));

        AgentController& controller = AgentController::instance();
        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        const QList<QPair<QString, QJsonObject>> openTools = {
            {QStringLiteral("get_annotation_session"), {{QStringLiteral("sessionPath"), sessionPath}}},
            {QStringLiteral("open_active_annotation_session"), {{QStringLiteral("sessionPath"), sessionPath}}}};
        const QList<QPair<QString, QJsonObject>> activeReadTools = {
            {QStringLiteral("sam_predict"),
             {{QStringLiteral("imagePath"), QStringLiteral("source.png")},
              {QStringLiteral("points"), QJsonArray{QJsonArray{1, 2}}}}},
            {QStringLiteral("list_active_annotation_objects"), {}},
            {QStringLiteral("list_active_annotation_categories"), {}},
            {QStringLiteral("get_active_annotation_prompt_status"), {}}};
        const QList<QPair<QString, QJsonObject>> activeEditTools = {
            {QStringLiteral("edit_active_annotation_masks"),
             {{QStringLiteral("operation"), QStringLiteral("dilate")},
              {QStringLiteral("objectIds"), QJsonArray{QStringLiteral("permission-object")}},
              {QStringLiteral("radiusPx"), 1}}},
            {QStringLiteral("edit_active_annotation_categories"),
             {{QStringLiteral("operation"), QStringLiteral("set_object_label")},
              {QStringLiteral("objectIds"), QJsonArray{QStringLiteral("permission-object")}},
              {QStringLiteral("label"), QStringLiteral("scratch")}}}};
        const QList<QPair<QString, QJsonObject>> saveTools = {
            {QStringLiteral("save_active_annotation_session"),
             {{QStringLiteral("outputPath"), dir.filePath("save.deeplux-anno.json")}}},
            {QStringLiteral("sam_annotate"),
             {{QStringLiteral("imagePath"), "source.png"},
              {QStringLiteral("sessionPath"), dir.filePath("sam.deeplux-anno.json")},
              {QStringLiteral("items"),
               QJsonArray{QJsonObject{{QStringLiteral("points"), QJsonArray{QJsonArray{1, 2}}}}}}}},
            {QStringLiteral("export_annotation_session"),
             {{QStringLiteral("sessionPath"), sessionPath},
              {QStringLiteral("format"), "labelme"},
              {QStringLiteral("outputPath"), dir.filePath("export.json")}}},
            {QStringLiteral("transform_annotation_session"),
             {{QStringLiteral("sessionPath"), sessionPath},
              {QStringLiteral("outputPath"), dir.filePath("transform.anno")},
              {QStringLiteral("operation"), "delete_objects"},
              {QStringLiteral("objectIds"), QJsonArray{QStringLiteral("permission-object")}}}}};

        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("OpenFile")));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("SaveFile")));
        const QJsonObject operatorRead =
            controller.handleToolCall(QStringLiteral("get_annotation_session"), openTools.first().second);
        QCOMPARE(operatorRead.value(QStringLiteral("status")).toString(), QStringLiteral("session_loaded"));
        for (const auto& tool : openTools)
            QVERIFY2(actor->userPermissionPreflight(tool.first, tool.second).isEmpty(), qPrintable(tool.first));
        for (const auto& tool : activeReadTools)
            QVERIFY2(actor->userPermissionPreflight(tool.first, tool.second).isEmpty(), qPrintable(tool.first));
        for (const auto& tool : activeEditTools) {
            const QJsonObject denied = actor->userPermissionPreflight(tool.first, tool.second);
            QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(denied.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     QStringLiteral("Edit"));
        }
        for (const auto& tool : saveTools) {
            const QJsonObject denied = actor->userPermissionPreflight(tool.first, tool.second);
            QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(denied.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     QStringLiteral("SaveFile"));
        }
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        for (const auto& tool : saveTools) {
            const QJsonObject denied = controller.handleToolCall(tool.first, tool.second);
            QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(denied.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     QStringLiteral("SaveFile"));
        }
        QVERIFY(!QFileInfo::exists(saveTools.at(0).second.value(QStringLiteral("outputPath")).toString()));
        QVERIFY(!QFileInfo::exists(saveTools.at(1).second.value(QStringLiteral("sessionPath")).toString()));
        QVERIFY(!QFileInfo::exists(saveTools.at(2).second.value(QStringLiteral("outputPath")).toString()));
        QVERIFY(!QFileInfo::exists(saveTools.at(3).second.value(QStringLiteral("outputPath")).toString()));

        for (const QString& user : {QStringLiteral("developer"), QStringLiteral("administrator")}) {
            QVERIFY(UserManager::instance().login(user, QString()));
            for (const auto& tool : openTools)
                QVERIFY2(actor->userPermissionPreflight(tool.first, tool.second).isEmpty(), qPrintable(tool.first));
            for (const auto& tool : activeReadTools)
                QVERIFY2(actor->userPermissionPreflight(tool.first, tool.second).isEmpty(), qPrintable(tool.first));
            for (const auto& tool : activeEditTools)
                QVERIFY2(actor->userPermissionPreflight(tool.first, tool.second).isEmpty(), qPrintable(tool.first));
            for (const auto& tool : saveTools)
                QVERIFY2(actor->userPermissionPreflight(tool.first, tool.second).isEmpty(), qPrintable(tool.first));
        }

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        QCOMPARE(UserManager::instance().currentUser().type, UserType::Guest);
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("OpenFile")));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("SaveFile")));
        for (const auto& tool : openTools) {
            const QJsonObject denied = actor->userPermissionPreflight(tool.first, tool.second);
            QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(denied.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     QStringLiteral("OpenFile"));
        }
        for (const auto& tool : activeReadTools) {
            const QJsonObject denied = actor->userPermissionPreflight(tool.first, tool.second);
            QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(denied.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     QStringLiteral("OpenFile"));
        }
        QVERIFY(actor->userPermissionPreflight(QStringLiteral("cancel_active_annotation_prompt"), {}).isEmpty());
        for (const auto& tool : activeEditTools) {
            const QJsonObject denied = actor->userPermissionPreflight(tool.first, tool.second);
            QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(denied.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     QStringLiteral("Edit"));
        }
        for (const auto& tool : saveTools) {
            const QJsonObject denied = actor->userPermissionPreflight(tool.first, tool.second);
            QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(denied.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     QStringLiteral("SaveFile"));
        }
        UserManager::instance().logout();
        const QJsonObject loggedOutRead =
            actor->userPermissionPreflight(openTools.first().first, openTools.first().second);
        QCOMPARE(loggedOutRead.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        const QJsonObject loggedOutWrite =
            actor->userPermissionPreflight(saveTools.last().first, saveTools.last().second);
        QCOMPARE(loggedOutWrite.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(actor->userPermissionPreflight(QStringLiteral("cancel_active_annotation_prompt"), {}).isEmpty());
        QFile file(sessionPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().isEmpty());
    }

    void testCommunicationModuleConfigurationRequiresCommunicationSet() {
        QVERIFY(PluginManager::instance().initialize());
        const PluginInfo serialInfo = PluginManager::instance().pluginInfo(QStringLiteral("SerialPort"));
        QCOMPARE(serialInfo.category.toLower(), QStringLiteral("communication"));
        const PluginInfo grabInfo = PluginManager::instance().pluginInfo(QStringLiteral("GrabImage"));
        QVERIFY(!grabInfo.name.isEmpty());
        QVERIFY(PluginManager::instance().loadPlugin(serialInfo.name));
        QVERIFY(PluginManager::instance().loadPlugin(grabInfo.name));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance serial;
        serial.id = QStringLiteral("permission-serial");
        serial.moduleId = serialInfo.name;
        serial.name = QStringLiteral("Permission serial");
        serial.params = QJsonObject{{QStringLiteral("portName"), QStringLiteral("/dev/unchanged")}};
        project->addModule(serial);
        ModuleInstance grab;
        grab.id = QStringLiteral("permission-grab");
        grab.moduleId = grabInfo.name;
        grab.name = QStringLiteral("Permission grab");
        project->addModule(grab);
        const QJsonObject before = project->toJson();

        AgentActor actor;
        const QJsonObject addSerial{{QStringLiteral("plugin"), QStringLiteral("SerialPort")}};
        const QJsonObject serialEdit{
            {QStringLiteral("instanceId"), serial.id},
            {QStringLiteral("values"), QJsonObject{{QStringLiteral("portName"), QStringLiteral("/dev/changed")}}}};

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        QVERIFY(actor.userPermissionPreflight(QStringLiteral("add_module"), addSerial).isEmpty());
        QVERIFY(actor.userPermissionPreflight(QStringLiteral("set_module_params"), serialEdit).isEmpty());

        QVERIFY(UserManager::instance().login(QStringLiteral("administrator"), QString()));
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("Edit")));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("CommunicationSet")));
        const auto requiredPermission = [&actor](const QString& tool, const QJsonObject& params) {
            return actor.userPermissionPreflight(tool, params)
                .value(QStringLiteral("details"))
                .toObject()
                .value(QStringLiteral("requiredPermission"))
                .toString();
        };
        QCOMPARE(requiredPermission(QStringLiteral("add_module"), addSerial), QStringLiteral("CommunicationSet"));
        QCOMPARE(requiredPermission(QStringLiteral("set_param"),
                                    {{QStringLiteral("instanceId"), serial.id},
                                     {QStringLiteral("key"), QStringLiteral("portName")},
                                     {QStringLiteral("value"), QStringLiteral("/dev/changed")}}),
                 QStringLiteral("CommunicationSet"));
        QCOMPARE(requiredPermission(QStringLiteral("set_module_params"), serialEdit),
                 QStringLiteral("CommunicationSet"));
        QCOMPARE(requiredPermission(QStringLiteral("reset_module_params"),
                                    {{QStringLiteral("instanceId"), serial.id},
                                     {QStringLiteral("keys"), QJsonArray{QStringLiteral("portName")}}}),
                 QStringLiteral("CommunicationSet"));
        QCOMPARE(
            actor.executeTool(QStringLiteral("add_module"), addSerial).value(QStringLiteral("errorCode")).toString(),
            QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(actor.executeTool(QStringLiteral("set_module_params"), serialEdit)
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(project->toJson(), before);
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject ordinaryAdd =
            controller.handleToolCall(QStringLiteral("add_module"), {{QStringLiteral("plugin"), grabInfo.name}});
        QCOMPARE(ordinaryAdd.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString addedGrabId = ordinaryAdd.value(QStringLiteral("instanceId")).toString();
        QVERIFY(!addedGrabId.isEmpty());
        QVERIFY(project->moduleById(addedGrabId).has_value());
        const QJsonObject afterNonCommunicationAdd = project->toJson();
        QVERIFY(actor
                    .userPermissionPreflight(
                        QStringLiteral("set_module_params"),
                        {{QStringLiteral("instanceId"), grab.id}, {QStringLiteral("values"), QJsonObject{}}})
                    .isEmpty());

        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QCOMPARE(actor.userPermissionPreflight(QStringLiteral("add_module"), addSerial)
                     .value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("requiredPermission"))
                     .toString(),
                 QStringLiteral("Edit"));
        QCOMPARE(actor.userPermissionPreflight(QStringLiteral("set_module_params"), serialEdit)
                     .value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("requiredPermission"))
                     .toString(),
                 QStringLiteral("Edit"));
        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        QCOMPARE(requiredPermission(QStringLiteral("add_module"), addSerial), QStringLiteral("Edit"));
        QCOMPARE(requiredPermission(QStringLiteral("set_module_params"), serialEdit), QStringLiteral("Edit"));
        UserManager::instance().logout();
        QCOMPARE(requiredPermission(QStringLiteral("add_module"), addSerial), QStringLiteral("Edit"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QJsonObject developerEdit{
            {QStringLiteral("instanceId"), serial.id},
            {QStringLiteral("values"), QJsonObject{{QStringLiteral("portName"), QStringLiteral("/dev/developer")}}}};
        const QJsonObject developerPending =
            controller.handleToolCall(QStringLiteral("set_module_params"), developerEdit);
        QCOMPARE(developerPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(project->moduleById(serial.id)->params.value(QStringLiteral("portName")).toString(),
                 QStringLiteral("/dev/developer"));

        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject pending = controller.handleToolCall(QStringLiteral("set_module_params"), serialEdit);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        const int undoCountBeforeApproval = controller.actor()->undoStack()->count();
        const int undoIndexBeforeApproval = controller.actor()->undoStack()->index();
        const QJsonObject projectBeforeApproval = project->toJson();
        QVERIFY(UserManager::instance().login(QStringLiteral("administrator"), QString()));
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QCOMPARE(project->toJson(), projectBeforeApproval);
        QCOMPARE(controller.actor()->undoStack()->count(), undoCountBeforeApproval);
        QCOMPARE(controller.actor()->undoStack()->index(), undoIndexBeforeApproval);
        QVERIFY(controller.state() != AgentController::AgentState::Confirming);
        QVERIFY(!auditSpy.isEmpty());
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
    }

    // 第一迭代：新增只读工具在 Observer 下直接执行（不进入待确认、不被拒绝）
    void testNewReadOnlyToolsExecuteInObserverMode() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject withNode{{"nodeName", "missing_node"}};
        const QList<QPair<QString, QJsonObject>> calls = {
            {QStringLiteral("validate_workflow"), QJsonObject()},
            {QStringLiteral("get_node_runtime_outputs"), withNode},
            {QStringLiteral("get_execution_logs"), QJsonObject()},
            {QStringLiteral("capture_viewport_image"), QJsonObject()},
            {QStringLiteral("export_node_output"), withNode},
        };
        for (const auto& call : calls) {
            const QJsonObject result = AgentController::instance().handleToolCall(call.first, call.second);
            QVERIFY2(result["status"].toString() != QStringLiteral("pending_confirmation"),
                     qPrintable(QStringLiteral("%1 must not require confirmation in Observer mode").arg(call.first)));
            QVERIFY2(!result["error"].toString().contains(QStringLiteral("Observer")),
                     qPrintable(QStringLiteral("%1 must be allowed in Observer mode").arg(call.first)));
            QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        }
    }

    // 第一迭代：tune_node_and_verify 是写工具但非 dangerous —— Advisor 待确认、Autopilot 直接执行
    void testTuneToolRequiresAdvisorConfirmationButAutopilotExecutes() {
        const QJsonObject params{{"nodeName", "missing_node"}, {"parameters", QJsonObject{{"threshold", 10}}}};

        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject pending = AgentController::instance().handleToolCall("tune_node_and_verify", params);
        QCOMPARE(pending["status"].toString(), QString("pending_confirmation"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
        AgentController::instance().rejectPendingTools();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);

        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject executed = AgentController::instance().handleToolCall("tune_node_and_verify", params);
        QVERIFY(executed["status"].toString() != QString("pending_confirmation"));
        // 无工程/节点不存在 → 业务错误，但绝不是"待确认"
        QVERIFY(executed.contains("error"));
    }
};

QTEST_MAIN(TestAgentPermissions)
#include "test_agentpermissions.moc"
