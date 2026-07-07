#include <QtTest/QtTest>
#include <QSignalSpy>

// 测试需要访问 AgentController 的私有成员（m_conversationHistory, trimHistory）
// 这是 Qt 测试中的常见做法，仅用于测试文件
#define private public
#include "core/agent/AgentController.h"
#undef private

#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "core/agent/ILLMClient.h"

using namespace DeepLux;

// ========== Mock LLM Client ==========

class MockLLMClient : public ILLMClient
{
    Q_OBJECT
public:
    explicit MockLLMClient(QObject* parent = nullptr) : ILLMClient(parent) {}

    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}

    AgentConversation lastCtx;
    QList<ToolDefinition> lastTools;

    void sendRequest(const AgentConversation& ctx,
                     const QList<ToolDefinition>& tools) override
    {
        lastCtx = ctx;
        lastTools = tools;
    }

    void emitResponse(const AgentResponse& resp) {
        emit responseReceived(resp);
        QCoreApplication::processEvents();
    }

    void emitError(const QString& error) {
        emit errorOccurred(error);
        QCoreApplication::processEvents();
    }
};

// ========== Test Suite ==========

class TestAgentController : public QObject
{
    Q_OBJECT

private:
    MockLLMClient* m_mock = nullptr;
    ILLMClient* m_oldClient = nullptr;

private slots:
    void initTestCase()
    {
        ToolSchema::instance().registerDefaultTools();
        AgentController::instance().initialize();
    }

    void cleanupTestCase()
    {
        AgentController::instance().shutdown();
    }

    void init()
    {
        // 每个测试前彻底重置状态（状态机 + 对话历史 + client）
        AgentController::instance().clearConversation();
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        // 强制回到 Idle（前一个测试可能留下非 Idle 状态）
        AgentController::instance().transitionTo(AgentController::AgentState::Idle);

        m_mock = new MockLLMClient(&AgentController::instance());
        m_oldClient = AgentController::instance().llmClient();
        AgentController::instance().setLLMClient(m_mock);
    }

    void cleanup()
    {
        AgentController::instance().setLLMClient(m_oldClient);
        delete m_mock;
        m_mock = nullptr;
    }

    // ---------- 状态机基础 ----------

    void testInitialStateIsIdle()
    {
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testSendMessageTransitionsToThinking()
    {
        AgentController::instance().sendUserMessage("Hello");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->lastCtx.messages.size(), 1);
        QCOMPARE(m_mock->lastCtx.messages.first().role, QString("user"));
    }

    void testPlainTextResponseReturnsToIdle()
    {
        AgentController::instance().sendUserMessage("Hello");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        AgentResponse resp;
        resp.success = true;
        resp.content = "Hi there!";
        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testErrorReturnsToIdle()
    {
        AgentController::instance().sendUserMessage("Hello");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        m_mock->emitError("Network timeout");

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    // ---------- Advisor 模式确认流程 ----------

    void testAdvisorToolCallsTransitionToConfirming()
    {
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

    void testConfirmToolsTransitionsToExecutingAndThenThinking()
    {
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
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        // extendAgentLoop 执行工具后会发送新的 LLM 请求（mock 不响应）
        // 状态应为 Thinking（等待第二轮 LLM 响应）
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        QVERIFY(AgentController::instance().pendingToolCalls().isEmpty());
    }

    void testRejectToolsReturnsToIdle()
    {
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
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(responseSpy.count(), 1);
        QVERIFY(AgentController::instance().pendingToolCalls().isEmpty());
    }

    // ---------- 并发防护 ----------

    void testBusyStateRejectsNewMessage()
    {
        AgentController::instance().sendUserMessage("First");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);
        AgentController::instance().sendUserMessage("Second");

        QCOMPARE(responseSpy.count(), 1);
        QString reply = responseSpy.takeFirst()[0].toString();
        QVERIFY(reply.contains("busy"));
    }

    // ---------- 错误恢复 ----------

    void testErrorInConfirmingCleansPendingTools()
    {
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

    void testTrimHistoryPreservesMessagePairs()
    {
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
                            if (v.toObject()["id"].toString() == AgentController::instance().m_conversationHistory[i].toolCallId) {
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

    // ---------- 最大轮数限制 ----------

    void testAutopilotStopsAtTurnLimit()
    {
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

    void testAutopilotRejectsTooManyToolCallsInOneTurn()
    {
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

    void testObserverModeShowsToolsButDoesNotExecute()
    {
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
};

QTEST_MAIN(TestAgentController)
#include "test_agentcontroller.moc"
