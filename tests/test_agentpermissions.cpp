#include "TestPluginRegistry.h"
#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/AgentToolImage.h"
#include "core/agent/ToolSchema.h"
#include "core/common/Logger.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/model/Project.h"
#include "manager/ProjectManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestAgentPermissions : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appDataDir;
    QStringList m_generatedImagePaths;

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const auto grabImage = DeepLuxTest::pluginEntry(QStringLiteral("GrabImage"));
        QVERIFY(!grabImage.libraryPath.isEmpty());
        QVERIFY(!grabImage.metadataPath.isEmpty());
        const QString pluginDir = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins/GrabImage"));
        QVERIFY(QDir().mkpath(pluginDir));
        QVERIFY(QFile::copy(grabImage.metadataPath, QDir(pluginDir).filePath(QStringLiteral("metadata.json"))));
        QVERIFY(
            QFile::copy(grabImage.libraryPath, QDir(pluginDir).filePath(QFileInfo(grabImage.libraryPath).fileName())));
        PluginManager::instance().addPluginPath(QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins")));
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
    }

    void init() {
        AgentController::instance().clearConversation();
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void cleanup() {
        AgentController::instance().clearConversation();
        AgentController::instance().actor()->setViewportImageProvider(AgentActor::ViewportImageProvider());
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        for (const QString& path : m_generatedImagePaths)
            QFile::remove(path);
        m_generatedImagePaths.clear();
    }

    void cleanupTestCase() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        PluginManager::instance().shutdown();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
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
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString sessionPath = tempDir.filePath(QStringLiteral("never-written-sam-gate.json"));
        QJsonObject annotatePending = AgentController::instance().handleToolCall(
            "sam_annotate",
            QJsonObject{{"imagePath", "/nonexistent_sam_gate.png"},
                        {"sessionPath", sessionPath},
                        {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}}}}}});
        QCOMPARE(annotatePending["status"].toString(), QString("pending_confirmation"));
        QVERIFY(!QFileInfo::exists(sessionPath));
        AgentController::instance().rejectPendingTools();
    }

    // Read-only tools must execute successfully in Observer mode, not just avoid permission denial.
    void testNewReadOnlyToolsExecuteInObserverMode() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params.insert(QStringLiteral("grabSource"), QStringLiteral("Demo"));
        project->addModule(grab);

        const QJsonObject validation = controller.handleToolCall(QStringLiteral("validate_workflow"), QJsonObject());
        QVERIFY2(!validation.contains(QStringLiteral("error")), qPrintable(validation.value("error").toString()));
        QCOMPARE(validation.value("ok").toBool(), true);
        QCOMPARE(validation.value("moduleCount").toInt(), 1);

        RunEngine::instance().runOnce();
        QVERIFY(!RunEngine::instance().isBusy());
        const QJsonObject outputs =
            controller.handleToolCall(QStringLiteral("get_node_runtime_outputs"),
                                      QJsonObject{{QStringLiteral("nodeName"), QStringLiteral("grab")}});
        QVERIFY2(!outputs.contains(QStringLiteral("error")), qPrintable(outputs.value("error").toString()));
        QCOMPARE(outputs.value("node").toString(), QStringLiteral("grab"));
        QCOMPARE(outputs.value("lastStatus").toString(), QStringLiteral("success"));
        QVERIFY(outputs.value("frameId").toDouble() > 0);
        QCOMPARE(outputs.value("outputs")
                     .toObject()
                     .value(QStringLiteral("image"))
                     .toObject()
                     .value(QStringLiteral("present"))
                     .toBool(),
                 true);

        Logger::instance().info(QStringLiteral("observer-readonly-contract"), QStringLiteral("Run"));
        const QJsonObject logs = controller.handleToolCall(
            QStringLiteral("get_execution_logs"),
            QJsonObject{{QStringLiteral("category"), QStringLiteral("Run")},
                        {QStringLiteral("keyword"), QStringLiteral("observer-readonly-contract")}});
        QVERIFY2(!logs.contains(QStringLiteral("error")), qPrintable(logs.value("error").toString()));
        QCOMPARE(logs.value("source").toString(), QStringLiteral("Logger::memory"));
        QCOMPARE(logs.value("total").toInt(), 1);
        QCOMPARE(logs.value("entries").toArray().first().toObject().value("message").toString(),
                 QStringLiteral("observer-readonly-contract"));

        controller.actor()->setViewportImageProvider([](QString*) {
            QImage image(32, 24, QImage::Format_RGB32);
            image.fill(Qt::green);
            return image;
        });
        const QJsonObject captured = controller.handleToolCall(QStringLiteral("capture_viewport_image"), QJsonObject());
        const QString capturedPath = captured.value("imagePath").toString();
        if (!capturedPath.isEmpty())
            m_generatedImagePaths.append(capturedPath);
        QVERIFY2(!captured.contains(QStringLiteral("error")), qPrintable(captured.value("error").toString()));
        QCOMPARE(captured.value("width").toInt(), 32);
        QCOMPARE(captured.value("height").toInt(), 24);
        QVERIFY(QFileInfo::exists(capturedPath));

        const QJsonObject exported = controller.handleToolCall(
            QStringLiteral("export_node_output"), QJsonObject{{QStringLiteral("nodeName"), QStringLiteral("grab")}});
        const QString exportedPath = exported.value("imagePath").toString();
        if (!exportedPath.isEmpty())
            m_generatedImagePaths.append(exportedPath);
        QVERIFY2(!exported.contains(QStringLiteral("error")), qPrintable(exported.value("error").toString()));
        QCOMPARE(exported.value("source").toString(), QStringLiteral("module_output"));
        QVERIFY(QFileInfo::exists(exportedPath));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
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
