#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "core/engine/FlowGraphAnalyzer.h"
#include "core/engine/RunEngine.h"
#include "core/interface/IModule.h"
#include "core/model/Project.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"
#include "ui/controllers/FlowLayoutCoordinator.h"
#include "ui/controllers/ProcessTreeController.h"
#include "ui/widgets/AgentChatPanel.h"
#include "ui/widgets/FlowCanvas.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUndoStack>
#include <QtTest/QtTest>
#include <memory>

using namespace DeepLux;

namespace {

QJsonObject makeToolCallJson(const QString& id, const QString& name, const QJsonObject& arguments) {
    QJsonObject call;
    call["id"] = id;
    call["type"] = "function";
    call["name"] = name;
    call["arguments"] = arguments;
    return call;
}

// 确定性假 LLM：记录请求上下文，由测试显式投递响应（含图片能力开关）
class ClosedLoopLLMClient : public ILLMClient {
    Q_OBJECT
public:
    explicit ClosedLoopLLMClient(QObject* parent = nullptr) : ILLMClient(parent) {}

    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}
    bool supportsImageInput() const override {
        return imageInputSupported;
    }

    void sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>&) override {
        lastCtx = ctx;
        ++requestCount;
    }

    void reply(const AgentResponse& resp) {
        emit responseReceived(resp);
        QCoreApplication::processEvents();
    }

    void replyWithToolCall(const QString& name, const QJsonObject& arguments) {
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("calling %1").arg(name);
        resp.toolCalls.append(makeToolCallJson(QStringLiteral("call_%1").arg(requestCount), name, arguments));
        reply(resp);
    }

    bool imageInputSupported = true;
    int requestCount = 0;
    AgentConversation lastCtx;
};

// B4：与产品 getFlowState 同源的 catalog——按 Project 模块顺序，用真实
// PluginManager::createModule() 取 flowControlType()/inputPorts()/outputPorts()。
FlowGraphCatalog b4CatalogForProject(const Project& project) {
    FlowGraphCatalog catalog;
    PluginManager& pm = PluginManager::instance();
    for (const ModuleInstance& instance : project.modules()) {
        std::unique_ptr<IModule> module(pm.createModule(instance.moduleId));
        if (!module)
            continue;
        FlowGraphModule graphModule;
        graphModule.instanceId = instance.id;
        graphModule.flowType = module->flowControlType();
        graphModule.inputPorts = module->inputPorts();
        graphModule.outputPorts = module->outputPorts();
        catalog.modules.append(graphModule);
    }
    return catalog;
}

struct B4PlanResult {
    FlowOrderPlan plan;
    QString error;
};

/// 期望计划完全由真实 FlowGraphAnalyzer 计算，测试不手工伪造 displayOrder/rank。
B4PlanResult b4AnalyzeProject(const Project& project) {
    B4PlanResult result;
    const FlowGraphCatalog catalog = b4CatalogForProject(project);
    for (const ModuleInstance& instance : project.modules()) {
        if (!catalog.find(instance.id)) {
            result.error = QStringLiteral("catalog is missing module %1").arg(instance.id);
            return result;
        }
    }
    QString normalizeError;
    const QList<ModuleConnection> normalized =
        FlowGraphAnalyzer::normalizeConnections(catalog, project.connections(), &normalizeError);
    if (!normalizeError.isEmpty()) {
        result.error = normalizeError;
        return result;
    }
    QStringList moduleOrder;
    for (const ModuleInstance& instance : project.modules())
        moduleOrder.append(instance.id);
    result.plan = FlowGraphAnalyzer::analyze(catalog, normalized, moduleOrder);
    return result;
}

QStringList b4TreeInstanceIds(const QTreeWidget& tree) {
    QStringList ids;
    for (int i = 0; i < tree.topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree.topLevelItem(i);
        ids.append(item ? item->data(0, Qt::UserRole + 1).toString() : QString());
    }
    return ids;
}

/// 确定性「事件队列空闲」屏障：把终止标记投递到队列尾部，执行到标记时
/// 此前排队的结构信号与 30ms 合并布局回调都已处理完毕；不使用 sleep 构造顺序。
void b4DrainEventQueue() {
    QEventLoop loop;
    QTimer::singleShot(0, &loop, [&loop]() { QTimer::singleShot(0, &loop, [&loop]() { loop.quit(); }); });
    loop.exec();
}

void b4AssertCanvasFollowsDisplayOrder(const FlowCanvas& canvas, const QStringList& displayOrder) {
    qreal previousY = -1.0e9;
    for (const QString& id : displayOrder) {
        FlowNodeItem* item = canvas.nodeItem(id);
        QVERIFY(item != nullptr);
        QVERIFY2(item->pos().y() >= previousY, "canvas rows must follow displayOrder top-to-bottom");
        previousY = item->pos().y();
    }
}

/// B4 测试夹具：真实 FlowCanvas + FlowLayoutCoordinator + ProcessTreeController，
/// 由 Project 结构信号驱动布局（Agent 不直接改画布/流程树）。
struct B4LayoutFixture {
    QTreeWidget tree;
    ProcessTreeController treeController{&tree};
    FlowCanvas canvas;
    FlowLayoutCoordinator coordinator{nullptr, &canvas, b4CatalogForProject};
    QSignalSpy layoutSpy{&coordinator, &FlowLayoutCoordinator::layoutApplied};

    void attach(Project* project) {
        QVERIFY(QObject::connect(project, &Project::moduleAdded, &treeController,
                                 &ProcessTreeController::onModuleAddedFromProject));
        QVERIFY(QObject::connect(project, &Project::moduleRemoved, &treeController,
                                 &ProcessTreeController::onModuleRemovedFromProject));
        coordinator.setProcessTreeController(&treeController);
        coordinator.setProject(project);
        QVERIFY(layoutSpy.isValid());
    }
};

} // namespace

// 第一迭代闭环验收（设计 §9.5）：
// 1) 工具级：建流程 → validate_workflow → run_flow(wait) → get_node_runtime_outputs
//    → tune_node_and_verify → capture_viewport_image（fake provider），并断言耗时字段与注册表一致；
// 2) 控制器级：工具产生的 imagePath 回灌为下一轮带图 user 消息（视觉开启）/文本降级（视觉关闭），
//    且 _elapsedMs 不进入模型上下文、审计 durationMs ≥ 0。
class TestAgentClosedLoop : public QObject {
    Q_OBJECT

private:
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

    static QString dataImagePath() {
        return QStringLiteral(DEEPLUX_TEST_SOURCE_DIR "/tests/acceptance/data/circle_640x480.png");
    }

    QTemporaryDir m_appDataDir;

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        ToolSchema::instance().registerDefaultTools();
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(m_appDataDir.path()).filePath("plugins");
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(TEST_CLOSEDLOOP_META_GrabImage),
                              QStringLiteral(TEST_CLOSEDLOOP_LIB_GrabImage)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("ShowImage"), QStringLiteral(TEST_CLOSEDLOOP_META_ShowImage),
                              QStringLiteral(TEST_CLOSEDLOOP_LIB_ShowImage)));
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("ShowImage")));
        QVERIFY(QFileInfo::exists(dataImagePath()));
    }

    void init() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
    }

    void cleanup() {
        AgentController::instance().setLLMClient(nullptr);
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
    }

    void cleanupTestCase() {
        AgentController::instance().shutdown();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    // ---------- 工具级闭环 ----------

    void testFlowToolsAndViewportCapture() {
        AgentActor actor;

        QJsonObject result = actor.executeTool(QStringLiteral("create_project"), {{"name", "ClosedLoop"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));

        result = actor.executeTool(QStringLiteral("add_module"), {{"plugin", "GrabImage"}, {"instanceName", "grab_1"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        actor.executeTool(QStringLiteral("set_param"),
                          {{"instanceId", "grab_1"}, {"key", "grabSource"}, {"value", "Path"}});
        actor.executeTool(QStringLiteral("set_param"),
                          {{"instanceId", "grab_1"}, {"key", "filePath"}, {"value", dataImagePath()}});

        // 1) 结构化校验
        result = actor.executeTool(QStringLiteral("validate_workflow"), {});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        QVERIFY2(result["ok"].toBool(), qPrintable(QString::fromUtf8(
                                            QJsonDocument(result["issues"].toArray()).toJson(QJsonDocument::Compact))));

        // 2) run_flow(wait=true) 真实执行完成
        result =
            actor.executeTool(QStringLiteral("run_flow"), {{"mode", "once"}, {"wait", true}, {"timeoutMs", 30000}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        QVERIFY2(result["success"].toBool(), qPrintable(result["errorMessage"].toString()));
        QVERIFY(!RunEngine::instance().isBusy());

        // 3) 节点运行输出
        result = actor.executeTool(QStringLiteral("get_node_runtime_outputs"), {{"nodeName", "grab_1"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        QCOMPARE(result["lastStatus"].toString(), QStringLiteral("success"));

        // 4) tune 改参核对（runMode=node 零副作用）
        result = actor.executeTool(
            QStringLiteral("tune_node_and_verify"),
            {{"nodeName", "grab_1"}, {"parameters", QJsonObject{{"filePath", dataImagePath()}}}, {"runMode", "node"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        QVERIFY2(result["success"].toBool(), qPrintable(result["error"].toString()));
        QCOMPARE(result["applied"].toInt(), 1);
        QCOMPARE(result["executed"].toBool(), false);

        // 5) 视口截图（fake provider）→ imagePath 落盘
        bool providerCalled = false;
        actor.setViewportImageProvider([&providerCalled](QString*) {
            providerCalled = true;
            QImage image(100, 80, QImage::Format_RGB32);
            image.fill(Qt::darkGreen);
            return image;
        });
        result = actor.executeTool(QStringLiteral("capture_viewport_image"), {});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        QVERIFY(providerCalled);
        QCOMPARE(result["source"].toString(), QStringLiteral("viewport"));
        QCOMPARE(result["width"].toInt(), 100);
        QCOMPARE(result["height"].toInt(), 80);
        const QString imagePath = result["imagePath"].toString();
        QVERIFY(!imagePath.isEmpty());
        QVERIFY(QFileInfo::exists(imagePath));
        QVERIFY2(imagePath.startsWith(QDir::tempPath() + QStringLiteral("/deeplux-agent")), qPrintable(imagePath));

        // 6) 耗时内部字段：工具层必须产出（消费端剥离由控制器测试断言）
        QVERIFY(result.contains(QStringLiteral("_elapsedMs")));
        QVERIFY(result.value(QStringLiteral("_elapsedMs")).toInt() >= 0);
    }

    void testSchemaMatchesSupportedTools() {
        AgentActor actor;
        QSet<QString> schemaNames;
        for (const ToolDefinition& tool : ToolSchema::instance().allTools())
            schemaNames.insert(tool.name);
        QSet<QString> handlerNames;
        for (const QString& name : actor.supportedTools())
            handlerNames.insert(name);

        const QStringList missingHandlers = (schemaNames - handlerNames).values();
        QVERIFY2(missingHandlers.isEmpty(),
                 qPrintable(QStringLiteral("schema tools without handler: %1").arg(missingHandlers.join(", "))));
        const QStringList orphanHandlers = (handlerNames - schemaNames).values();
        QVERIFY2(orphanHandlers.isEmpty(),
                 qPrintable(QStringLiteral("handlers without schema: %1").arg(orphanHandlers.join(", "))));
    }

    // ---------- B1：失败闭环 ----------

    // run_flow 运行失败后，本批剩余写操作零执行，失败与跳过结果都回灌模型，
    // 模型获得一次重新规划机会；参数保持失败时的值（不静默回滚）。
    void testRunFailureSkipsRemainingBatchAndReplans() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        AgentActor* actor = controller.actor();
        QVERIFY(actor != nullptr);

        QVERIFY2(!actor->executeTool(QStringLiteral("create_project"), {{"name", "FailureLoop"}}).contains("error"),
                 "create_project failed");
        QVERIFY2(
            !actor->executeTool(QStringLiteral("add_module"), {{"plugin", "GrabImage"}, {"instanceName", "grab_fail"}})
                 .contains("error"),
            "add_module failed");
        QVERIFY2(!actor
                      ->executeTool(QStringLiteral("set_param"),
                                    {{"instanceId", "grab_fail"}, {"key", "grabSource"}, {"value", "Path"}})
                      .contains("error"),
                 "set grabSource failed");
        QVERIFY2(!actor
                      ->executeTool(QStringLiteral("set_param"), {{"instanceId", "grab_fail"},
                                                                  {"key", "filePath"},
                                                                  {"value", "/nonexistent/deeplux-missing.png"}})
                      .contains("error"),
                 "set filePath failed");
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);
        const QJsonValue beforeTimeout = project->moduleById(QStringLiteral("grab_fail"))->params.value("grabTimeout");

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ClosedLoopLLMClient client;
        controller.setLLMClient(&client);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);

        controller.sendUserMessage(QStringLiteral("run the flow, then tweak the timeout"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("running");
        resp.toolCalls.append(makeToolCallJson(QStringLiteral("call-run"), QStringLiteral("run_flow"), {}));
        resp.toolCalls.append(
            makeToolCallJson(QStringLiteral("call-tune"), QStringLiteral("set_param"),
                             QJsonObject{{"instanceId", "grab_fail"}, {"key", "grabTimeout"}, {"value", 12345}}));
        client.reply(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));

        // 有副作用的批次执行后失败时停止自动重试，失败和跳过结果仍配对保存在对话中。
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 10000);
        QCOMPARE(client.requestCount, 1);

        bool sawRunFailure = false;
        bool sawSkippedTune = false;
        for (const AgentMessage& message : controller.conversationHistory()) {
            if (message.role != QStringLiteral("tool"))
                continue;
            const QJsonObject payload = QJsonDocument::fromJson(message.content.toUtf8()).object();
            if (message.toolCallId == QLatin1String("call-run")) {
                sawRunFailure = payload.value("errorCode").toString() == QLatin1String("RUN_FAILED");
            } else if (message.toolCallId == QLatin1String("call-tune")) {
                sawSkippedTune =
                    payload.value("errorCode").toString() == QLatin1String("SKIPPED_DUE_TO_PREVIOUS_ERROR");
            }
        }
        QVERIFY2(sawRunFailure, "run failure must be fed back to the model");
        QVERIFY2(sawSkippedTune, "the remaining write must be reported as skipped");

        // 跳过项零执行：参数保持失败时的值
        QCOMPARE(project->moduleById(QStringLiteral("grab_fail"))->params.value("grabTimeout"), beforeTimeout);

        QStringList auditResults;
        for (const QList<QVariant>& args : auditSpy) {
            const AgentActionLogEntry entry = args.at(0).value<AgentActionLogEntry>();
            if (entry.action == QLatin1String("run_flow") || entry.action == QLatin1String("set_param"))
                auditResults << entry.result;
        }
        QVERIFY2(auditResults.contains(QStringLiteral("error")), qPrintable(auditResults.join(",")));
        QCOMPARE(auditResults.size(), 2);

        AgentResponse done;
        done.success = true;
        done.content = QStringLiteral("replanned");
        client.reply(done);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    // run_flow 失败后停止自动重试，不能让模型连续重放有副作用的执行请求。
    void testRepeatedRunFailuresStopAtTurnLimit() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        AgentActor* actor = controller.actor();
        QVERIFY(actor != nullptr);
        QVERIFY(!actor->executeTool(QStringLiteral("create_project"), {{"name", "RetryLoop"}}).contains("error"));
        QVERIFY(
            !actor->executeTool(QStringLiteral("add_module"), {{"plugin", "GrabImage"}, {"instanceName", "grab_retry"}})
                 .contains("error"));
        QVERIFY(!actor
                     ->executeTool(QStringLiteral("set_param"),
                                   {{"instanceId", "grab_retry"}, {"key", "grabSource"}, {"value", "Path"}})
                     .contains("error"));
        QVERIFY(!actor
                     ->executeTool(QStringLiteral("set_param"), {{"instanceId", "grab_retry"},
                                                                 {"key", "filePath"},
                                                                 {"value", "/nonexistent/deeplux-retry.png"}})
                     .contains("error"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ClosedLoopLLMClient client;
        controller.setLLMClient(&client);
        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);

        controller.sendUserMessage(QStringLiteral("keep retrying"));
        AgentResponse response;
        response.success = true;
        response.toolCalls.append(makeToolCallJson(QStringLiteral("retry_0"), QStringLiteral("run_flow"), {}));
        client.reply(response);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 10000);

        QCOMPARE(client.requestCount, 1);
        QVERIFY(!errorSpy.isEmpty());
        QVERIFY(errorSpy.last().at(0).toString().contains(QStringLiteral("失败")));
    }

    // 运行失败会跳过同批次诊断工具，等待用户发起新的修复轮次。
    void testDiagnoseTuneRerunClosedLoop() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        AgentActor* actor = controller.actor();
        QVERIFY(actor != nullptr);
        QVERIFY(!actor->executeTool(QStringLiteral("create_project"), {{"name", "DiagnoseLoop"}}).contains("error"));
        QVERIFY(
            !actor->executeTool(QStringLiteral("add_module"), {{"plugin", "GrabImage"}, {"instanceName", "grab_fix"}})
                 .contains("error"));
        QVERIFY(!actor
                     ->executeTool(QStringLiteral("set_param"),
                                   {{"instanceId", "grab_fix"}, {"key", "grabSource"}, {"value", "Path"}})
                     .contains("error"));
        const QString missingPath = QStringLiteral("/nonexistent/deeplux-diagnose.png");
        QVERIFY(!actor
                     ->executeTool(QStringLiteral("set_param"),
                                   {{"instanceId", "grab_fix"}, {"key", "filePath"}, {"value", missingPath}})
                     .contains("error"));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ClosedLoopLLMClient client;
        controller.setLLMClient(&client);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);

        controller.sendUserMessage(QStringLiteral("the flow fails, diagnose and fix it"));
        AgentResponse runFail;
        runFail.success = true;
        runFail.toolCalls.append(makeToolCallJson(QStringLiteral("run-1"), QStringLiteral("run_flow"), {}));
        runFail.toolCalls.append(makeToolCallJson(QStringLiteral("diag-1"), QStringLiteral("get_node_runtime_outputs"),
                                                  QJsonObject{{"nodeName", "grab_fix"}}));
        client.reply(runFail);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));

        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 10000);
        QCOMPARE(client.requestCount, 1);
        bool runFailed = false;
        bool diagnoseSkipped = false;
        for (const AgentMessage& message : controller.conversationHistory()) {
            if (message.role != QLatin1String("tool"))
                continue;
            const QJsonObject payload = QJsonDocument::fromJson(message.content.toUtf8()).object();
            if (message.toolCallId == QLatin1String("run-1"))
                runFailed = payload.value("errorCode").toString() == QLatin1String("RUN_FAILED");
            if (message.toolCallId == QLatin1String("diag-1"))
                diagnoseSkipped =
                    payload.value("errorCode").toString() == QLatin1String("SKIPPED_DUE_TO_PREVIOUS_ERROR");
        }
        QVERIFY(runFailed);
        QVERIFY(diagnoseSkipped);

        QStringList outcomes;
        for (const QList<QVariant>& args : auditSpy) {
            const AgentActionLogEntry entry = args.at(0).value<AgentActionLogEntry>();
            if (entry.action == QLatin1String("run_flow") || entry.action == QLatin1String("set_param") ||
                entry.action == QLatin1String("get_node_runtime_outputs"))
                outcomes << entry.result;
        }
        QVERIFY2(outcomes.contains(QStringLiteral("error")), qPrintable(outcomes.join(QStringLiteral(","))));
        QCOMPARE(outcomes.size(), 2);

        // 失败批次没有改动工程参数。
        QCOMPARE(project->moduleById(QStringLiteral("grab_fix"))->params.value("filePath").toString(), missingPath);
    }

    // 不可重试的参数校验失败会停止批次；后续写调用不执行且每个 callId 都有结果。
    void testInvalidPluginParameterStopsBatchWithoutRunningRemainingWrites() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        AgentActor* actor = controller.actor();
        QVERIFY(actor != nullptr);
        QVERIFY(!actor->executeTool(QStringLiteral("create_project"), {{"name", "ParamFailure"}}).contains("error"));
        QVERIFY(
            !actor->executeTool(QStringLiteral("add_module"), {{"plugin", "GrabImage"}, {"instanceName", "grab_param"}})
                 .contains("error"));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);
        const int beforeModules = project->modules().size();

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ClosedLoopLLMClient client;
        controller.setLLMClient(&client);

        controller.sendUserMessage(QStringLiteral("set a valid plugin parameter"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("try a parameter, then continue only if valid");
        resp.toolCalls.append(makeToolCallJson(
            QStringLiteral("call-bad"), QStringLiteral("set_param"),
            QJsonObject{{"instanceId", "grab_param"}, {"key", "notAPluginParameter"}, {"value", "value"}}));
        resp.toolCalls.append(
            makeToolCallJson(QStringLiteral("call-add"), QStringLiteral("add_module"),
                             QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "must_not_exist"}}));
        client.reply(resp);

        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(client.requestCount, 1);
        QCOMPARE(project->modules().size(), beforeModules);

        // 模型收到明确参数错误和批次跳过结果，且两条 tool_call 均配对。
        bool sawInvalidParameter = false;
        bool sawSkippedWrite = false;
        for (const AgentMessage& message : controller.conversationHistory()) {
            if (message.role != QLatin1String("tool"))
                continue;
            const QJsonObject payload = QJsonDocument::fromJson(message.content.toUtf8()).object();
            if (message.toolCallId == QLatin1String("call-bad"))
                sawInvalidParameter = payload.value("errorCode").toString() == QLatin1String("INVALID_PARAM");
            if (message.toolCallId == QLatin1String("call-add"))
                sawSkippedWrite =
                    payload.value("errorCode").toString() == QLatin1String("SKIPPED_DUE_TO_PREVIOUS_ERROR");
        }
        QVERIFY(sawInvalidParameter);
        QVERIFY(sawSkippedWrite);
    }

    void testGenericToolErrorStopsBatchWithoutExecutingRemainingCalls() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        AgentActor* actor = controller.actor();
        QVERIFY(actor != nullptr);
        QVERIFY(
            !actor->executeTool(QStringLiteral("create_project"), {{"name", "ConnectionRepair"}}).contains("error"));
        for (const QString& name : {QStringLiteral("grab_old"), QStringLiteral("grab_new")}) {
            QVERIFY(!actor->executeTool(QStringLiteral("add_module"), {{"plugin", "GrabImage"}, {"instanceName", name}})
                         .contains("error"));
        }
        QVERIFY(!actor->executeTool(QStringLiteral("add_module"), {{"plugin", "ShowImage"}, {"instanceName", "show"}})
                     .contains("error"));
        QVERIFY(!actor->executeTool(QStringLiteral("connect_modules"), {{"fromId", "grab_old"}, {"toId", "show"}})
                     .contains("error"));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ClosedLoopLLMClient client;
        controller.setLLMClient(&client);
        controller.sendUserMessage(QStringLiteral("connect the new image source"));

        AgentResponse failedPlan;
        failedPlan.success = true;
        failedPlan.toolCalls.append(makeToolCallJson(QStringLiteral("connect-conflict"),
                                                     QStringLiteral("connect_modules"),
                                                     QJsonObject{{"fromId", "grab_new"}, {"toId", "show"}}));
        failedPlan.toolCalls.append(
            makeToolCallJson(QStringLiteral("must-be-skipped"), QStringLiteral("add_module"),
                             QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "must_not_be_added"}}));
        client.reply(failedPlan);

        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(client.requestCount, 1);
        QVERIFY(!project->moduleById(QStringLiteral("must_not_be_added")));
        bool sawOccupiedInputError = false;
        bool sawSkippedCall = false;
        for (const AgentMessage& message : controller.conversationHistory()) {
            if (message.role != QLatin1String("tool"))
                continue;
            const QJsonObject payload = QJsonDocument::fromJson(message.content.toUtf8()).object();
            if (message.toolCallId == QLatin1String("connect-conflict"))
                sawOccupiedInputError = payload.value("error").toString().contains("Input port already connected");
            if (message.toolCallId == QLatin1String("must-be-skipped"))
                sawSkippedCall =
                    payload.value("errorCode").toString() == QLatin1String("SKIPPED_DUE_TO_PREVIOUS_ERROR");
        }
        QVERIFY(sawOccupiedInputError);
        QVERIFY(sawSkippedCall);

        QCOMPARE(project->connections().size(), 1);
        QCOMPARE(project->connections().first().fromModuleId, QStringLiteral("grab_old"));
    }

    // ---------- 控制器级图像回灌 ----------

    void testToolImageReflowsIntoNextTurn() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        AgentActor* actor = controller.actor();
        QVERIFY(actor != nullptr);
        // 大图（3000x1500）：回灌副本必须长边 ≤1280，磁盘原件尺寸不受影响
        actor->setViewportImageProvider([](QString*) {
            QImage image(3000, 1500, QImage::Format_RGB32);
            image.fill(Qt::darkCyan);
            return image;
        });

        ClosedLoopLLMClient client;
        client.imageInputSupported = true;
        controller.setLLMClient(&client);
        QSignalSpy usageSpy(&controller, &AgentController::actionLogEntryAdded);

        controller.sendUserMessage(QStringLiteral("capture the viewport"));
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        client.replyWithToolCall(QStringLiteral("capture_viewport_image"), {});

        // 工具执行 + 图像回灌后自动续跑 → 第二次 LLM 请求
        QTRY_COMPARE_WITH_TIMEOUT(client.requestCount, 2, 5000);
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);

        int imageMessages = 0;
        QString attachedPath;
        for (const AgentMessage& message : client.lastCtx.messages) {
            if (message.role == QStringLiteral("tool")) {
                QVERIFY2(!message.content.contains(QStringLiteral("_elapsedMs")),
                         "_elapsedMs must never reach the model context");
            }
            if (message.role != QStringLiteral("user") || message.images.isEmpty())
                continue;
            ++imageMessages;
            QVERIFY2(message.images.size() <= 2, "at most 2 tool images per turn");
            QVERIFY(message.content.contains(QStringLiteral("Images produced by the tool calls above")));
            const AgentImageAttachment& attachment = message.images.constFirst();
            attachedPath = attachment.description;
            QVERIFY(!attachment.data.isEmpty());
            const QImage decoded = QImage::fromData(attachment.data);
            QVERIFY(!decoded.isNull());
            QVERIFY2(qMax(decoded.width(), decoded.height()) <= 1280, "reflow copy long edge must be <= 1280");
        }
        QCOMPARE(imageMessages, 1);
        QVERIFY(!attachedPath.isEmpty());
        QVERIFY2(QFileInfo::exists(attachedPath), qPrintable(attachedPath));

        // 审计：durationMs 已由 _elapsedMs 采集（≥0），且工具调用轮次未被图片消息改变
        QVERIFY(!usageSpy.isEmpty());
        bool sawCaptureTool = false;
        for (const QList<QVariant>& args : usageSpy) {
            const AgentActionLogEntry entry = args.at(0).value<AgentActionLogEntry>();
            if (entry.action != QStringLiteral("capture_viewport_image"))
                continue;
            sawCaptureTool = true;
            QVERIFY2(entry.durationMs >= 0, "audit durationMs must be collected from _elapsedMs");
        }
        QVERIFY(sawCaptureTool);

        AgentResponse done;
        done.success = true;
        done.content = QStringLiteral("done");
        client.reply(done);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(client.requestCount, 2); // 一次工具轮 + 一次收尾，不因图片回灌增加轮次
    }

    void testToolImageFallsBackToTextWhenVisionDisabled() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        AgentActor* actor = controller.actor();
        QVERIFY(actor != nullptr);
        actor->setViewportImageProvider([](QString*) {
            QImage image(120, 90, QImage::Format_RGB32);
            image.fill(Qt::yellow);
            return image;
        });

        ClosedLoopLLMClient client;
        client.imageInputSupported = false; // 视觉关闭：只允许文本降级
        controller.setLLMClient(&client);

        controller.sendUserMessage(QStringLiteral("capture the viewport"));
        client.replyWithToolCall(QStringLiteral("capture_viewport_image"), {});

        QTRY_COMPARE_WITH_TIMEOUT(client.requestCount, 2, 5000);
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);

        bool sawFallback = false;
        for (const AgentMessage& message : client.lastCtx.messages) {
            if (message.role != QStringLiteral("user"))
                continue;
            QVERIFY(message.images.isEmpty());
            if (message.content.contains(QStringLiteral("does not support image input"))) {
                sawFallback = true;
                QVERIFY(message.content.contains(QStringLiteral("Image path: ")));
                QVERIFY(message.content.contains(QLatin1String("deeplux-agent")));
            }
        }
        QVERIFY(sawFallback);

        AgentResponse done;
        done.success = true;
        done.content = QStringLiteral("done");
        client.reply(done);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    // 每轮最多回灌 2 张（设计 §6.3）：3 次截图只带 2 张，第 3 张以文字说明跳过
    void testToolImageCountIsCappedPerTurn() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        AgentActor* actor = controller.actor();
        QVERIFY(actor != nullptr);
        actor->setViewportImageProvider([](QString*) {
            QImage image(160, 120, QImage::Format_RGB32);
            image.fill(Qt::magenta);
            return image;
        });

        ClosedLoopLLMClient client;
        client.imageInputSupported = true;
        controller.setLLMClient(&client);

        controller.sendUserMessage(QStringLiteral("capture three times"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("capturing");
        for (int i = 0; i < 3; ++i) {
            resp.toolCalls.append(
                makeToolCallJson(QStringLiteral("cap_%1").arg(i), QStringLiteral("capture_viewport_image"), {}));
        }
        client.reply(resp);

        QTRY_COMPARE_WITH_TIMEOUT(client.requestCount, 2, 5000);
        int imageMessages = 0;
        for (const AgentMessage& message : client.lastCtx.messages) {
            if (message.role != QStringLiteral("user") || message.images.isEmpty())
                continue;
            ++imageMessages;
            QCOMPARE(message.images.size(), 2);
            QVERIFY2(message.content.contains(QStringLiteral("per-turn image limit reached")),
                     qPrintable(message.content));
        }
        QCOMPARE(imageMessages, 1);

        AgentResponse done;
        done.success = true;
        done.content = QStringLiteral("done");
        client.reply(done);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    // ---------- 停止按钮 / 状态条（设计 §5.6） ----------

    void testStopButtonAndThinkingStatus() {
        AgentChatPanel panel;
        auto* stop = panel.findChild<QToolButton*>(QStringLiteral("AgentStopMessage"));
        auto* status = panel.findChild<QLabel*>(QStringLiteral("AgentChatStatusLabel"));
        QVERIFY(stop != nullptr);
        QVERIFY(status != nullptr);
        QVERIFY2(stop->isHidden(), "stop button must be hidden while idle");

        panel.setThinking(true);
        QVERIFY2(!stop->isHidden(), "stop button must be visible while thinking");
        QVERIFY2(status->text().contains(QStringLiteral("正在思考")), qPrintable(status->text()));

        panel.setExecuting(true);
        QVERIFY2(!stop->isHidden(), "stop button must be visible while executing");
        QVERIFY2(status->text().contains(QStringLiteral("执行中")), qPrintable(status->text()));

        QSignalSpy stopSpy(&panel, &AgentChatPanel::stopRequested);
        stop->click();
        QCOMPARE(stopSpy.count(), 1);

        panel.setStopped();
        QVERIFY2(status->text().contains(QStringLiteral("已停止")), qPrintable(status->text()));
        QVERIFY2(stop->isHidden(), "stop button must hide after stop");
    }

    // ---------- B4：Agent 集成与顺序字段 ----------

    // 脚本化 Agent 构建线性链；最终画布/流程树顺序 == 真实 FlowGraphAnalyzer 的
    // plan.displayOrder，且 get_flow_state 的字段与同一计划一致。
    void testAgentFlowStateAndLayoutFollowExecutionPlan() {
        AgentActor actor;
        QJsonObject result = actor.executeTool("create_project", {{"name", "B4Layout"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);

        B4LayoutFixture fixture;
        fixture.attach(project);

        result = actor.executeTool("add_module", {{"plugin", "GrabImage"}, {"instanceName", "grab"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        result = actor.executeTool("add_module", {{"plugin", "ShowImage"}, {"instanceName", "show1"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        result = actor.executeTool("add_module", {{"plugin", "ShowImage"}, {"instanceName", "show2"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        result = actor.executeTool("connect_modules", {{"fromId", "grab"}, {"toId", "show1"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        result = actor.executeTool("connect_modules", {{"fromId", "show1"}, {"toId", "show2"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));

        fixture.canvas.loadFromProject(project);
        QTRY_VERIFY(fixture.coordinator.layoutRunCount() >= 1);
        b4DrainEventQueue();

        const B4PlanResult expected = b4AnalyzeProject(*project);
        QVERIFY2(expected.error.isEmpty(), qPrintable(expected.error));
        QVERIFY(expected.plan.valid);
        QCOMPARE(expected.plan.displayOrder.size(), 3);
        QCOMPARE(fixture.coordinator.currentPlan().displayOrder, expected.plan.displayOrder);
        QCOMPARE(b4TreeInstanceIds(fixture.tree), expected.plan.displayOrder);
        b4AssertCanvasFollowsDisplayOrder(fixture.canvas, expected.plan.displayOrder);

        QHash<QString, FlowOrderNode> nodesById;
        for (const FlowOrderNode& node : expected.plan.nodes)
            nodesById.insert(node.instanceId, node);

        const QJsonObject state = actor.executeTool("get_flow_state", QJsonObject());
        QVERIFY2(!state.contains("error"), qPrintable(state.value("error").toString()));
        const QJsonArray modules = state.value("modules").toArray();
        QCOMPARE(modules.size(), 3);
        for (const QJsonValue& value : modules) {
            const QJsonObject obj = value.toObject();
            const QString id = obj.value("id").toString();
            QVERIFY(nodesById.contains(id));
            const FlowOrderNode& node = nodesById.value(id);
            QCOMPARE(obj.value("executionRank").toInt(), node.rank);
            QCOMPARE(obj.value("lane").toInt(), node.lane);
            QCOMPARE(obj.value("displayIndex").toInt(), node.displayIndex);
        }
    }

    // 连续添加 5 个模块只触发一次合并布局（单计时器合并生效）。
    // 用 QSignalSpy/QTRY + 事件队列空闲屏障构造确定顺序，不使用 sleep。
    void testConsecutiveAgentAddsCoalesceIntoSingleLayout() {
        AgentActor actor;
        QJsonObject result = actor.executeTool("create_project", {{"name", "B4Coalesce"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);

        B4LayoutFixture fixture;
        fixture.attach(project);

        // 5 次 add_module 全部在同一轮同步调用内完成，各自发出 moduleAdded +
        // structureChanged(Agent)，必须合并为一次 30ms 布局。
        for (int i = 0; i < 5; ++i) {
            const QString name = QStringLiteral("node_%1").arg(i);
            result = actor.executeTool("add_module", {{"plugin", "ShowImage"}, {"instanceName", name}});
            QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        }
        fixture.canvas.loadFromProject(project);

        QTRY_COMPARE(fixture.layoutSpy.count(), 1);
        QCOMPARE(fixture.coordinator.layoutRunCount(), 1);
        b4DrainEventQueue();
        QCOMPARE(fixture.coordinator.layoutRunCount(), 1);
        QCOMPARE(fixture.layoutSpy.count(), 1);

        const B4PlanResult expected = b4AnalyzeProject(*project);
        QVERIFY2(expected.error.isEmpty(), qPrintable(expected.error));
        QVERIFY(expected.plan.valid);
        QCOMPARE(fixture.coordinator.currentPlan().displayOrder, expected.plan.displayOrder);
        QCOMPARE(b4TreeInstanceIds(fixture.tree), expected.plan.displayOrder);
        b4AssertCanvasFollowsDisplayOrder(fixture.canvas, expected.plan.displayOrder);
    }

    // Agent 暂停/撤销后布局仍与计划一致：暂停不改结构，撤销由结构信号重新合并布局。
    void testAgentPauseAndUndoKeepLayoutConsistentWithPlan() {
        AgentActor actor;
        QJsonObject result = actor.executeTool("create_project", {{"name", "B4PauseUndo"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);

        B4LayoutFixture fixture;
        fixture.attach(project);

        result = actor.executeTool("add_module", {{"plugin", "GrabImage"}, {"instanceName", "grab"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        result = actor.executeTool("add_module", {{"plugin", "ShowImage"}, {"instanceName", "show1"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        result = actor.executeTool("add_module", {{"plugin", "ShowImage"}, {"instanceName", "show2"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        result = actor.executeTool("connect_modules", {{"fromId", "grab"}, {"toId", "show1"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        result = actor.executeTool("connect_modules", {{"fromId", "show1"}, {"toId", "show2"}});
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));

        fixture.canvas.loadFromProject(project);
        QTRY_VERIFY(fixture.coordinator.layoutRunCount() >= 1);
        b4DrainEventQueue();
        const int baselineRuns = fixture.coordinator.layoutRunCount();

        B4PlanResult expected = b4AnalyzeProject(*project);
        QVERIFY2(expected.error.isEmpty(), qPrintable(expected.error));
        QVERIFY(expected.plan.valid);
        QCOMPARE(b4TreeInstanceIds(fixture.tree), expected.plan.displayOrder);

        // 1) 暂停：只读/无结构变化，不触发新布局，计划保持一致。
        const QJsonObject paused = actor.executeTool("pause_flow", QJsonObject());
        QCOMPARE(paused.value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(paused.value("reason").toString(), QStringLiteral("engine not running; nothing to pause"));
        b4DrainEventQueue();
        QCOMPARE(fixture.coordinator.layoutRunCount(), baselineRuns);
        QCOMPARE(fixture.coordinator.currentPlan().displayOrder, expected.plan.displayOrder);
        QCOMPARE(b4TreeInstanceIds(fixture.tree), expected.plan.displayOrder);

        // 2) 撤销最后一次连接：结构信号驱动一次新的合并布局。
        QVERIFY(actor.undoStack()->canUndo());
        actor.undoStack()->undo();
        QCOMPARE(project->connections().size(), 1);
        fixture.canvas.loadFromProject(project);
        QTRY_VERIFY(fixture.coordinator.layoutRunCount() > baselineRuns);
        b4DrainEventQueue();

        expected = b4AnalyzeProject(*project);
        QVERIFY2(expected.error.isEmpty(), qPrintable(expected.error));
        QVERIFY(expected.plan.valid);
        QCOMPARE(fixture.coordinator.currentPlan().displayOrder, expected.plan.displayOrder);
        QCOMPARE(b4TreeInstanceIds(fixture.tree), expected.plan.displayOrder);
        b4AssertCanvasFollowsDisplayOrder(fixture.canvas, expected.plan.displayOrder);

        QHash<QString, FlowOrderNode> nodesById;
        for (const FlowOrderNode& node : expected.plan.nodes)
            nodesById.insert(node.instanceId, node);
        const QJsonObject state = actor.executeTool("get_flow_state", QJsonObject());
        QVERIFY2(!state.contains("error"), qPrintable(state.value("error").toString()));
        for (const QJsonValue& value : state.value("modules").toArray()) {
            const QJsonObject obj = value.toObject();
            const QString id = obj.value("id").toString();
            QVERIFY(nodesById.contains(id));
            QCOMPARE(obj.value("displayIndex").toInt(), nodesById.value(id).displayIndex);
        }
    }
};

QTEST_MAIN(TestAgentClosedLoop)
#include "test_agentclosedloop.moc"
