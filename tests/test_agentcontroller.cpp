#include <QJsonDocument>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>

// 测试需要访问 AgentController 的私有成员（m_conversationHistory, trimHistory）
// 这是 Qt 测试中的常见做法，仅用于测试文件
#define private public
#include "core/agent/AgentController.h"
#include "core/agent/OpenAILLMClient.h"
#undef private

#include "core/agent/AgentActor.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/base/ModuleBase.h"
#include "core/engine/RunEngine.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

using namespace DeepLux;

class AgentBusyProbeModule : public ModuleBase {
public:
    AgentBusyProbeModule() {
        m_moduleId = QStringLiteral("com.deeplux.test.agentbusy");
        m_name = QStringLiteral("AgentBusy");
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class UndoCounterCommand : public QUndoCommand {
public:
    explicit UndoCounterCommand(int* undoCount) : m_undoCount(undoCount) {}
    void undo() override {
        ++*m_undoCount;
    }
    void redo() override {}

private:
    int* m_undoCount;
};

// ========== Mock LLM Client ==========

class MockLLMClient : public ILLMClient {
    Q_OBJECT
public:
    explicit MockLLMClient(QObject* parent = nullptr) : ILLMClient(parent) {}

    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}
    bool supportsImageInput() const override {
        return imageInputSupported;
    }

    AgentConversation lastCtx;
    QList<ToolDefinition> lastTools;
    bool imageInputSupported = true;
    int requestCount = 0;

    void sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>& tools) override {
        lastCtx = ctx;
        lastTools = tools;
        ++requestCount;
    }

    void emitResponse(const AgentResponse& resp) {
        emit responseReceived(resp);
        QCoreApplication::processEvents();
    }

    void emitError(const QString& error) {
        emit errorOccurred(error);
        QCoreApplication::processEvents();
    }

    // 模拟 OpenAILLMClient 的最终失败链路：先分类、后 errorOccurred
    void emitClassifiedError(const LLMErrorInfo& info) {
        emit errorOccurredClassified(info);
        emit errorOccurred(info.message);
        QCoreApplication::processEvents();
    }
};

// ========== Test Suite ==========

class TestAgentController : public QObject {
    Q_OBJECT

private:
    MockLLMClient* m_mock = nullptr;
    ILLMClient* m_oldClient = nullptr;

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
        AgentController::instance().initialize();
    }

    void cleanupTestCase() {
        AgentController::instance().shutdown();
    }

    void init() {
        // 每个测试前彻底重置状态（状态机 + 对话历史 + client）
        AgentController::instance().clearConversation();
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        // 强制回到 Idle（前一个测试可能留下非 Idle 状态）
        AgentController::instance().transitionTo(AgentController::AgentState::Idle);
        // 上下文治理：每例恢复默认上限，避免上例的测试覆盖值泄漏
        AgentController::instance().setContextLimitsForTest(128000, 6000, 24, 24);
        AgentController::instance().setTurnDeadlineMsForTest(-1);
        AgentController::instance().setTurnDeadlineConfigForTest(-1);

        m_mock = new MockLLMClient(&AgentController::instance());
        m_oldClient = AgentController::instance().llmClient();
        AgentController::instance().setLLMClient(m_mock);
    }

    void cleanup() {
        AgentController::instance().setLLMClient(m_oldClient);
        delete m_mock;
        m_mock = nullptr;
    }

    // ---------- 状态机基础 ----------

    void testInitialStateIsIdle() {
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testDestroyedLLMClientIsCleared() {
        AgentController controller;
        {
            MockLLMClient client;
            controller.setLLMClient(&client);
            QCOMPARE(controller.llmClient(), static_cast<ILLMClient*>(&client));
        }
        QVERIFY(controller.llmClient() == nullptr);
        controller.setLLMClient(m_mock);
        QCOMPARE(controller.llmClient(), static_cast<ILLMClient*>(m_mock));
        controller.setLLMClient(nullptr);
    }

    void testSendMessageTransitionsToThinking() {
        AgentController::instance().sendUserMessage("Hello");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->lastCtx.messages.size(), 1);
        QCOMPARE(m_mock->lastCtx.messages.first().role, QString("user"));
    }

    void testPlainTextResponseReturnsToIdle() {
        AgentController::instance().sendUserMessage("Hello");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        AgentResponse resp;
        resp.success = true;
        resp.content = "Hi there!";
        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testErrorReturnsToIdle() {
        AgentController::instance().sendUserMessage("Hello");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        m_mock->emitError("Network timeout");

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testSystemPromptUpdateAffectsNextRequest() {
        AgentController& controller = AgentController::instance();
        const QString originalPrompt = controller.systemPrompt();
        controller.setSystemPrompt(QStringLiteral("Use the current calibration policy."));

        controller.sendUserMessage(QStringLiteral("Check the project."));

        QVERIFY(m_mock->lastCtx.systemPrompt.startsWith(QStringLiteral("Use the current calibration policy.")));
        controller.setSystemPrompt(originalPrompt);
    }

    void testImageAttachmentReachesModelPayload() {
        QPixmap image(2, 2);
        image.fill(Qt::red);

        AgentController::instance().sendUserMessageWithImages(QStringLiteral("Inspect this image."), {image});

        QCOMPARE(m_mock->lastCtx.messages.size(), 1);
        const AgentMessage& message = m_mock->lastCtx.messages.constFirst();
        QCOMPARE(message.role, QStringLiteral("user"));
        QCOMPARE(message.content, QStringLiteral("Inspect this image."));
        QCOMPARE(message.images.size(), 1);
        QVERIFY(!message.images.constFirst().data.isEmpty());
        QCOMPARE(message.images.constFirst().mimeType, QStringLiteral("image/png"));

        const QJsonArray messages = m_mock->lastCtx.toOpenAIMessages();
        const QJsonArray content = messages.last().toObject().value(QStringLiteral("content")).toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content.last().toObject().value(QStringLiteral("type")).toString(), QStringLiteral("image_url"));
        QVERIFY(content.last()
                    .toObject()
                    .value(QStringLiteral("image_url"))
                    .toObject()
                    .value(QStringLiteral("url"))
                    .toString()
                    .startsWith(QStringLiteral("data:image/png;base64,")));
    }

    void testInvalidImageAttachmentFailsClosed() {
        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);

        AgentController::instance().sendUserMessageWithImages(QString(), {QPixmap()});

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QVERIFY(m_mock->lastCtx.messages.isEmpty());
    }

    void testImageAttachmentCountIsBounded() {
        QList<QPixmap> images;
        for (int i = 0; i < 5; ++i) {
            QPixmap image(1, 1);
            image.fill(Qt::red);
            images.append(image);
        }
        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);

        AgentController::instance().sendUserMessageWithImages(QStringLiteral("Too many"), images);

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QVERIFY(m_mock->lastCtx.messages.isEmpty());
    }

    void testImageAttachmentRequiresModelCapability() {
        m_mock->imageInputSupported = false;
        QPixmap image(1, 1);
        image.fill(Qt::red);
        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);

        AgentController::instance().sendUserMessageWithImages(QStringLiteral("Inspect"), {image});

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QVERIFY(m_mock->lastCtx.messages.isEmpty());
    }

    void testOpenAIRequestDefaultsToCompatiblePayload() {
        OpenAILLMClient client;
        AgentConversation context;
        AgentMessage message;
        message.role = QStringLiteral("user");
        message.content = QStringLiteral("Hello");
        context.messages.append(message);

        const QJsonObject defaultPayload = client.buildRequestBody(context, {});
        QVERIFY(!defaultPayload.contains(QStringLiteral("thinking")));
        QVERIFY(!defaultPayload.contains(QStringLiteral("reasoning_effort")));

        client.setThinkingEnabled(true);
        client.setReasoningEffort(QStringLiteral("high"));
        const QJsonObject thinkingPayload = client.buildRequestBody(context, {});
        QCOMPARE(thinkingPayload.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("enabled"));
        QCOMPARE(thinkingPayload.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("high"));
    }

    void testEndpointNormalizationMatchesConnectionTest() {
        QCOMPARE(OpenAILLMClient::normalizeEndpoint(QStringLiteral("api.example.test")),
                 QStringLiteral("https://api.example.test/v1/chat/completions"));
        QCOMPARE(OpenAILLMClient::normalizeEndpoint(QStringLiteral("http://localhost:8080/custom")),
                 QStringLiteral("http://localhost:8080/custom"));
    }

    // ---------- Advisor 模式确认流程 ----------

    void testAdvisorToolCallsTransitionToConfirming() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage("Add module");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy confirmSpy(&AgentController::instance(), &AgentController::toolsPendingConfirmation);
        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);

        AgentResponse resp;
        resp.success = true;
        resp.content = "I'll add a module.";
        QJsonObject tc;
        tc["id"] = "call_1";
        tc["type"] = "function";
        tc["name"] = "add_module";
        tc["arguments"] = QJsonObject{{"plugin", "GrabImage"}};
        resp.toolCalls.append(tc);

        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
        QCOMPARE(confirmSpy.count(), 1);
        QCOMPARE(responseSpy.count(), 1);
        QVERIFY(!AgentController::instance().pendingToolCalls().isEmpty());
    }

    void testConfirmToolsTransitionsToExecutingAndThenThinking() {
        // 设置到 Confirming 状态
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage("Create project");

        AgentResponse resp;
        resp.success = true;
        resp.content = "Creating...";
        QJsonObject tc;
        tc["id"] = "call_1";
        tc["type"] = "function";
        tc["name"] = "create_project";
        tc["arguments"] = QJsonObject{{"name", "AgentProject"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);

        // 确认 — QueuedConnection 需要 processEvents 处理入队的 lambda
        // 单次 processEvents 可能不足以处理完所有内部事件，需多次调用
        AgentController::instance().confirmPendingTools();
        for (int i = 0; i < 10; ++i)
            QCoreApplication::processEvents();

        // extendAgentLoop 执行工具后会发送新的 LLM 请求（mock 不响应）
        // 状态应为 Thinking（等待第二轮 LLM 响应）
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        QVERIFY(AgentController::instance().pendingToolCalls().isEmpty());
    }

    void testRejectToolsReturnsToIdle() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage("Add module");

        AgentResponse resp;
        resp.success = true;
        resp.content = "Adding...";
        QJsonObject tc;
        tc["id"] = "call_1";
        tc["type"] = "function";
        tc["name"] = "add_module";
        tc["arguments"] = QJsonObject{{"plugin", "GrabImage"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);

        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);

        // QueuedConnection 需要 processEvents 处理入队的 lambda
        AgentController::instance().rejectPendingTools();
        for (int i = 0; i < 10; ++i)
            QCoreApplication::processEvents();

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(responseSpy.count(), 1);
        QVERIFY(AgentController::instance().pendingToolCalls().isEmpty());
    }

    void testMultipleToolResultsRemainPairedInNextRequest() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        controller.sendUserMessage(QStringLiteral("Inspect workflow"));
        AgentResponse response;
        response.success = true;
        response.toolCalls =
            QJsonArray{QJsonObject{{"id", "call-a"}, {"type", "function"}, {"name", "get_available_plugins"}},
                       QJsonObject{{"id", "call-b"}, {"type", "function"}, {"name", "get_available_plugins"}}};
        m_mock->emitResponse(response);
        QTRY_COMPARE(m_mock->requestCount, 2);
        const QJsonArray messages = m_mock->lastCtx.toOpenAIMessages();
        int assistantIndex = -1;
        for (int i = 0; i < messages.size(); ++i) {
            if (messages.at(i).toObject().value("tool_calls").toArray().size() == 2)
                assistantIndex = i;
        }
        QVERIFY(assistantIndex >= 0);
        QVERIFY(assistantIndex + 2 < messages.size());
        QCOMPARE(messages.at(assistantIndex + 1).toObject().value("tool_call_id").toString(), QStringLiteral("call-a"));
        QCOMPARE(messages.at(assistantIndex + 2).toObject().value("tool_call_id").toString(), QStringLiteral("call-b"));
        QCOMPARE(messages.at(assistantIndex + 2).toObject().value("role").toString(), QStringLiteral("tool"));
    }

    void testRejectedChatToolsKeepResultsInNextRequest() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("Modify workflow"));
        AgentResponse response;
        response.success = true;
        response.toolCalls =
            QJsonArray{QJsonObject{{"id", "reject-a"}, {"type", "function"}, {"name", "add_module"}},
                       QJsonObject{{"id", "reject-b"}, {"type", "function"}, {"name", "remove_module"}}};
        m_mock->emitResponse(response);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        controller.rejectPendingTools();
        controller.sendUserMessage(QStringLiteral("Continue"));
        const QJsonArray messages = m_mock->lastCtx.toOpenAIMessages();
        int assistantIndex = -1;
        for (int i = 0; i < messages.size(); ++i)
            if (messages.at(i).toObject().value("tool_calls").toArray().size() == 2)
                assistantIndex = i;
        QVERIFY(assistantIndex >= 0);
        QCOMPARE(messages.at(assistantIndex + 1).toObject().value("tool_call_id").toString(),
                 QStringLiteral("reject-a"));
        QCOMPARE(messages.at(assistantIndex + 2).toObject().value("tool_call_id").toString(),
                 QStringLiteral("reject-b"));
        QVERIFY(messages.at(assistantIndex + 1).toObject().value("content").toString().contains("USER_REJECTED"));
    }

    void testBridgeRejectionDoesNotCreateToolHistory() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const int before = controller.conversationHistorySize();
        const QJsonObject result = controller.handleToolCall(QStringLiteral("add_module"), {{"plugin", "GrabImage"}});
        QCOMPARE(result.value("status").toString(), QStringLiteral("pending_confirmation"));
        controller.rejectPendingTools();
        QCOMPARE(controller.conversationHistorySize(), before);
    }

    void testAbortQueuedConfirmationKeepsToolResult() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("Modify workflow"));
        AgentResponse response;
        response.success = true;
        response.toolCalls = QJsonArray{QJsonObject{{"id", "cancel-a"}, {"type", "function"}, {"name", "add_module"}}};
        m_mock->emitResponse(response);
        controller.confirmPendingTools();
        controller.abortCurrentTurn();
        QCoreApplication::processEvents();
        // B1：中止后不得续跑，模型请求计数保持为首次请求
        QCOMPARE(m_mock->requestCount, 1);
        controller.sendUserMessage(QStringLiteral("Continue"));
        bool found = false;
        for (const QJsonValue& value : m_mock->lastCtx.toOpenAIMessages()) {
            const QJsonObject message = value.toObject();
            if (message.value("tool_call_id").toString() == QLatin1String("cancel-a"))
                found = message.value("content").toString().contains("USER_CANCELLED");
        }
        QVERIFY(found);
    }

    void testPermissionDowngradeBeforeConfirmationDeniesExecution() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("Create project"));
        AgentResponse response;
        response.success = true;
        response.toolCalls = QJsonArray{QJsonObject{{"id", "denied-a"},
                                                    {"type", "function"},
                                                    {"name", "create_project"},
                                                    {"arguments", QJsonObject{{"name", "MustNotCreate"}}}}};
        m_mock->emitResponse(response);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QSignalSpy actions(&controller, &AgentController::agentActionCompleted);
        controller.confirmPendingTools();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCoreApplication::processEvents();
        QCOMPARE(actions.count(), 0);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        controller.sendUserMessage(QStringLiteral("Continue"));
        const QJsonArray messages = m_mock->lastCtx.toOpenAIMessages();
        bool found = false;
        for (const QJsonValue& value : messages) {
            const QJsonObject message = value.toObject();
            if (message.value("tool_call_id").toString() == QLatin1String("denied-a"))
                found = message.value("content").toString().contains("PERMISSION_DENIED");
        }
        QVERIFY(found);
    }

    // ---------- 并发防护 ----------

    void testBusyStateRejectsNewMessage() {
        AgentController::instance().sendUserMessage("First");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);
        AgentController::instance().sendUserMessage("Second");

        QCOMPARE(responseSpy.count(), 1);
        QString reply = responseSpy.takeFirst()[0].toString();
        QVERIFY(reply.contains("busy"));
    }

    void testUndoRejectedWhileFlowBusy() {
        AgentController& controller = AgentController::instance();
        controller.m_actor->undoStack()->clear();
        int undoCount = 0;
        controller.m_actor->undoStack()->push(new UndoCounterCommand(&undoCount));

        RunEngine& engine = RunEngine::instance();
        auto* module = new AgentBusyProbeModule();
        module->initialize();
        engine.addModule(module);
        engine.start();

        QVERIFY(engine.isBusy());
        QVERIFY(!controller.undoLastAgentAction());
        QCOMPARE(undoCount, 0);

        engine.stop();
        engine.clearModules();
        QVERIFY(controller.undoLastAgentAction());
        QCOMPARE(undoCount, 1);
        delete module;
    }

    // ---------- 错误恢复 ----------

    void testErrorInConfirmingCleansPendingTools() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage("Add module");

        AgentResponse resp;
        resp.success = true;
        resp.content = "Adding...";
        QJsonObject tc;
        tc["id"] = "call_1";
        tc["type"] = "function";
        tc["name"] = "add_module";
        tc["arguments"] = QJsonObject{{"plugin", "GrabImage"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
        QVERIFY(!AgentController::instance().pendingToolCalls().isEmpty());

        // 模拟用户在确认前发生网络错误（通过重新发送消息触发错误）
        // 实际上我们需要测试 onLLMError 在 Confirming 状态下的行为
        // 由于 onLLMError 只在 LLM 响应时触发，我们直接调用它
        m_mock->emitError("Connection lost");

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QVERIFY(AgentController::instance().pendingToolCalls().isEmpty());
    }

    // ---------- History 截断 ----------

    void testTrimHistoryPreservesMessagePairs() {
        AgentController::instance().clearConversation();

        // 构造足够大的历史以超过 token-based 裁剪阈值。
        // 格式: user, assistant, tool, user, assistant, tool, ...
        for (int round = 0; round < 7; ++round) {
            AgentMessage userMsg;
            userMsg.role = "user";
            userMsg.content = QString("msg_%1 ").arg(round) + QString(100000, QLatin1Char('u'));
            AgentController::instance().m_conversationHistory.append(userMsg);

            AgentMessage assistantMsg;
            assistantMsg.role = "assistant";
            assistantMsg.content = QString("resp_%1 ").arg(round) + QString(100000, QLatin1Char('a'));
            if (round % 2 == 0) {
                QJsonArray tcs;
                QJsonObject tc;
                tc["id"] = QString("call_%1").arg(round);
                tc["type"] = "function";
                tc["name"] = "get_flow_state";
                tcs.append(tc);
                assistantMsg.toolCalls = tcs;
            }
            AgentController::instance().m_conversationHistory.append(assistantMsg);

            if (round % 2 == 0) {
                AgentMessage toolMsg;
                toolMsg.role = "tool";
                toolMsg.toolCallId = QString("call_%1").arg(round);
                toolMsg.content = "{\"status\":\"ok\"}";
                AgentController::instance().m_conversationHistory.append(toolMsg);
            }
        }

        int sizeBefore = AgentController::instance().m_conversationHistory.size();
        QVERIFY(sizeBefore > 15);

        AgentController::instance().trimHistoryIfNeeded();

        int sizeAfter = AgentController::instance().conversationHistorySize();
        QVERIFY(sizeAfter < sizeBefore);

        // 验证第一条消息是 user（完整轮次的起点）
        QCOMPARE(AgentController::instance().m_conversationHistory.first().role, QString("user"));

        // 验证没有孤立的 tool message（每个 tool 前面都有带 toolCalls 的 assistant）
        for (int i = 0; i < AgentController::instance().m_conversationHistory.size(); ++i) {
            if (AgentController::instance().m_conversationHistory[i].role == "tool") {
                bool foundMatchingAssistant = false;
                for (int j = i - 1; j >= 0; --j) {
                    if (AgentController::instance().m_conversationHistory[j].role == "assistant") {
                        QJsonArray tcs = AgentController::instance().m_conversationHistory[j].toolCalls;
                        for (const QJsonValue& v : tcs) {
                            if (v.toObject()["id"].toString() ==
                                AgentController::instance().m_conversationHistory[i].toolCallId) {
                                foundMatchingAssistant = true;
                                break;
                            }
                        }
                        break; // 只检查最近的 assistant
                    }
                }
                QVERIFY2(foundMatchingAssistant,
                         qPrintable(QString("Tool message at index %1 has no matching assistant tool_call").arg(i)));
            }
        }
    }

    void testTrimHistoryEnforcesHardMessageLimit() {
        AgentController::instance().clearConversation();
        for (int i = 0; i < 40; ++i) {
            AgentMessage user;
            user.role = QStringLiteral("user");
            user.content = QStringLiteral("user-%1").arg(i);
            AgentController::instance().m_conversationHistory.append(user);
            AgentMessage assistant;
            assistant.role = QStringLiteral("assistant");
            assistant.content = QStringLiteral("assistant-%1").arg(i);
            AgentController::instance().m_conversationHistory.append(assistant);
        }

        AgentController::instance().trimHistoryIfNeeded();

        QVERIFY(AgentController::instance().m_conversationHistory.size() <= 64);
        QCOMPARE(AgentController::instance().m_conversationHistory.first().role, QStringLiteral("user"));
        QCOMPARE(AgentController::instance().m_conversationHistory.last().content, QStringLiteral("assistant-39"));
    }

    void testTrimHistoryEnforcesHardLimitWithinSingleUserTurn() {
        AgentController::instance().clearConversation();
        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("single request");
        AgentController::instance().m_conversationHistory.append(user);
        for (int i = 0; i < 35; ++i) {
            AgentMessage assistant;
            assistant.role = QStringLiteral("assistant");
            assistant.toolCalls = QJsonArray{QJsonObject{{"id", QStringLiteral("call-%1").arg(i)}}};
            AgentController::instance().m_conversationHistory.append(assistant);
            AgentMessage tool;
            tool.role = QStringLiteral("tool");
            tool.toolCallId = QStringLiteral("call-%1").arg(i);
            AgentController::instance().m_conversationHistory.append(tool);
        }

        AgentController::instance().trimHistoryIfNeeded();

        QVERIFY(AgentController::instance().m_conversationHistory.size() <= 64);
        QCOMPARE(AgentController::instance().m_conversationHistory.first().role, QStringLiteral("user"));
        QVERIFY(AgentController::instance().m_conversationHistory.size() > 1);
        QCOMPARE(AgentController::instance().m_conversationHistory[1].role, QStringLiteral("assistant"));
    }

    void testTrimHistoryAccountsForImageBytes() {
        AgentController::instance().clearConversation();
        AgentMessage imageTurn;
        imageTurn.role = QStringLiteral("user");
        imageTurn.content = QStringLiteral("old image");
        imageTurn.images.append({QByteArray(2 * 1024 * 1024, 'x'), QStringLiteral("image/png"), QString()});
        AgentController::instance().m_conversationHistory.append(imageTurn);
        AgentMessage response;
        response.role = QStringLiteral("assistant");
        response.content = QStringLiteral("old response");
        AgentController::instance().m_conversationHistory.append(response);
        AgentMessage latest;
        latest.role = QStringLiteral("user");
        latest.content = QStringLiteral("latest");
        AgentController::instance().m_conversationHistory.append(latest);

        AgentController::instance().trimHistoryIfNeeded();

        QCOMPARE(AgentController::instance().m_conversationHistory.size(), 1);
        QCOMPARE(AgentController::instance().m_conversationHistory.first().content, QStringLiteral("latest"));
    }

    // ---------- B3：上下文保护 / 预算失败关闭 ----------

    void testContextBudgetExceededFailsClosed() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);

        controller.sendUserMessage(QString(200000, QLatin1Char('x')));

        QCOMPARE(m_mock->requestCount, 0);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(controller.m_conversationHistory.size(), 1); // 保留可诊断的用户请求
        QVERIFY(!errorSpy.isEmpty());
        QVERIFY2(errorSpy.last().at(0).toString().contains(QStringLiteral("context_budget_exceeded")),
                 qPrintable(errorSpy.last().at(0).toString()));
    }

    void testSystemPromptOverBudgetFailsClosed() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        const QString original = controller.systemPrompt();
        controller.setSystemPrompt(QString(200000, QLatin1Char('s')));

        controller.sendUserMessage(QStringLiteral("hello"));

        const int requests = m_mock->requestCount;
        const AgentController::AgentState finalState = controller.state();
        controller.setSystemPrompt(original); // 先恢复，避免失败提前 return 污染后续用例
        QCOMPARE(requests, 0);
        QCOMPARE(finalState, AgentController::AgentState::Idle);
    }

    void testImageAttachmentOverBudgetFailsClosed() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);

        QImage noise(700, 700, QImage::Format_RGB32);
        quint32 seed = 0x12345678u;
        for (int y = 0; y < noise.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(noise.scanLine(y));
            for (int x = 0; x < noise.width(); ++x) {
                seed = seed * 1664525u + 1013904223u;
                const quint32 value = seed;
                line[x] = qRgb(value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff);
            }
        }
        controller.sendUserMessageWithImages(QStringLiteral("inspect"), {QPixmap::fromImage(noise)});

        QCOMPARE(m_mock->requestCount, 0);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    void testInFlightToolCallIsProtected() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        controller.m_conversationHistory.clear();

        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("current request");
        controller.m_conversationHistory.append(user);

        AgentMessage inFlight;
        inFlight.role = QStringLiteral("assistant");
        inFlight.toolCalls = QJsonArray{QJsonObject{{"id", "pending-call"}}};
        inFlight.content = QString(200000, QLatin1Char('a'));
        controller.m_conversationHistory.append(inFlight);

        controller.trimHistoryIfNeeded();

        QVERIFY(!controller.m_conversationHistory.isEmpty());
        QCOMPARE(controller.m_conversationHistory.constFirst().content, QStringLiteral("current request"));
        const QJsonArray expectedGroup{QJsonObject{{"id", "pending-call"}}};
        QVERIFY(controller.m_conversationHistory.constLast().toolCalls == expectedGroup);
    }

    void testAutoInjectedImageDoesNotCreateTaskBoundary() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        controller.m_conversationHistory.clear();

        AgentMessage original;
        original.role = QStringLiteral("user");
        original.content = QStringLiteral("original task");
        controller.m_conversationHistory.append(original);

        AgentMessage oldGroup;
        oldGroup.role = QStringLiteral("assistant");
        oldGroup.toolCalls = QJsonArray{QJsonObject{{"id", "old-call"}}};
        controller.m_conversationHistory.append(oldGroup);
        AgentMessage oldResult;
        oldResult.role = QStringLiteral("tool");
        oldResult.toolCallId = QStringLiteral("old-call");
        oldResult.content = QString(200000, QLatin1Char('o'));
        controller.m_conversationHistory.append(oldResult);

        AgentMessage recentGroup;
        recentGroup.role = QStringLiteral("assistant");
        recentGroup.toolCalls = QJsonArray{QJsonObject{{"id", "recent-call"}}};
        controller.m_conversationHistory.append(recentGroup);
        AgentMessage recentResult;
        recentResult.role = QStringLiteral("tool");
        recentResult.toolCallId = QStringLiteral("recent-call");
        recentResult.content = QStringLiteral("recent result");
        controller.m_conversationHistory.append(recentResult);

        AgentMessage injected;
        injected.role = QStringLiteral("user");
        injected.content = QStringLiteral("Images produced by the tool calls above");
        injected.internalAutoInjected = true;
        controller.m_conversationHistory.append(injected);

        controller.trimHistoryIfNeeded();

        QCOMPARE(controller.m_conversationHistory.constFirst().role, QStringLiteral("user"));
        QCOMPARE(controller.m_conversationHistory.constFirst().content, QStringLiteral("original task"));
        bool oldGone = true;
        bool recentKept = false;
        bool injectedKept = false;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("old-call"))
                oldGone = false;
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("recent-call"))
                recentKept = true;
            if (message.internalAutoInjected)
                injectedKept = true;
        }
        QVERIFY2(oldGone, "the older complete tool group must be trimmed first");
        QVERIFY2(recentKept, "the latest complete tool group is protected");
        QVERIFY2(injectedKept, "the auto-injected image message belongs to the protected recent group");
    }

    // B3：保护集合自身超过消息数硬上限时确定性停止（不放宽到 65+ 条）
    void testProtectedMessageLimitFailsClosed() {
        AgentController& controller = AgentController::instance();
        controller.m_conversationHistory.clear();

        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("one request with many tool results");
        controller.m_conversationHistory.append(user);

        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        QJsonArray calls;
        for (int i = 0; i < 64; ++i)
            calls.append(QJsonObject{{"id", QStringLiteral("bulk-%1").arg(i)}});
        assistant.toolCalls = calls;
        controller.m_conversationHistory.append(assistant);
        for (int i = 0; i < 64; ++i) {
            AgentMessage tool;
            tool.role = QStringLiteral("tool");
            tool.toolCallId = QStringLiteral("bulk-%1").arg(i);
            tool.content = QStringLiteral("{}");
            controller.m_conversationHistory.append(tool);
        }
        QVERIFY(controller.m_conversationHistory.size() > 64);

        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        QVERIFY(!controller.sendPreparedContext());
        QCOMPARE(m_mock->requestCount, 0);
        QVERIFY(!errorSpy.isEmpty());
        QVERIFY(errorSpy.last().at(0).toString().contains(QStringLiteral("context_budget_exceeded")));
    }

    void testInvalidToolPairingBlocksSend() {
        AgentController& controller = AgentController::instance();
        controller.m_conversationHistory.clear();
        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("request");
        controller.m_conversationHistory.append(user);
        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        assistant.toolCalls = QJsonArray{QJsonObject{{"id", "missing-result"}}};
        controller.m_conversationHistory.append(assistant);

        QString error;
        QVERIFY(!controller.historyPairsValid(&error));
        QVERIFY2(error.contains(QStringLiteral("missing")), qPrintable(error));

        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        QVERIFY(!controller.sendPreparedContext());
        QCOMPARE(m_mock->requestCount, 0);
        QVERIFY(!errorSpy.isEmpty());
        QVERIFY(errorSpy.last().at(0).toString().contains(QStringLiteral("context_invalid_history")));
    }

    // ---------- 最大轮数限制 ----------

    void testAutopilotStopsAtTurnLimit() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        AgentController::instance().sendUserMessage("Run");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);
        for (int turn = 0; turn < 21; ++turn) {
            AgentResponse resp;
            resp.success = true;
            resp.content = QString("Turn %1").arg(turn);
            QJsonObject tc;
            tc["id"] = QString("call_%1").arg(turn);
            tc["type"] = "function";
            tc["name"] = "get_available_plugins";
            resp.toolCalls.append(tc);

            m_mock->emitResponse(resp);

            if (turn < 20) {
                QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
            }
        }
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy.takeFirst()[0].toString().contains("轮数上限"));
    }

    void testAutopilotRejectsTooManyToolCallsInOneTurn() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        AgentController::instance().sendUserMessage("Run many tools");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);
        AgentResponse resp;
        resp.success = true;
        resp.content = "Too many tools";
        for (int i = 0; i < 11; ++i) {
            QJsonObject tc;
            tc["id"] = QString("call_%1").arg(i);
            tc["type"] = "function";
            tc["name"] = "get_flow_state";
            resp.toolCalls.append(tc);
        }

        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy.takeFirst()[0].toString().contains("工具数量上限"));
    }

    // ---------- Observer 模式 ----------

    void testObserverModeShowsToolsButDoesNotExecute() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        AgentController::instance().sendUserMessage("Show me tools");

        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);

        AgentResponse resp;
        resp.success = true;
        resp.content = "Here are some tools.";
        QJsonObject tc;
        tc["id"] = "call_1";
        tc["type"] = "function";
        tc["name"] = "add_module";
        tc["arguments"] = QJsonObject{{"plugin", "GrabImage"}};
        resp.toolCalls.append(tc);

        m_mock->emitResponse(resp);

        // Observer 模式：直接回到 Idle，不进入 Confirming
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(responseSpy.count(), 1);
    }

    // B2：Observer 纯只读批次正常执行并续跑（查询能力不被降权破坏）
    void testObserverReadOnlyBatchExecutesAndContinues() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        controller.sendUserMessage(QStringLiteral("list plugins"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("checking");
        resp.toolCalls.append(QJsonObject{
            {"id", "read-1"}, {"type", "function"}, {"name", "get_available_plugins"}, {"arguments", QJsonObject()}});
        m_mock->emitResponse(resp);

        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->requestCount, 2);
        bool readOk = false;
        for (const AgentMessage& message : m_mock->lastCtx.messages) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("read-1"))
                readOk = !message.content.contains(QStringLiteral("\"error\""));
        }
        QVERIFY(readOk);

        AgentResponse done;
        done.success = true;
        done.content = QStringLiteral("done");
        m_mock->emitResponse(done);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    // B2：Observer 混合批次逐项判定——只读执行、写入拒绝，且不因含读工具放行写工具
    void testObserverMixedBatchExecutesReadsAndDeniesWrites() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().newProject();
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        controller.sendUserMessage(QStringLiteral("read and write"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("working");
        resp.toolCalls.append(QJsonObject{
            {"id", "read-1"}, {"type", "function"}, {"name", "get_available_plugins"}, {"arguments", QJsonObject()}});
        resp.toolCalls.append(QJsonObject{{"id", "write-1"},
                                          {"type", "function"},
                                          {"name", "add_module"},
                                          {"arguments", QJsonObject{{"plugin", "GrabImage"}}}});
        m_mock->emitResponse(resp);

        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(m_mock->requestCount, 1); // 拒绝项结束本轮，不续跑
        QCOMPARE(project->modules().size(), 0);

        bool readSeen = false;
        bool writeDenied = false;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role != QLatin1String("tool"))
                continue;
            if (message.toolCallId == QLatin1String("read-1"))
                readSeen = !message.content.contains(QStringLiteral("PERMISSION_DENIED"));
            if (message.toolCallId == QLatin1String("write-1"))
                writeDenied = message.content.contains(QStringLiteral("PERMISSION_DENIED"));
        }
        QVERIFY(readSeen);
        QVERIFY(writeDenied);
    }

    // B2：确认前切换工程（含同 ID 模块）——旧确认失效，新工程零修改
    void testConfirmationExpiresOnProjectSwitch() {
        AgentController& controller = AgentController::instance();
        Project* first = ProjectManager::instance().newProject();
        QVERIFY(first != nullptr);
        ModuleInstance sameId;
        sameId.id = QStringLiteral("same_id");
        sameId.moduleId = QStringLiteral("GrabImage");
        first->addModule(sameId);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("remove it"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(QJsonObject{{"id", "remove-old"},
                                          {"type", "function"},
                                          {"name", "remove_module"},
                                          {"arguments", QJsonObject{{"instanceId", "same_id"}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        Project* second = ProjectManager::instance().newProject();
        QVERIFY(second != nullptr);
        ModuleInstance sameIdInSecond;
        sameIdInSecond.id = QStringLiteral("same_id");
        sameIdInSecond.moduleId = QStringLiteral("GrabImage");
        second->addModule(sameIdInSecond);

        // 工程切换使旧确认立即失效，且不会执行旧批次
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY(controller.pendingToolCalls().isEmpty());
        controller.confirmPendingTools();
        QCoreApplication::processEvents();
        QCOMPARE(second->modules().size(), 1);

        bool expired = false;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("remove-old"))
                expired = message.content.contains(QStringLiteral("confirmation_expired"));
        }
        QVERIFY(expired);
    }

    // B2：确认入队后切工程——排队 lambda 复核并拒绝
    void testQueuedConfirmationRejectedAfterProjectSwitch() {
        AgentController& controller = AgentController::instance();
        QVERIFY(ProjectManager::instance().newProject() != nullptr);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("add"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(
            QJsonObject{{"id", "add-old"},
                        {"type", "function"},
                        {"name", "add_module"},
                        {"arguments", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "must_not_add"}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        controller.confirmPendingTools(); // 入队
        Project* second = ProjectManager::instance().newProject();
        QVERIFY(second != nullptr);
        QCoreApplication::processEvents();

        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(second->modules().size(), 0);
    }

    // B2：外部参数修改使确认失效，确认不得覆盖外部值
    void testExternalParamChangeExpiresConfirmation() {
        AgentController& controller = AgentController::instance();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance inst;
        inst.id = QStringLiteral("mc1");
        inst.moduleId = QStringLiteral("MeasureCircle");
        project->addModule(inst);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("set radius"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(
            QJsonObject{{"id", "param-old"},
                        {"type", "function"},
                        {"name", "set_param"},
                        {"arguments", QJsonObject{{"instanceId", "mc1"}, {"key", "initialRadius"}, {"value", 222}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        // 用户在界面上改了同一参数
        project->setModuleParam(QStringLiteral("mc1"), QStringLiteral("initialRadius"), 111);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);

        controller.confirmPendingTools();
        QCoreApplication::processEvents();
        QCOMPARE(project->moduleById(QStringLiteral("mc1"))->params.value("initialRadius"), QJsonValue(111));
    }

    // B2：目标删除同样使旧确认失效（不得在删除后的工程上执行旧写操作）
    void testTargetRemovalExpiresConfirmation() {
        AgentController& controller = AgentController::instance();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance inst;
        inst.id = QStringLiteral("target-1");
        inst.moduleId = QStringLiteral("GrabImage");
        project->addModule(inst);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("add another module"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(
            QJsonObject{{"id", "add-after-removal"},
                        {"type", "function"},
                        {"name", "add_module"},
                        {"arguments", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "must_not_add"}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        project->removeModule(QStringLiteral("target-1"));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        controller.confirmPendingTools();
        QCoreApplication::processEvents();
        QCOMPARE(project->modules().size(), 0);
    }

    // B2：Autopilot 下 create_project 成功后保守终止旧批次，让模型读取新工程再规划
    void testProjectSwitchStopsRemainingBatch() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        controller.sendUserMessage(QStringLiteral("create and add"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("working");
        resp.toolCalls.append(QJsonObject{{"id", "create-1"},
                                          {"type", "function"},
                                          {"name", "create_project"},
                                          {"arguments", QJsonObject{{"name", "BatchSwitch"}}}});
        resp.toolCalls.append(
            QJsonObject{{"id", "add-after"},
                        {"type", "function"},
                        {"name", "add_module"},
                        {"arguments", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "stale"}}}});
        m_mock->emitResponse(resp);

        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->requestCount, 2); // project_switched 允许重新规划
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);
        QCOMPARE(project->modules().size(), 0);

        bool addSkipped = false;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("add-after"))
                addSkipped = message.content.contains(QStringLiteral("skipped_after_failure"));
        }
        QVERIFY(addSkipped);
    }

    // B2：合法确认只执行一次（双击不重复执行）
    void testConfirmationExecutesOnlyOnce() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("add one"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(QJsonObject{{"id", "create-once"},
                                          {"type", "function"},
                                          {"name", "create_project"},
                                          {"arguments", QJsonObject{{"name", "OnlyOnce"}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        controller.confirmPendingTools();
        controller.confirmPendingTools(); // 连点
        QCoreApplication::processEvents();

        int results = 0;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("create-once"))
                ++results;
        }
        QCOMPARE(results, 1);
        QCOMPARE(m_mock->requestCount, 2); // 执行后正常续跑一次
    }

    // ---------- 第一迭代：排队执行 / 中止 / 视觉降级 ----------

    void testConfirmDispatchesQueuedExactlyOnce() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage(QStringLiteral("Create project"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("Creating...");
        QJsonObject tc;
        tc["id"] = "call_queued";
        tc["type"] = "function";
        tc["name"] = "create_project";
        tc["arguments"] = QJsonObject{{"name", "QueuedProject"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);

        QSignalSpy logSpy(&AgentController::instance(), &AgentController::actionLogEntryAdded);
        AgentController::instance().confirmPendingTools();
        // 连点保护：第二次确认不得重复排队或重复执行
        AgentController::instance().confirmPendingTools();
        // 排队 tick 尚未处理：状态不得提前回到 Idle（仍为 Confirming/Executing）
        QVERIFY(AgentController::instance().state() != AgentController::AgentState::Idle);

        QTRY_COMPARE_WITH_TIMEOUT(AgentController::instance().state(), AgentController::AgentState::Thinking, 5000);
        QCOMPARE(logSpy.count(), 1); // create_project 只执行一次
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);
    }

    void testAbortWhileThinkingIsIdempotent() {
        AgentController::instance().sendUserMessage(QStringLiteral("Hello"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy abortSpy(&AgentController::instance(), &AgentController::agentTurnAborted);
        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);

        AgentController::instance().abortCurrentTurn();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(abortSpy.count(), 1);
        QCOMPARE(responseSpy.count(), 1);
        QCOMPARE(responseSpy.takeFirst().at(0).toString(), QStringLiteral("Stopped by user."));

        const quint64 generation = AgentController::instance().turnGeneration();
        AgentController::instance().abortCurrentTurn(); // Idle → no-op（幂等）
        QCOMPARE(abortSpy.count(), 1);
        QCOMPARE(AgentController::instance().turnGeneration(), generation);
    }

    void testAbortCancelsQueuedDispatch() {
        ProjectManager::instance().closeProject(); // 隔离：本用例断言"未执行建工程"
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage(QStringLiteral("Create project"));

        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("Creating...");
        QJsonObject tc;
        tc["id"] = "call_cancel";
        tc["type"] = "function";
        tc["name"] = "create_project";
        tc["arguments"] = QJsonObject{{"name", "CancelledProject"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);

        QSignalSpy logSpy(&AgentController::instance(), &AgentController::actionLogEntryAdded);
        AgentController::instance().confirmPendingTools(); // 入队
        AgentController::instance().abortCurrentTurn();    // 在 tick 之前中止
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);

        for (int i = 0; i < 10; ++i)
            QCoreApplication::processEvents();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(logSpy.count(), 0); // 排队任务被中止代号作废，未执行
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);
    }

    void testUserMessageRejectedWhileExecuting() {
        AgentController::instance().transitionTo(AgentController::AgentState::Executing);
        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);

        AgentController::instance().sendUserMessage(QStringLiteral("Second"));

        QCOMPARE(responseSpy.count(), 1);
        QVERIFY(responseSpy.takeFirst().at(0).toString().contains(QStringLiteral("busy")));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Executing);
        AgentController::instance().transitionTo(AgentController::AgentState::Idle);
    }

    void testVisionRejectionRetriesWithoutImagesOnce() {
        m_mock->imageInputSupported = true;
        QPixmap image(8, 8);
        image.fill(Qt::blue);
        AgentController::instance().sendUserMessageWithImages(QStringLiteral("Inspect"), {image});
        QCOMPARE(m_mock->requestCount, 1);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        LLMErrorInfo info;
        info.category = LLMErrorCategory::BadRequest;
        info.httpStatus = 400;
        info.retryable = false;
        info.visionDowngradeHint = true;
        info.message = QStringLiteral("The model may not support image input.");
        m_mock->emitClassifiedError(info);

        // 自动去图重发一次：仍处 Thinking，第二次请求里的图片已被剥离
        QCOMPARE(m_mock->requestCount, 2);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        bool foundImageRequest = false;
        for (const AgentMessage& message : m_mock->lastCtx.messages) {
            if (message.role == QStringLiteral("user") && message.content == QStringLiteral("Inspect")) {
                foundImageRequest = true;
                QVERIFY(message.images.isEmpty());
            }
        }
        QVERIFY(foundImageRequest);

        // 第二次仍被拒：不再重发，正常报错回 Idle（只上报一次）
        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);
        // R6：UI 审计在 llmErrorOccurred 直连槽内能读到本次分类；随后控制器复位
        QString categoryDuringEmit;
        const QMetaObject::Connection categoryConnection =
            connect(&AgentController::instance(), &AgentController::llmErrorOccurred, this, [&categoryDuringEmit]() {
                categoryDuringEmit = AgentController::instance().lastLLMErrorCategory();
            });
        m_mock->emitClassifiedError(info);
        disconnect(categoryConnection);
        QCOMPARE(m_mock->requestCount, 2);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(categoryDuringEmit, QStringLiteral("BadRequest"));
        QVERIFY2(AgentController::instance().lastLLMErrorCategory().isEmpty(),
                 "error category must be reset once the failed turn is closed");
    }

    void testAbortWhileExecutingReturnsToIdleAndRecovers() {
        AgentController::instance().transitionTo(AgentController::AgentState::Executing);
        QSignalSpy abortSpy(&AgentController::instance(), &AgentController::agentTurnAborted);

        AgentController::instance().abortCurrentTurn();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(abortSpy.count(), 1);

        // 状态机未被破坏：中止后可立即开始新一轮
        AgentController::instance().sendUserMessage(QStringLiteral("after abort"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        AgentController::instance().abortCurrentTurn();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testHandleToolCallStripsElapsedMsAndAuditsDuration() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy logSpy(&AgentController::instance(), &AgentController::actionLogEntryAdded);

        const QJsonObject result =
            AgentController::instance().handleToolCall(QStringLiteral("get_available_plugins"), {});
        QVERIFY2(!result.contains(QStringLiteral("_elapsedMs")), "_elapsedMs must not leak into IPC responses");
        QVERIFY(!result.contains("error"));

        QCOMPARE(logSpy.count(), 1);
        const AgentActionLogEntry entry = logSpy.takeFirst().at(0).value<AgentActionLogEntry>();
        QVERIFY2(entry.durationMs >= 0, "audit durationMs must be collected from _elapsedMs");
        QCOMPARE(entry.action, QStringLiteral("get_available_plugins"));
    }

    void testDefaultPromptIncludesClosedLoopRules() {
        const QString prompt = AgentController::defaultSystemPrompt();
        QVERIFY2(prompt.contains(QStringLiteral("validate_workflow")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("wait defaults to true")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("get_node_runtime_outputs")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("capture_viewport_image")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("At most 2 tool images are attached per turn")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("do not repeat the same failing call")), qPrintable(prompt));
    }

    // R1：失败的写工具不得在审计里被标成 undoable（实际没有压入撤销栈）
    void testFailedRemoveModuleIsAuditedAsNotUndoable() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(!controller.handleToolCall(QStringLiteral("create_project"), QJsonObject{{"name", "r1-audit"}})
                     .contains("error"));

        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.transitionTo(AgentController::AgentState::Executing);
        QJsonObject call;
        call["id"] = "call_r1";
        call["type"] = "function";
        call["name"] = "remove_module";
        call["arguments"] = QJsonObject{{"instanceId", "missing_node"}};
        controller.extendAgentLoop(QJsonArray{call});

        QCOMPARE(logSpy.count(), 1);
        const AgentActionLogEntry entry = logSpy.takeFirst().at(0).value<AgentActionLogEntry>();
        QCOMPARE(entry.action, QStringLiteral("remove_module"));
        QCOMPARE(entry.result, QStringLiteral("error"));
        QVERIFY2(!entry.undoable, "failed remove_module must not be audited as undoable");
        controller.abortCurrentTurn();
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    // R6：最近错误类别不得串用——未分类错误与成功响应都必须清空
    void testLastErrorCategoryDoesNotLeakBetweenTurns() {
        AgentController& controller = AgentController::instance();
        controller.sendUserMessage(QStringLiteral("first"));
        LLMErrorInfo auth;
        auth.category = LLMErrorCategory::Auth;
        auth.message = QStringLiteral("auth failed");
        m_mock->emitClassifiedError(auth); // 最终失败（非视觉降级）
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY2(controller.lastLLMErrorCategory().isEmpty(), "error category must reset after a failed turn");

        controller.sendUserMessage(QStringLiteral("second"));
        m_mock->emitError(QStringLiteral("unclassified failure"));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY2(controller.lastLLMErrorCategory().isEmpty(),
                 "unclassified llmErrorOccurred must not inherit a stale category");

        // 分类错误后成功响应同样复位
        controller.sendUserMessage(QStringLiteral("third"));
        m_mock->emitClassifiedError(auth);
        controller.sendUserMessage(QStringLiteral("fourth"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("ok");
        m_mock->emitResponse(resp);
        QVERIFY(controller.lastLLMErrorCategory().isEmpty());
    }

    // R7：未注册工具的早退分支也不得泄漏内部字段 _elapsedMs
    void testUnknownToolEarlyReturnStripsElapsedMs() {
        const QJsonObject result =
            AgentController::instance().handleToolCall(QStringLiteral("not_a_registered_tool"), {});
        QVERIFY(result.contains("error"));
        QVERIFY2(!result.contains(QStringLiteral("_elapsedMs")),
                 "IPC responses must never expose the internal _elapsedMs field");
    }

    // §9.5 第 11 条：执行/等待期间外部 handleToolCall 必须返回 busy
    void testHandleToolCallBusyDuringExecution() {
        AgentController& controller = AgentController::instance();
        controller.transitionTo(AgentController::AgentState::Executing);
        const QJsonObject result = controller.handleToolCall(QStringLiteral("get_flow_state"), {});
        QVERIFY2(result.value("error").toString().contains(QStringLiteral("busy")),
                 qPrintable(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))));
        controller.transitionTo(AgentController::AgentState::Idle);
    }

    // ---------- 第二迭代：上下文治理（设计 §7/§8） ----------

    // I1/I2/I3：工具组原子裁剪——不存在孤立 tool，assistant(tool_calls) 与响应同生共死
    void testToolGroupTrimIsAtomic() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        controller.clearConversation();

        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QString(20000, QLatin1Char('u'));
        controller.m_conversationHistory.append(user);

        for (int i = 0; i < 40; ++i) {
            AgentMessage assistant;
            assistant.role = QStringLiteral("assistant");
            assistant.toolCalls = QJsonArray{QJsonObject{{"id", QStringLiteral("call-%1").arg(i)}}};
            controller.m_conversationHistory.append(assistant);
            AgentMessage tool;
            tool.role = QStringLiteral("tool");
            tool.toolCallId = QStringLiteral("call-%1").arg(i);
            tool.content = QString(2000, QLatin1Char('t'));
            controller.m_conversationHistory.append(tool);
        }
        const int sizeBefore = controller.m_conversationHistory.size();
        controller.trimHistoryIfNeeded();
        QVERIFY(controller.m_conversationHistory.size() < sizeBefore);
        QVERIFY(controller.m_conversationHistory.size() <= 64);
        QCOMPARE(controller.m_conversationHistory.first().role, QStringLiteral("user"));

        // I1/I2：每个 tool 必须紧邻其 assistant，且 assistant 的所有 id 都有响应
        const QList<AgentMessage>& history = controller.m_conversationHistory;
        for (int i = 0; i < history.size(); ++i) {
            if (history.at(i).role == QStringLiteral("tool")) {
                QVERIFY2(i > 0 && history.at(i - 1).role == QStringLiteral("assistant"),
                         "tool message must directly follow its assistant(tool_calls)");
                bool idMatched = false;
                for (const QJsonValue& call : history.at(i - 1).toolCalls) {
                    if (call.toObject().value(QStringLiteral("id")).toString() == history.at(i).toolCallId) {
                        idMatched = true;
                        break;
                    }
                }
                QVERIFY2(idMatched, "tool message must match an id of the previous assistant");
            }
            if (history.at(i).role != QStringLiteral("assistant") || history.at(i).toolCalls.isEmpty())
                continue;
            QSet<QString> ids;
            for (const QJsonValue& call : history.at(i).toolCalls)
                ids.insert(call.toObject().value(QStringLiteral("id")).toString());
            int j = i + 1;
            QSet<QString> found;
            while (j < history.size() && history.at(j).role == QStringLiteral("tool")) {
                found.insert(history.at(j).toolCallId);
                ++j;
            }
            QVERIFY2((ids - found).isEmpty(), "assistant(tool_calls) lost part of its tool responses");
        }
    }

    // I1/I2 防御性修复：孤立 tool 与不完整组在裁剪入口被整组移除
    void testTrimRepairsOrphanAndIncompleteToolGroups() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        controller.clearConversation();

        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("u");
        controller.m_conversationHistory.append(user);

        AgentMessage orphanTool;
        orphanTool.role = QStringLiteral("tool");
        orphanTool.toolCallId = QStringLiteral("orphan");
        controller.m_conversationHistory.append(orphanTool);

        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        assistant.toolCalls = QJsonArray{QJsonObject{{"id", "a"}}, QJsonObject{{"id", "b"}}};
        controller.m_conversationHistory.append(assistant);
        AgentMessage partialTool;
        partialTool.role = QStringLiteral("tool");
        partialTool.toolCallId = QStringLiteral("a");
        controller.m_conversationHistory.append(partialTool);

        controller.trimHistoryIfNeeded();

        QCOMPARE(controller.m_conversationHistory.size(), 1);
        QCOMPARE(controller.m_conversationHistory.first().role, QStringLiteral("user"));
        QVERIFY(controller.m_conversationHistory.first().content == QStringLiteral("u"));
    }

    // §7.4：tool 结果 6k 保首尾截断，中间标注省略字符数（clamp 生效）
    void testToolResultTruncationKeepsHeadAndTail() {
        const QString big = QString(10000, QLatin1Char('x'));
        const QString truncated = AgentController::truncateToolResult(big, 6000);
        QCOMPARE(truncated.size(), 6000 + QStringLiteral("\n…[truncated 4000 chars]…\n").size());
        QVERIFY2(truncated.startsWith(QString(4000, QLatin1Char('x'))), "head must be preserved");
        QVERIFY2(truncated.endsWith(QString(2000, QLatin1Char('x'))), "tail must be preserved");
        QVERIFY2(truncated.contains(QStringLiteral("…[truncated 4000 chars]…")), qPrintable(truncated.mid(3900, 200)));

        // 短结果不截断
        const QString small = QStringLiteral("{\"status\":\"ok\"}");
        QCOMPARE(AgentController::truncateToolResult(small, 6000), small);

        // clamp：maxChars 下限 1000
        const QString clamped = AgentController::truncateToolResult(big, 500);
        QVERIFY2(clamped.contains(QStringLiteral("…[truncated 9000 chars]…")), qPrintable(clamped.mid(600, 120)));
    }

    // §7.1：预算/阈值可配且 clamp；预算内不裁剪
    void testContextBudgetConfigAndNoTrimWithinBudget() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(1, 1, 0, 0);
        QCOMPARE(controller.contextBudgetTokens(), 16000); // clamp 下限
        QCOMPARE(controller.toolResultMaxChars(), 1000);
        QCOMPARE(controller.flowSummaryThreshold(), 1);
        QCOMPARE(controller.flowSummaryPreviewCount(), 1);

        controller.setContextLimitsForTest(2000000, 100000, 5000, 5000);
        QCOMPARE(controller.contextBudgetTokens(), 1000000); // clamp 上限
        QCOMPARE(controller.toolResultMaxChars(), 64000);
        QCOMPARE(controller.flowSummaryThreshold(), 1000);
        QCOMPARE(controller.flowSummaryPreviewCount(), 200);

        // 预算内不裁剪
        controller.setContextLimitsForTest(1000000, 6000, 24, 24);
        controller.clearConversation();
        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("small");
        controller.m_conversationHistory.append(user);
        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        assistant.content = QStringLiteral("reply");
        controller.m_conversationHistory.append(assistant);
        controller.trimHistoryIfNeeded();
        QCOMPARE(controller.m_conversationHistory.size(), 2);
    }

    // §8：buildContext 摘要化阈值两侧 + 大工程 16k 硬上限
    void testBuildContextSummarizesLargeProject() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(128000, 6000, 24, 24);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        for (int i = 0; i < 3; ++i) {
            ModuleInstance instance;
            instance.id = QStringLiteral("inst-%1").arg(i);
            instance.moduleId = QStringLiteral("plugin-%1").arg(i);
            instance.name = instance.id;
            project->addModule(instance);
        }
        QString smallContext = controller.buildContext();
        for (int i = 0; i < 3; ++i)
            QVERIFY2(smallContext.contains(QStringLiteral("inst-%1").arg(i)), qPrintable(smallContext));
        QVERIFY(!smallContext.contains(QStringLiteral("… (+"))); // 小工程不加摘要后缀（提示词里的 (+M) 除外）

        for (int i = 3; i < 25; ++i) {
            ModuleInstance instance;
            instance.id = QStringLiteral("inst-%1").arg(i);
            instance.moduleId = QStringLiteral("plugin-%1").arg(i);
            instance.name = instance.id;
            project->addModule(instance);
        }
        const QString largeContext = controller.buildContext();
        QVERIFY2(largeContext.contains(QStringLiteral("(25)")), qPrintable(largeContext));
        for (int i = 0; i < 24; ++i)
            QVERIFY2(largeContext.contains(QStringLiteral("inst-%1").arg(i)), qPrintable(largeContext));
        QVERIFY2(!largeContext.contains(QStringLiteral("inst-24")), "第 25 个模块名应被摘要省略");
        QVERIFY2(largeContext.contains(QStringLiteral("(+1 more; call get_flow_state for the full list)")),
                 qPrintable(largeContext));
        // Recent Events 仍 ≤5 条（用时间戳行计数；无事件时 0 条）
        static const QRegularExpression eventLine(QStringLiteral("\\[\\d{2}:\\d{2}:\\d{2}\\]"));
        QVERIFY(largeContext.count(eventLine) <= 5);

        // 16k 硬上限：阈值放大到 1000 且 200 个超长模块 → 强制摘要 + 截断标记 + 长度受限
        ProjectManager::instance().closeProject();
        Project* huge = ProjectManager::instance().newProject();
        QVERIFY(huge != nullptr);
        for (int i = 0; i < 200; ++i) {
            ModuleInstance instance;
            instance.id = QStringLiteral("huge-instance-with-a-quite-long-name-%1").arg(i);
            instance.moduleId =
                QStringLiteral("com.deeplux.huge.plugin.with.a.very.long.module.identifier.number-%1").arg(i);
            instance.name = instance.id;
            huge->addModule(instance);
        }
        controller.setContextLimitsForTest(-1, -1, 1000, 200);
        const QString hugeContext = controller.buildContext();
        QVERIFY2(hugeContext.size() <= 16384,
                 qPrintable(QStringLiteral("buildContext size=%1").arg(hugeContext.size())));
        QVERIFY2(hugeContext.contains(QStringLiteral("…[context truncated]")), qPrintable(hugeContext.right(200)));
        ProjectManager::instance().closeProject();
    }

    // ---------- L3：视觉能力模型名启发式 ----------

    void testVisionHeuristicCommonModels() {
        QVERIFY(OpenAILLMClient::heuristicSupportsVision(QStringLiteral("gpt-4o")));
        QVERIFY(OpenAILLMClient::heuristicSupportsVision(QStringLiteral("claude-3-5-sonnet")));
        QVERIFY(OpenAILLMClient::heuristicSupportsVision(QStringLiteral("qwen2.5-vl-72b")));
        QVERIFY(!OpenAILLMClient::heuristicSupportsVision(QStringLiteral("deepseek-reasoner")));
        QVERIFY(!OpenAILLMClient::heuristicSupportsVision(QStringLiteral("deepseek-chat")));
        QVERIFY(!OpenAILLMClient::heuristicSupportsVision(QStringLiteral("o1-mini")));
        QVERIFY(OpenAILLMClient::heuristicSupportsVision(QStringLiteral("unknown-model-x")));
    }

    // ---------- L9：单轮全局 deadline ----------

    void testTurnDeadlineAbortsContinuation() {
        AgentController& controller = AgentController::instance();
        controller.setTurnDeadlineMsForTest(1); // 绕过 clamp，仅测试触发
        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        QSignalSpy abortSpy(&controller, &AgentController::agentTurnAborted);

        controller.sendUserMessage(QStringLiteral("deadline"));
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QThread::msleep(8);
        controller.transitionTo(AgentController::AgentState::Executing);
        const int requestsBefore = m_mock->requestCount;
        controller.extendAgentLoop(
            QJsonArray{QJsonObject{{"id", "call_deadline"}, {"name", "get_flow_state"}, {"arguments", "{}"}}});

        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY2(errorSpy.at(0).at(0).toString().contains(QStringLiteral("单轮总时长")),
                 qPrintable(errorSpy.at(0).at(0).toString()));
        QCOMPARE(abortSpy.count(), 1);
        QCOMPARE(m_mock->requestCount, requestsBefore); // 超时后不得续跑
    }

    void testTurnDeadlineResetsPerUserMessage() {
        AgentController& controller = AgentController::instance();
        controller.setTurnDeadlineMsForTest(1);
        controller.sendUserMessage(QStringLiteral("first"));
        QThread::msleep(8);
        QVERIFY(controller.turnDeadlineExceeded());
        controller.endTurnWithReason(QStringLiteral("deadline"), QString(), false);

        controller.setTurnDeadlineMsForTest(-1); // 恢复配置默认
        controller.sendUserMessage(QStringLiteral("second"));
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QVERIFY2(!controller.turnDeadlineExceeded(), "新用户消息必须重置计时");
        controller.abortCurrentTurn();
    }

    void testTurnDeadlineClamp() {
        AgentController& controller = AgentController::instance();
        controller.setTurnDeadlineConfigForTest(1);
        QCOMPARE(controller.turnDeadlineMs(), 10000);
        controller.setTurnDeadlineConfigForTest(1000000000);
        QCOMPARE(controller.turnDeadlineMs(), 3600000);
        controller.setTurnDeadlineConfigForTest(-1);
        QCOMPARE(controller.turnDeadlineMs(), 600000); // 默认配置
    }

    void testTurnDeadlineDoesNotFireEarly() {
        AgentController& controller = AgentController::instance();
        controller.setTurnDeadlineConfigForTest(600000);
        controller.sendUserMessage(QStringLiteral("quick"));
        QTest::qWait(5);
        QVERIFY(!controller.turnDeadlineExceeded());
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        controller.abortCurrentTurn();
    }
};

QTEST_MAIN(TestAgentController)
#include "test_agentcontroller.moc"
