#include <QJsonDocument>
#include <QSignalSpy>
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
};

QTEST_MAIN(TestAgentController)
#include "test_agentcontroller.moc"
