#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/engine/RunEngine.h"
#include "core/model/Project.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"
#include "ui/widgets/AgentChatPanel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSet>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest/QtTest>

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
        ToolSchema::instance().registerDefaultTools();
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(m_appDataDir.path()).filePath("plugins");
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(TEST_CLOSEDLOOP_META_GrabImage),
                              QStringLiteral(TEST_CLOSEDLOOP_LIB_GrabImage)));
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(QFileInfo::exists(dataImagePath()));
    }

    void init() {
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
};

QTEST_MAIN(TestAgentClosedLoop)
#include "test_agentclosedloop.moc"
