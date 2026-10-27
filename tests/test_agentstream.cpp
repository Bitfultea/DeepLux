#include "core/agent/ILLMClient.h"
#include "core/agent/OpenAILLMClient.h"
#include "core/agent/SseStreamParser.h"
#include "core/agent/ToolSchema.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest/QtTest>
#include <functional>

using namespace DeepLux;

namespace {

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

/// 测试内最小 HTTP 服务：按脚本先写头，再按 delay 逐块写 body（模拟 SSE 半包/多包）
class SseHttpStub : public QObject {
public:
    struct Script {
        QByteArray headers; // 含状态行与 Content-Type，不要求结尾空行
        QList<QByteArray> chunks;
        int delayMs = 3;
    };

    explicit SseHttpStub(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, &SseHttpStub::onNewConnection);
    }

    bool listen() {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString endpoint() const {
        return QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(m_server.serverPort());
    }
    void script(const QByteArray& headers, const QList<QByteArray>& chunks, int delayMs = 3) {
        m_scripts.append({headers, chunks, delayMs});
    }

    static QByteArray jsonHeaders(const QByteArray& status = QByteArrayLiteral("200 OK")) {
        return "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n";
    }
    static QByteArray sseHeaders() {
        return QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n");
    }
    static QByteArray completionBody(const QString& content) {
        return QJsonDocument(
                   QJsonObject{{QStringLiteral("choices"),
                                QJsonArray{QJsonObject{
                                    {QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), content}}}}}},
                               {QStringLiteral("usage"), QJsonObject{{QStringLiteral("prompt_tokens"), 3},
                                                                     {QStringLiteral("completion_tokens"), 2}}}})
            .toJson(QJsonDocument::Compact);
    }
    static QByteArray dataEvent(const QByteArray& payload) {
        return QByteArrayLiteral("data: ") + payload + QByteArrayLiteral("\n\n");
    }

    int requestCount = 0;
    QList<QByteArray> requestBodies;

private:
    void onNewConnection() {
        while (QTcpSocket* socket = m_server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    }

    void onReadyRead(QTcpSocket* socket) {
        QByteArray& data = m_buffers[socket];
        data += socket->readAll();
        const int headerEnd = data.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        int contentLength = 0;
        const QByteArray header = data.left(headerEnd);
        for (const QByteArray& line : header.split('\n')) {
            if (line.trimmed().toLower().startsWith("content-length:"))
                contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
        }
        if (data.size() < headerEnd + 4 + contentLength)
            return;
        requestBodies.append(data.mid(headerEnd + 4, contentLength));
        data.remove(0, headerEnd + 4 + contentLength);
        ++requestCount;

        const Script response =
            m_scripts.isEmpty() ? Script{} : m_scripts[qMin(requestCount - 1, m_scripts.size() - 1)];
        QByteArray head = response.headers;
        if (!head.endsWith("\r\n\r\n"))
            head += "\r\n";
        socket->write(head);

        QPointer<QTcpSocket> guard(socket);
        const QList<QByteArray> chunks = response.chunks;
        if (chunks.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }
        // 固定间隔逐块写入：用单个周期定时器保证 chunk 顺序（多个 singleShot 在粗定时器下可能乱序）
        auto* timer = new QTimer(this);
        timer->setInterval(response.delayMs);
        int index = 0;
        connect(timer, &QTimer::timeout, this, [timer, guard, chunks, index]() mutable {
            if (!guard) {
                timer->stop();
                timer->deleteLater();
                return;
            }
            if (index >= chunks.size()) {
                timer->stop();
                timer->deleteLater();
                guard->disconnectFromHost();
                return;
            }
            guard->write(chunks.at(index++));
            guard->flush();
        });
        timer->start();
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QList<Script> m_scripts;
};

struct StreamProbe : public QObject {
    StreamProbe() = default;

    int starts = 0;
    int finished = 0;
    int failed = 0;
    int fallbacks = 0;
    int errors = 0;
    int retries = 0;
    int responses = 0;
    QStringList contentDeltas;
    QStringList reasoningDeltas;
    QStringList fallbackReasons;
    QStringList signalOrder;
    AgentResponse response;
    LLMErrorInfo lastError;

    void attach(OpenAILLMClient& client) {
        connect(&client, &ILLMClient::streamStarted, this, [this]() {
            ++starts;
            signalOrder.append(QStringLiteral("started"));
        });
        connect(&client, &ILLMClient::streamContentDelta, this, [this](const QString& text) {
            contentDeltas.append(text);
            if (!signalOrder.contains(QStringLiteral("content")))
                signalOrder.append(QStringLiteral("content"));
        });
        connect(&client, &ILLMClient::streamReasoningDelta, this, [this](const QString& text) {
            reasoningDeltas.append(text);
            if (!signalOrder.contains(QStringLiteral("reasoning")))
                signalOrder.append(QStringLiteral("reasoning"));
        });
        connect(&client, &ILLMClient::streamFinished, this, [this]() {
            ++finished;
            signalOrder.append(QStringLiteral("finished"));
        });
        connect(&client, &ILLMClient::streamFailed, this, [this]() {
            ++failed;
            signalOrder.append(QStringLiteral("failed"));
        });
        connect(&client, &ILLMClient::streamFallback, this, [this](const QString& reason) {
            ++fallbacks;
            fallbackReasons.append(reason);
            signalOrder.append(QStringLiteral("fallback"));
        });
        connect(&client, &ILLMClient::errorOccurred, this, [this](const QString&) {
            ++errors;
            signalOrder.append(QStringLiteral("error"));
        });
        connect(&client, &ILLMClient::errorOccurredClassified, this,
                [this](const LLMErrorInfo& info) { lastError = info; });
        connect(&client, &ILLMClient::retryScheduled, this, [this](const LLMErrorInfo&, int, int) { ++retries; });
        connect(&client, &ILLMClient::responseReceived, this, [this](const AgentResponse& resp) {
            ++responses;
            response = resp;
            signalOrder.append(QStringLiteral("response"));
        });
    }
};

AgentConversation simpleConversation(const QString& text = QStringLiteral("ping")) {
    AgentMessage message;
    message.role = QStringLiteral("user");
    message.content = text;
    AgentConversation ctx;
    ctx.messages.append(message);
    return ctx;
}

QByteArray reasoningEvent(const QByteArray& reasoning) {
    return QJsonDocument(QJsonObject{{QStringLiteral("choices"),
                                      QJsonArray{QJsonObject{
                                          {QStringLiteral("delta"), QJsonObject{{QStringLiteral("reasoning_content"),
                                                                                 QString::fromUtf8(reasoning)}}}}}}})
        .toJson(QJsonDocument::Compact);
}

QByteArray contentEvent(const QString& content) {
    return QJsonDocument(QJsonObject{{QStringLiteral("choices"),
                                      QJsonArray{QJsonObject{{QStringLiteral("delta"),
                                                              QJsonObject{{QStringLiteral("content"), content}}}}}}})
        .toJson(QJsonDocument::Compact);
}

} // namespace

// 第二迭代流式（设计 §3/§4/§6/§9.1）：解析器级（半包/多包/异常/未完成行）+ HTTP stub 端到端
class TestAgentStream : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
    }

    // ---------- 解析器级（无网络） ----------

    void testParserHalfPacketAndUtf8() {
        SseStreamParser parser;
        const QByteArray event = "data: {\"content\":\"你好世界\"}\n\n";
        QList<SseStreamParser::Event> events;
        for (char byte : event) {
            const QList<SseStreamParser::Event> fed = parser.feed(QByteArray(1, byte));
            events += fed;
        }
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().data, QByteArray("{\"content\":\"你好世界\"}"));
        QVERIFY(!events.first().done);
        QCOMPARE(parser.eventCount(), 1);
    }

    void testParserMultiEventOneFeed() {
        SseStreamParser parser;
        const QList<SseStreamParser::Event> events = parser.feed("data: a\n\ndata: b\n\ndata: [DONE]\n\n");
        QCOMPARE(events.size(), 3);
        QCOMPARE(events.at(0).data, QByteArray("a"));
        QCOMPARE(events.at(1).data, QByteArray("b"));
        QVERIFY(events.at(2).done);
        QVERIFY(events.at(0).name.isEmpty()); // 缺省视为 message
    }

    void testParserCrlfCommentsAndMultiLineData() {
        SseStreamParser parser;
        const QList<SseStreamParser::Event> events = parser.feed(": ping\r\ndata: line1\r\ndata: line2\r\n\r\n");
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().data, QByteArray("line1\nline2"));
    }

    void testParserIncompleteLastLineBuffered() {
        SseStreamParser parser;
        QVERIFY(parser.feed("data: par").isEmpty());
        const QList<SseStreamParser::Event> events = parser.feed("tial\n\n");
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().data, QByteArray("partial"));
    }

    void testParserDroppedOversizedLine() {
        SseStreamParser parser;
        const QByteArray oversized(SseStreamParser::kMaxLineBytes + 1, 'x');
        QByteArray input = oversized + "\n";
        input += "data: ok\n\n";
        const QList<SseStreamParser::Event> events = parser.feed(input);
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().data, QByteArray("ok"));
        QVERIFY(parser.droppedLines() >= 1);
    }

    void testParserDoneIsIdempotent() {
        SseStreamParser parser;
        QCOMPARE(parser.feed("data: [DONE]\n\n").size(), 1);
        QVERIFY(parser.feed("data: ignored\n\n").isEmpty());
        parser.reset();
        QCOMPARE(parser.feed("data: again\n\n").size(), 1);
    }

    // ---------- HTTP 端到端 ----------

    void testSseSingleEvent() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QByteArray body = SseHttpStub::dataEvent(contentEvent(QStringLiteral("Hello")));
        body += SseHttpStub::dataEvent("[DONE]");
        stub.script(SseHttpStub::sseHeaders(), {body}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);

        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(probe.response.content, QStringLiteral("Hello"));
        QCOMPARE(probe.finished, 1);
        QCOMPARE(probe.errors, 0);
        QCOMPARE(stub.requestCount, 1);
        QVERIFY(!probe.contentDeltas.isEmpty());
        QVERIFY(probe.response.truncatedByMaxTokens == false);
        // §9.1 #1：信号顺序 started → content → finished → response
        QVERIFY(probe.signalOrder.indexOf(QStringLiteral("started")) >= 0);
        QVERIFY(probe.signalOrder.indexOf(QStringLiteral("started")) <
                probe.signalOrder.indexOf(QStringLiteral("content")));
        QVERIFY(probe.signalOrder.indexOf(QStringLiteral("finished")) >= 0);
        QVERIFY2(probe.signalOrder.indexOf(QStringLiteral("finished")) <
                     probe.signalOrder.indexOf(QStringLiteral("response")),
                 qPrintable(probe.signalOrder.join(QStringLiteral(" -> "))));
    }

    void testSseHalfPacketUtf8() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QByteArray body = SseHttpStub::dataEvent(contentEvent(QStringLiteral("你好世界")));
        body += SseHttpStub::dataEvent("[DONE]");
        QList<QByteArray> bytes;
        for (char byte : body)
            bytes.append(QByteArray(1, byte));
        stub.script(SseHttpStub::sseHeaders(), bytes, 1);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(probe.response.content, QStringLiteral("你好世界"));
        QCOMPARE(probe.errors, 0);
    }

    void testSseMultiEventAndToolCallFragments() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        // 两个 tool call（index 0/1），arguments 各分 3 片
        QList<QByteArray> events;
        events.append(SseHttpStub::dataEvent(contentEvent(QStringLiteral("Hel"))));
        events.append(SseHttpStub::dataEvent(contentEvent(QStringLiteral("lo"))));
        const QList<QByteArray> fragments = {
            QByteArrayLiteral(
                "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_a\","
                "\"type\":\"function\",\"function\":{\"name\":\"get_flow_state\",\"arguments\":\"{\\\"a\\\":\"}}]}}]}"),
            QByteArrayLiteral("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":"
                              "{\"arguments\":\"1}\"}}]}}]}"),
            QByteArrayLiteral("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call_b\","
                              "\"function\":{\"name\":\"get_run_results\",\"arguments\":\"{}\"}}]}}]}"),
            QByteArrayLiteral("{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}"),
        };
        for (const QByteArray& fragment : fragments)
            events.append(SseHttpStub::dataEvent(fragment));
        events.append(SseHttpStub::dataEvent("[DONE]"));

        // 多包：两个事件合一个 chunk
        QList<QByteArray> chunks;
        chunks.append(events.at(0) + events.at(1));
        for (int i = 2; i < events.size(); ++i)
            chunks.append(events.at(i));
        stub.script(SseHttpStub::sseHeaders(), chunks, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));

        QCOMPARE(probe.response.content, QStringLiteral("Hello"));
        QCOMPARE(probe.response.toolCalls.size(), 2);
        const QJsonObject first = probe.response.toolCalls.at(0).toObject();
        QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("call_a"));
        QCOMPARE(first.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("get_flow_state"));
        QCOMPARE(first.value(QStringLiteral("function")).toObject().value(QStringLiteral("arguments")).toString(),
                 QStringLiteral("{\"a\":1}"));
        const QJsonObject second = probe.response.toolCalls.at(1).toObject();
        QCOMPARE(second.value(QStringLiteral("id")).toString(), QStringLiteral("call_b"));
        QCOMPARE(second.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("get_run_results"));
    }

    // §9.1 #7：单工具分片缺 index → 归入 0，仍产出 1 个 tool call
    void testSseToolCallMissingIndex() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QByteArray body = SseHttpStub::dataEvent(
            "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"call_x\",\"type\":\"function\",\"function\":"
            "{\"name\":\"get_flow_state\",\"arguments\":\"{}\"}}]}}]}");
        body += SseHttpStub::dataEvent("[DONE]");
        stub.script(SseHttpStub::sseHeaders(), {body}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(probe.response.toolCalls.size(), 1);
        const QJsonObject call = probe.response.toolCalls.first().toObject();
        QCOMPARE(call.value(QStringLiteral("id")).toString(), QStringLiteral("call_x"));
        QCOMPARE(call.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("get_flow_state"));
    }

    // §9.1 #8：超长行 + 已有 delta 后的坏 JSON → 流不中断、最终 content 正确
    void testSseDroppedLineAndBadJsonAfterDelta() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QList<QByteArray> chunks;
        chunks.append(SseHttpStub::dataEvent(contentEvent(QStringLiteral("Hi"))));
        QByteArray oversized(SseStreamParser::kMaxLineBytes + 1, 'x');
        oversized += '\n';
        chunks.append(oversized);
        chunks.append(SseHttpStub::dataEvent("{this is not json}"));
        QByteArray tail = SseHttpStub::dataEvent(contentEvent(QStringLiteral("!")));
        tail += SseHttpStub::dataEvent("[DONE]");
        chunks.append(tail);
        stub.script(SseHttpStub::sseHeaders(), chunks, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(probe.response.content, QStringLiteral("Hi!"));
        QCOMPARE(probe.errors, 0);
        QCOMPARE(probe.finished, 1);
    }

    // R4（finding）：回退后同批剩余事件（含 [DONE]）不得再消费；
    // 3 个坏 JSON + [DONE] 同批 → 只允许 1 次 responseReceived，内容来自回退的非流式响应
    void testSseFallbackStopsRemainingBatchEvents() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QByteArray batch;
        for (int i = 0; i < 3; ++i)
            batch += SseHttpStub::dataEvent("{not-json-fragment}");
        batch += SseHttpStub::dataEvent("[DONE]");
        stub.script(SseHttpStub::sseHeaders(), {batch}, 2);
        stub.script(SseHttpStub::jsonHeaders(), {SseHttpStub::completionBody(QStringLiteral("fallback ok"))}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QTest::qWait(150); // 给潜在的重复终态留出时间
        QCOMPARE(probe.responses, 1);
        QCOMPARE(probe.response.content, QStringLiteral("fallback ok"));
        QCOMPARE(probe.fallbacks, 1);
        QCOMPARE(stub.requestCount, 2);
        QCOMPARE(probe.errors, 0);
    }

    // R5（finding）：首个事件即顶层 error → 只发一次错误，关闭连接不再 responseReceived/不重发
    void testSseFirstEventErrorTerminatesStream() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QByteArray body = SseHttpStub::dataEvent("{\"error\":{\"message\":\"boom at first event\"}}");
        stub.script(SseHttpStub::sseHeaders(), {body}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.errors == 1; }));
        QTest::qWait(250);
        QCOMPARE(probe.errors, 1);
        QCOMPARE(probe.responses, 0);
        QCOMPARE(stub.requestCount, 1);
        QCOMPARE(probe.retries, 0);
    }

    void testSseBatchedErrorAfterDeltaNoRetry() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QList<QByteArray> chunks;
        chunks.append(SseHttpStub::dataEvent(contentEvent(QStringLiteral("Hel"))));
        chunks.append(SseHttpStub::dataEvent("{\"error\":{\"message\":\"boom\"}}"));
        stub.script(SseHttpStub::sseHeaders(), chunks, 3);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.errors == 1; }));
        QCOMPARE(probe.retries, 0);
        QCOMPARE(stub.requestCount, 1);
        QCOMPARE(probe.responses, 0);
        QCOMPARE(probe.failed, 1); // 已收 delta 后失败 → streamFailed
        QVERIFY(probe.contentDeltas.join(QString()).contains(QStringLiteral("Hel")));
    }

    void testSseFinishReasonWithoutDone() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QByteArray body = SseHttpStub::dataEvent(contentEvent(QStringLiteral("done")));
        body += SseHttpStub::dataEvent("{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}");
        stub.script(SseHttpStub::sseHeaders(), {body}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(probe.response.content, QStringLiteral("done"));
        QCOMPARE(probe.errors, 0);
    }

    void testLengthFinishFlag() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QByteArray body = SseHttpStub::dataEvent(contentEvent(QStringLiteral("cut")));
        body += SseHttpStub::dataEvent("{\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}");
        body += SseHttpStub::dataEvent("[DONE]");
        stub.script(SseHttpStub::sseHeaders(), {body}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QVERIFY(probe.response.truncatedByMaxTokens);
    }

    void testStreamRetryBeforeFirstDelta() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(SseHttpStub::jsonHeaders("500 Internal Server Error"), {QByteArrayLiteral("{\"error\":1}")}, 2);
        QByteArray body = SseHttpStub::dataEvent(contentEvent(QStringLiteral("recovered")));
        body += SseHttpStub::dataEvent("[DONE]");
        stub.script(SseHttpStub::sseHeaders(), {body}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }, 20000));
        QCOMPARE(probe.retries, 1);
        QCOMPARE(stub.requestCount, 2);
        QCOMPARE(probe.response.content, QStringLiteral("recovered"));
        QCOMPARE(probe.errors, 0);
    }

    void testNonStreamFallbackOn400() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(SseHttpStub::jsonHeaders("400 Bad Request"),
                    {QByteArrayLiteral("{\"error\":{\"message\":\"stream is not supported by this gateway\"}}")}, 2);
        stub.script(SseHttpStub::jsonHeaders(), {SseHttpStub::completionBody(QStringLiteral("fallback ok"))}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(stub.requestCount, 2);
        QCOMPARE(probe.fallbacks, 1);
        QCOMPARE(probe.response.content, QStringLiteral("fallback ok"));
        // 第二次请求不得再带 stream 键
        const QJsonObject secondBody = QJsonDocument::fromJson(stub.requestBodies.value(1)).object();
        QVERIFY(!secondBody.contains(QStringLiteral("stream")));
        QVERIFY(!secondBody.contains(QStringLiteral("stream_options")));
    }

    void testNonStreamFallbackOnJsonContentType() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(SseHttpStub::jsonHeaders(), {SseHttpStub::completionBody(QStringLiteral("json body"))}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(stub.requestCount, 1); // F2：不重发
        QCOMPARE(probe.fallbacks, 1);
        QCOMPARE(probe.response.content, QStringLiteral("json body"));
    }

    // F4：200 + 无任何 data 行（空流）→ 回退一次性请求（且只回退一次）
    void testEmptyStreamFallsBackToNonStream() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(SseHttpStub::sseHeaders(), {QByteArray()}, 2);
        stub.script(SseHttpStub::jsonHeaders(), {SseHttpStub::completionBody(QStringLiteral("after empty"))}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(stub.requestCount, 2);
        QCOMPARE(probe.fallbacks, 1);
        QCOMPARE(probe.response.content, QStringLiteral("after empty"));
        const QJsonObject secondBody = QJsonDocument::fromJson(stub.requestBodies.value(1)).object();
        QVERIFY(!secondBody.contains(QStringLiteral("stream")));
    }

    void testStreamDisabledByConfig() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(SseHttpStub::jsonHeaders(), {SseHttpStub::completionBody(QStringLiteral("plain"))}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        client.setStreamEnabled(false);
        QVERIFY(!client.isStreamEnabled());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(probe.starts, 0);
        QCOMPARE(probe.fallbacks, 0);
        const QJsonObject body = QJsonDocument::fromJson(stub.requestBodies.value(0)).object();
        QVERIFY(!body.contains(QStringLiteral("stream")));
    }

    void testReasoningNotEchoedByDefault() {
        AgentConversation ctx = simpleConversation();
        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        assistant.content = QStringLiteral("answer");
        assistant.reasoningContent = QStringLiteral("hidden reasoning");
        ctx.messages.append(assistant);

        const QJsonArray byDefault = ctx.toOpenAIMessages(ReasoningEcho::None);
        QVERIFY(!byDefault.last().toObject().contains(QStringLiteral("reasoning_content")));
        const QJsonArray full = ctx.toOpenAIMessages(ReasoningEcho::Full);
        QCOMPARE(full.last().toObject().value(QStringLiteral("reasoning_content")).toString(),
                 QStringLiteral("hidden reasoning"));
        // 无参重载默认 none（第二迭代行为变更）
        QVERIFY(!ctx.toOpenAIMessages().last().toObject().contains(QStringLiteral("reasoning_content")));
    }

    void testReasoningDeltaChannel() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        QByteArray body = SseHttpStub::dataEvent(reasoningEvent("思考中"));
        body += SseHttpStub::dataEvent(contentEvent(QStringLiteral("答案")));
        body += SseHttpStub::dataEvent("[DONE]");
        stub.script(SseHttpStub::sseHeaders(), {body}, 2);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.responses == 1; }));
        QCOMPARE(probe.response.reasoningContent, QStringLiteral("思考中"));
        QCOMPARE(probe.response.content, QStringLiteral("答案"));
        QCOMPARE(probe.reasoningDeltas.join(QString()), QStringLiteral("思考中"));
        QCOMPARE(probe.contentDeltas.join(QString()), QStringLiteral("答案"));
    }

    void testAbortDuringStreamStopsSilently() {
        SseHttpStub stub;
        QVERIFY(stub.listen());
        // 第一块只有 role（触发 streamStarted），第二块才是内容；留出 abort 窗口
        const QByteArray roleEvent = SseHttpStub::dataEvent("{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}");
        QByteArray contentChunk = SseHttpStub::dataEvent(contentEvent(QStringLiteral("Hel")));
        contentChunk += SseHttpStub::dataEvent("[DONE]");
        stub.script(SseHttpStub::sseHeaders(), {roleEvent, contentChunk}, 60);

        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        StreamProbe probe;
        probe.attach(client);
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&probe]() { return probe.starts == 1; }, 5000));
        client.abortRequest();
        QTest::qWait(250); // 越过第二块的发送窗口
        QCOMPARE(probe.errors, 0);
        QCOMPARE(probe.responses, 0);
        QCOMPARE(probe.finished, 0);
        QCOMPARE(stub.requestCount, 1);
    }
};

QTEST_MAIN(TestAgentStream)
#include "test_agentstream.moc"
