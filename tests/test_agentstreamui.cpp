#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/agent/OpenAILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/manager/ConfigManager.h"
#include "ui/dialogs/AgentSettingsDialog.h"
#include "ui/widgets/AgentChatPanel.h"
#include "ui/widgets/AgentMessageBubble.h"

#include <QCheckBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest/QtTest>
#include <functional>

using namespace DeepLux;

namespace {

// 信号注入式假客户端：不访问网络，直接驱动控制器的流式/终态路径
class MockStreamingClient : public ILLMClient {
    Q_OBJECT
public:
    explicit MockStreamingClient(QObject* parent = nullptr) : ILLMClient(parent) {}

    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}
    bool supportsImageInput() const override {
        return true;
    }
    void sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>&) override {
        lastCtx = ctx;
        ++requestCount;
    }

    void emitStarted() {
        emit streamStarted();
        QCoreApplication::processEvents();
    }
    void emitContent(const QString& text) {
        emit streamContentDelta(text);
    }
    void emitReasoning(const QString& text) {
        emit streamReasoningDelta(text);
    }
    void emitFinished() {
        emit streamFinished();
        QCoreApplication::processEvents();
    }
    void emitFallback(const QString& reason) {
        emit streamFallback(reason);
        QCoreApplication::processEvents();
    }
    void emitResponse(const AgentResponse& resp) {
        emit responseReceived(resp);
        QCoreApplication::processEvents();
    }

    AgentConversation lastCtx;
    int requestCount = 0;
};

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

/// 最小 HTTP 服务：记录请求体并返回固定 JSON（用于 R3 的请求体断言）
class RequestCaptureStub : public QObject {
public:
    RequestCaptureStub() {
        connect(&m_server, &QTcpServer::newConnection, this, &RequestCaptureStub::onNewConnection);
    }
    bool listen() {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString endpoint() const {
        return QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(m_server.serverPort());
    }
    QList<QByteArray> requestBodies;

private:
    void onNewConnection() {
        while (QTcpSocket* socket = m_server.nextPendingConnection()) {
            auto* buffer = new QByteArray();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
                buffer->append(socket->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;
                int contentLength = 0;
                for (const QByteArray& line : buffer->left(headerEnd).split('\n')) {
                    if (line.trimmed().toLower().startsWith("content-length:"))
                        contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                }
                if (buffer->size() < headerEnd + 4 + contentLength)
                    return;
                requestBodies.append(buffer->mid(headerEnd + 4, contentLength));
                const QByteArray body =
                    "{\"choices\":[{\"message\":{\"content\":\"ok\"}}],\"usage\":{\"prompt_tokens\":1,"
                    "\"completion_tokens\":1}}";
                const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                socket->write(response);
                socket->flush();
                socket->disconnectFromHost();
                buffer->clear();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    }

    QTcpServer m_server;
};

} // namespace

// 第二迭代 UI/控制器流式接线（设计 §9.1.1 U1~U6）：
// 50ms 合批、中止保留已收、reasoning 折叠、空气泡移除、终态以最终文本为准、无重复气泡
class TestAgentStreamUi : public QObject {
    Q_OBJECT

private:
    MockStreamingClient* m_client = nullptr;
    QTemporaryDir m_configDir;

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
        QVERIFY(m_configDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_configDir.path().toLocal8Bit());
        QVERIFY(ConfigManager::instance().initialize());
        QVERIFY(AgentController::instance().initialize());
    }

    void cleanupTestCase() {
        AgentController::instance().setLLMClient(nullptr);
        AgentController::instance().shutdown();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void init() {
        AgentController::instance().clearConversation();
        AgentController::instance().transitionTo(AgentController::AgentState::Idle);
        m_client = new MockStreamingClient(&AgentController::instance());
        AgentController::instance().setLLMClient(m_client);
    }

    void cleanup() {
        AgentController::instance().setLLMClient(nullptr);
        delete m_client;
        m_client = nullptr;
    }

    // U1：300 个 1 字符 delta → 控制器合批后信号次数远小于 300，且文本完整
    void testCoalescing() {
        AgentController& controller = AgentController::instance();
        QSignalSpy deltaSpy(&controller, &AgentController::agentStreamContentDelta);
        QSignalSpy startedSpy(&controller, &AgentController::agentStreamStarted);

        controller.sendUserMessage(QStringLiteral("stream please"));
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        m_client->emitStarted();
        QCOMPARE(startedSpy.count(), 1);

        QString expected;
        for (int i = 0; i < 300; ++i) {
            const QString chunk = QStringLiteral("a");
            expected += chunk;
            m_client->emitContent(chunk);
        }
        // 第一批发 + 末尾 flush
        QTest::qWait(150);
        QCoreApplication::processEvents();

        AgentResponse resp;
        resp.success = true;
        resp.content = expected;
        m_client->emitFinished();
        m_client->emitResponse(resp);
        QCoreApplication::processEvents();
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);

        int emittedChars = 0;
        for (const QList<QVariant>& args : deltaSpy)
            emittedChars += args.at(0).toString().size();
        QCOMPARE(emittedChars, expected.size()); // 文本完整
        QVERIFY2(deltaSpy.count() >= 1 && deltaSpy.count() < 300,
                 qPrintable(QStringLiteral("delta signals=%1").arg(deltaSpy.count())));
        QVERIFY2(deltaSpy.count() <= 100, "50ms 合批应对 300 个 delta 产生远少于 100 次刷新");
    }

    // U2：中止保留已收、回 Idle、不误报、历史不提交
    void testAbortKeepsPartialAndNoError() {
        AgentController& controller = AgentController::instance();
        QSignalSpy abortSpy(&controller, &AgentController::agentStreamAborted);
        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        QSignalSpy responseSpy(&controller, &AgentController::llmResponseReceived);

        controller.sendUserMessage(QStringLiteral("stream please"));
        m_client->emitStarted();
        m_client->emitContent(QStringLiteral("Hel"));
        QTest::qWait(80);
        QCoreApplication::processEvents();

        controller.abortCurrentTurn();
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(abortSpy.count(), 1);
        QCOMPARE(errorSpy.count(), 0);                     // 不产生错误气泡
        QCOMPARE(controller.conversationHistorySize(), 1); // 仅 user，半截 assistant 不提交
        QCOMPARE(responseSpy.count(), 1);                  // "Stopped by user."
    }

    // U2b：面板层保留已收文本并追加"已停止"标记
    void testPanelAbortKeepsPartialText() {
        AgentChatPanel panel;
        panel.beginAgentStream();
        QVERIFY(panel.isStreamActive());
        panel.appendStreamContent(QStringLiteral("Hel"));
        panel.abortStreamedMessage(QStringLiteral("已停止"));
        QVERIFY(!panel.isStreamActive());

        const QList<AgentMessageBubble*> bubbles = panel.findChildren<AgentMessageBubble*>();
        QCOMPARE(bubbles.size(), 1);
        QVERIFY2(bubbles.first()->text().contains(QStringLiteral("Hel")), qPrintable(bubbles.first()->text()));
        QVERIFY2(bubbles.first()->text().contains(QStringLiteral("已停止")), qPrintable(bubbles.first()->text()));
    }

    // U3：reasoning 完整、默认折叠、正文不含 reasoning
    void testReasoningCollapsible() {
        AgentMessageBubble bubble(AgentMessageBubble::Sender::Agent, QStringLiteral("answer"), false);
        bubble.appendReasoning(QStringLiteral("hidden thoughts"));
        bubble.setReasoningStreaming(true);
        QCOMPARE(bubble.reasoningText(), QStringLiteral("hidden thoughts"));
        QCOMPARE(bubble.text(), QStringLiteral("answer")); // reasoning 不进入正文

        auto* toggle = bubble.findChild<QToolButton*>(QStringLiteral("AgentReasoningToggle"));
        auto* body = bubble.findChild<QLabel*>(QStringLiteral("AgentReasoningBody"));
        QVERIFY(toggle != nullptr);
        QVERIFY(body != nullptr);
        QVERIFY2(!toggle->isChecked(), "reasoning 默认折叠");
        QVERIFY2(body->isHidden(), "reasoning 正文默认隐藏");
        QVERIFY2(toggle->text().contains(QStringLiteral("思考中")), qPrintable(toggle->text()));

        toggle->click();
        QVERIFY2(!body->isHidden(), "展开后 reasoning 正文可见");
        QVERIFY2(body->text().contains(QStringLiteral("hidden thoughts")), qPrintable(body->text()));

        bubble.setReasoningStreaming(false);
        QVERIFY2(toggle->text().contains(QStringLiteral("已思考")), qPrintable(toggle->text()));
    }

    // U4：仅 tool_calls、无正文的流式气泡被移除，不产生空气泡
    void testEmptyToolCallBubbleRemoved() {
        AgentChatPanel panel;
        panel.beginAgentStream();
        panel.appendStreamReasoning(QString()); // 空，不应产生内容
        panel.finalizeStreamedMessage(QString());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); // 处理 deleteLater
        QVERIFY(!panel.isStreamActive());
        QVERIFY2(panel.findChildren<AgentMessageBubble*>().isEmpty(), "无正文/无 reasoning 的空气泡必须移除");
    }

    // U5：终态文本与流式累积不一致时以最终文本为准
    void testFinalizeUsesFinalText() {
        AgentChatPanel panel;
        panel.beginAgentStream();
        panel.appendStreamContent(QStringLiteral("Hel"));
        panel.finalizeStreamedMessage(QStringLiteral("Hello"));
        const QList<AgentMessageBubble*> bubbles = panel.findChildren<AgentMessageBubble*>();
        QCOMPARE(bubbles.size(), 1);
        QCOMPARE(bubbles.first()->text(), QStringLiteral("Hello"));
    }

    // U6：重复终态不产生第二个气泡；流式结束后 addMessage 正常
    void testNoDuplicateBubbleOnFinish() {
        AgentChatPanel panel;
        panel.beginAgentStream();
        panel.appendStreamContent(QStringLiteral("Hi"));
        panel.finalizeStreamedMessage(QStringLiteral("Hi"));
        panel.finalizeStreamedMessage(QStringLiteral("Hi")); // 重复终态
        QCOMPARE(panel.findChildren<AgentMessageBubble*>().size(), 1);
        QVERIFY(!panel.isStreamActive());
    }

    // 回退：控制器发 agentStreamFallback + 审计条目
    void testFallbackSignalAndAudit() {
        AgentController& controller = AgentController::instance();
        QSignalSpy fallbackSpy(&controller, &AgentController::agentStreamFallback);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);

        controller.sendUserMessage(QStringLiteral("stream please"));
        m_client->emitFallback(QStringLiteral("server returned non-stream body"));

        QCOMPARE(fallbackSpy.count(), 1);
        QCOMPARE(fallbackSpy.takeFirst().at(0).toString(), QStringLiteral("server returned non-stream body"));
        bool sawAudit = false;
        for (const QList<QVariant>& args : logSpy) {
            const AgentActionLogEntry entry = args.at(0).value<AgentActionLogEntry>();
            if (entry.action == QStringLiteral("LLM stream fallback")) {
                sawAudit = true;
                QVERIFY(entry.result == QStringLiteral("success"));
            }
        }
        QVERIFY(sawAudit);
        controller.abortCurrentTurn();
    }

    // 截断：response.truncatedByMaxTokens → agentStreamTruncated + result=truncated 审计
    void testTruncatedSignal() {
        AgentController& controller = AgentController::instance();
        QSignalSpy truncatedSpy(&controller, &AgentController::agentStreamTruncated);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);

        controller.sendUserMessage(QStringLiteral("stream please"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("cut");
        resp.truncatedByMaxTokens = true;
        m_client->emitResponse(resp);

        QCOMPARE(truncatedSpy.count(), 1);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        // R2：截断必须落一条 result="truncated" 的审计条目
        bool sawTruncatedAudit = false;
        for (const QList<QVariant>& args : auditSpy) {
            const AgentActionLogEntry entry = args.at(0).value<AgentActionLogEntry>();
            if (entry.result == QStringLiteral("truncated")) {
                sawTruncatedAudit = true;
                QCOMPARE(entry.actor, QStringLiteral("Agent"));
                QVERIFY(!entry.action.isEmpty());
                QVERIFY(entry.action.contains(QStringLiteral("truncated"), Qt::CaseInsensitive));
            }
        }
        QVERIFY2(sawTruncatedAudit, "finish_reason=length 必须 emit result=truncated 审计条目");
    }

    // R1：非流式响应 reasoning 进折叠块，正文保持不含 reasoning
    void testNonStreamReasoningDisplayed() {
        AgentChatPanel panel;
        panel.addMessage(AgentMessageBubble::Sender::Agent, QStringLiteral("answer"));
        panel.setLastAgentReasoning(QStringLiteral("hidden thoughts"));
        const QList<AgentMessageBubble*> bubbles = panel.findChildren<AgentMessageBubble*>();
        QCOMPARE(bubbles.size(), 1);
        QCOMPARE(bubbles.first()->text(), QStringLiteral("answer"));
        QCOMPARE(bubbles.first()->reasoningText(), QStringLiteral("hidden thoughts"));
        auto* body = bubbles.first()->findChild<QLabel*>(QStringLiteral("AgentReasoningBody"));
        QVERIFY(body != nullptr);
        QVERIFY2(body->isHidden(), "reasoning 默认折叠");
    }

    void testControllerExposesLastReasoning() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.lastResponseReasoning().isEmpty());
        controller.sendUserMessage(QStringLiteral("reason"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("answer");
        resp.reasoningContent = QStringLiteral("because");
        m_client->emitResponse(resp);
        QCOMPARE(controller.lastResponseReasoning(), QStringLiteral("because"));
    }

    // R3：streamIncludeUsage=true 时请求体含 stream_options.include_usage
    void testStreamIncludeUsageRequestBody() {
        RequestCaptureStub stub;
        QVERIFY(stub.listen());
        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        client.setStreamIncludeUsage(true);
        QSignalSpy responseSpy(&client, &ILLMClient::responseReceived);
        AgentConversation ctx;
        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("hi");
        ctx.messages.append(user);
        client.sendRequest(ctx, {});
        QVERIFY(waitFor([&stub]() { return !stub.requestBodies.isEmpty(); }));
        QVERIFY(waitFor([&responseSpy]() { return responseSpy.count() == 1; }));
        const QJsonObject body = QJsonDocument::fromJson(stub.requestBodies.first()).object();
        QCOMPARE(body.value(QStringLiteral("stream")).toBool(), true);
        QCOMPARE(
            body.value(QStringLiteral("stream_options")).toObject().value(QStringLiteral("include_usage")).toBool(),
            true);
    }

    // 设置页：上下文治理数值框越界即时纠正（QSpinBox range）
    void testSettingsContextBudgetSpinClamps() {
        AgentSettingsDialog dialog;
        auto* budget = dialog.findChild<QSpinBox*>(QStringLiteral("AgentContextBudgetSpin"));
        auto* toolChars = dialog.findChild<QSpinBox*>(QStringLiteral("AgentToolResultMaxCharsSpin"));
        QVERIFY(budget != nullptr);
        QVERIFY(toolChars != nullptr);
        QCOMPARE(budget->minimum(), 16000);
        QCOMPARE(budget->maximum(), 1000000);
        budget->setValue(1);
        QCOMPARE(budget->value(), 16000);
        budget->setValue(1000000000);
        QCOMPARE(budget->value(), 1000000);
        QCOMPARE(dialog.contextBudgetTokens(), 1000000);
        toolChars->setValue(1);
        QCOMPARE(toolChars->value(), 1000);
        toolChars->setValue(999999);
        QCOMPARE(toolChars->value(), 64000);
        QCOMPARE(dialog.toolResultMaxChars(), 64000);
    }

    // B5：隐私开关的默认值、显式值加载、保存绑定与窄窗口可见性
    void testSettingsPrivacyToggles() {
        ConfigManager& cfg = ConfigManager::instance();
        cfg.removeGroupKey(QStringLiteral("agent"), QStringLiteral("auditEnabled"));
        cfg.removeGroupKey(QStringLiteral("agent"), QStringLiteral("failedRequestLogEnabled"));

        {
            AgentSettingsDialog dialog;
            auto* audit = dialog.findChild<QCheckBox*>(QStringLiteral("AgentAuditEnabledCheck"));
            auto* failed = dialog.findChild<QCheckBox*>(QStringLiteral("AgentFailedRequestLogCheck"));
            QVERIFY(audit != nullptr);
            QVERIFY(failed != nullptr);
            QCOMPARE(audit->isChecked(), true);   // 审计默认保持开启（兼容）
            QCOMPARE(failed->isChecked(), false); // 失败正文默认关闭
            QCOMPARE(dialog.auditEnabled(), true);
            QCOMPARE(dialog.failedRequestLogEnabled(), false);

            dialog.resize(500, dialog.sizeHint().height());
            QVERIFY(audit->isVisibleTo(&dialog));
            QVERIFY(failed->isVisibleTo(&dialog));
            QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("AgentPrivacyNote")) != nullptr);
        }

        cfg.setGroupValue(QStringLiteral("agent"), QStringLiteral("auditEnabled"), false);
        cfg.setGroupValue(QStringLiteral("agent"), QStringLiteral("failedRequestLogEnabled"), true);
        {
            AgentSettingsDialog dialog;
            QCOMPARE(dialog.auditEnabled(), false);
            QCOMPARE(dialog.failedRequestLogEnabled(), true);

            auto* audit = dialog.findChild<QCheckBox*>(QStringLiteral("AgentAuditEnabledCheck"));
            auto* failed = dialog.findChild<QCheckBox*>(QStringLiteral("AgentFailedRequestLogCheck"));
            audit->setChecked(true);
            failed->setChecked(false);
            for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("OK")) {
                    button->click();
                    break;
                }
            }
            QCOMPARE(cfg.groupBool(QStringLiteral("agent"), QStringLiteral("auditEnabled"), false), true);
            QCOMPARE(cfg.groupBool(QStringLiteral("agent"), QStringLiteral("failedRequestLogEnabled"), true), false);
        }
    }
};

QTEST_MAIN(TestAgentStreamUi)
#include "test_agentstreamui.moc"
