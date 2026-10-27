#include "core/agent/ILLMClient.h"
#include "core/agent/OpenAILLMClient.h"
#include "core/agent/ToolSchema.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest/QtTest>
#include <functional>

using namespace DeepLux;

namespace {

// 测试内最小 HTTP 服务：按脚本返回状态码/响应体，模拟 OpenAI 兼容端点的
// 429/5xx/4xx/畸形响应。脚本用尽后重复最后一条（便于断言"重试耗尽"）。
class LlmHttpStub : public QObject {
public:
    struct ScriptedResponse {
        int status = 200;
        QByteArray body;
    };

    explicit LlmHttpStub(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, &LlmHttpStub::onNewConnection);
    }

    bool listen() {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    QString endpoint() const {
        return QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(m_server.serverPort());
    }

    void script(int status, const QByteArray& body) {
        m_script.append({status, body});
    }

    static QByteArray successBody() {
        return QByteArrayLiteral("{\"choices\":[{\"message\":{\"content\":\"ok\"}}],\"usage\":{\"prompt_tokens\":3,"
                                 "\"completion_tokens\":2}}");
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

        const ScriptedResponse response = m_script.isEmpty() ? ScriptedResponse{200, successBody()}
                                                             : m_script[qMin(requestCount - 1, m_script.size() - 1)];
        writeResponse(socket, response);
    }

    static QByteArray statusText(int status) {
        switch (status) {
        case 200:
            return QByteArrayLiteral("OK");
        case 400:
            return QByteArrayLiteral("Bad Request");
        case 401:
            return QByteArrayLiteral("Unauthorized");
        case 403:
            return QByteArrayLiteral("Forbidden");
        case 422:
            return QByteArrayLiteral("Unprocessable Entity");
        case 429:
            return QByteArrayLiteral("Too Many Requests");
        case 500:
            return QByteArrayLiteral("Internal Server Error");
        case 502:
            return QByteArrayLiteral("Bad Gateway");
        case 503:
            return QByteArrayLiteral("Service Unavailable");
        default:
            return QByteArrayLiteral("Error");
        }
    }

    void writeResponse(QTcpSocket* socket, const ScriptedResponse& response) {
        QByteArray http;
        http += "HTTP/1.1 " + QByteArray::number(response.status) + " " + statusText(response.status) + "\r\n";
        http += "Content-Type: application/json\r\n";
        http += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
        http += "Connection: close\r\n\r\n";
        http += response.body;
        socket->write(http);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QList<ScriptedResponse> m_script;
};

struct ClientProbe : public QObject {
    ClientProbe() = default;

    int responses = 0;
    int retries = 0;
    int errors = 0;
    int lastRetryAttempt = 0;
    int lastRetryDelayMs = 0;
    LLMErrorInfo lastRetryInfo;
    LLMErrorInfo lastError;
    AgentResponse lastResponse;

    void attach(OpenAILLMClient& client) {
        connect(&client, &ILLMClient::responseReceived, this, [this](const AgentResponse& resp) {
            ++responses;
            lastResponse = resp;
        });
        connect(&client, &ILLMClient::retryScheduled, this, [this](const LLMErrorInfo& info, int attempt, int delay) {
            ++retries;
            lastRetryInfo = info;
            lastRetryAttempt = attempt;
            lastRetryDelayMs = delay;
        });
        connect(&client, &ILLMClient::errorOccurredClassified, this,
                [this](const LLMErrorInfo& info) { lastError = info; });
        connect(&client, &ILLMClient::errorOccurred, this, [this](const QString&) { ++errors; });
    }
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

AgentConversation simpleConversation() {
    AgentMessage message;
    message.role = QStringLiteral("user");
    message.content = QStringLiteral("ping");
    AgentConversation ctx;
    ctx.messages.append(message);
    return ctx;
}

void configure(OpenAILLMClient& client, const LlmHttpStub& stub) {
    client.setEndpoint(stub.endpoint());
    client.setApiKey(QStringLiteral("test-key"));
    client.setModel(QStringLiteral("test-model"));
}

} // namespace

// LLM 错误分类与重试（docs/agent-iteration1-design.md §7）：
// 断言分类映射、退避矩阵与非重试类别，全部走本地 QTcpServer 假端点，无外网依赖。
class TestAgentRetry : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qRegisterMetaType<LLMErrorInfo>("LLMErrorInfo");
        qRegisterMetaType<LLMErrorInfo>("DeepLux::LLMErrorInfo");
    }

    void testClassificationNamesRoundTrip() {
        const QList<LLMErrorCategory> categories = {
            LLMErrorCategory::None,    LLMErrorCategory::Auth,      LLMErrorCategory::RateLimit,
            LLMErrorCategory::Server,  LLMErrorCategory::Network,   LLMErrorCategory::BadRequest,
            LLMErrorCategory::Timeout, LLMErrorCategory::Malformed,
        };
        QSet<QString> names;
        for (LLMErrorCategory category : categories) {
            const QString name = llmErrorCategoryName(category);
            QVERIFY(!name.isEmpty());
            QVERIFY2(!names.contains(name), qPrintable(QStringLiteral("duplicate category name: %1").arg(name)));
            names.insert(name);
        }
        QCOMPARE(llmErrorCategoryName(LLMErrorCategory::Auth), QStringLiteral("Auth"));
        QCOMPARE(llmErrorCategoryName(LLMErrorCategory::RateLimit), QStringLiteral("RateLimit"));
        QCOMPARE(llmErrorCategoryName(LLMErrorCategory::Server), QStringLiteral("Server"));
        QCOMPARE(llmErrorCategoryName(LLMErrorCategory::Network), QStringLiteral("Network"));
        QCOMPARE(llmErrorCategoryName(LLMErrorCategory::BadRequest), QStringLiteral("BadRequest"));
        QCOMPARE(llmErrorCategoryName(LLMErrorCategory::Timeout), QStringLiteral("Timeout"));
        QCOMPARE(llmErrorCategoryName(LLMErrorCategory::Malformed), QStringLiteral("Malformed"));
        QCOMPARE(llmErrorCategoryName(LLMErrorCategory::None), QStringLiteral("None"));
    }

    void testRetryPolicyMatrix() {
        // §7.3：RateLimit/Server 3 次（500/1000/2000、1000/2000/4000），Network 2 次（1000/3000），Timeout 1 次
        QCOMPARE(OpenAILLMClient::maxRetriesFor(LLMErrorCategory::RateLimit), 3);
        QCOMPARE(OpenAILLMClient::maxRetriesFor(LLMErrorCategory::Server), 3);
        QCOMPARE(OpenAILLMClient::maxRetriesFor(LLMErrorCategory::Network), 2);
        QCOMPARE(OpenAILLMClient::maxRetriesFor(LLMErrorCategory::Timeout), 1);
        QCOMPARE(OpenAILLMClient::maxRetriesFor(LLMErrorCategory::Auth), 0);
        QCOMPARE(OpenAILLMClient::maxRetriesFor(LLMErrorCategory::BadRequest), 0);
        QCOMPARE(OpenAILLMClient::maxRetriesFor(LLMErrorCategory::Malformed), 0);
        QCOMPARE(OpenAILLMClient::maxRetriesFor(LLMErrorCategory::None), 0);

        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::RateLimit, 1), 500);
        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::RateLimit, 2), 1000);
        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::RateLimit, 3), 2000);
        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::Server, 1), 1000);
        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::Server, 2), 2000);
        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::Server, 3), 4000);
        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::Network, 1), 1000);
        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::Network, 2), 3000);
        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::Timeout, 1), 2000);
        QCOMPARE(OpenAILLMClient::retryDelayMs(LLMErrorCategory::Auth, 1), 0);
    }

    void testClassifyHttpAndNetworkErrors() {
        const QByteArray empty;
        // Auth
        LLMErrorInfo info = OpenAILLMClient::classifyError(401, QNetworkReply::AuthenticationRequiredError,
                                                           QStringLiteral("Unauthorized"), empty);
        QCOMPARE(info.category, LLMErrorCategory::Auth);
        QVERIFY(!info.retryable);
        QVERIFY(info.message.contains(QStringLiteral("API key")));
        QCOMPARE(info.httpStatus, 401);

        // RateLimit
        info = OpenAILLMClient::classifyError(429, QNetworkReply::UnknownContentError, QStringLiteral("Too many"),
                                              QByteArrayLiteral("{\"error\":{\"message\":\"quota\"}}"));
        QCOMPARE(info.category, LLMErrorCategory::RateLimit);
        QVERIFY(info.retryable);

        // Server（任意 5xx）
        info = OpenAILLMClient::classifyError(503, QNetworkReply::ServiceUnavailableError,
                                              QStringLiteral("unavailable"), empty);
        QCOMPARE(info.category, LLMErrorCategory::Server);
        QVERIFY(info.retryable);
        QVERIFY(info.message.contains(QStringLiteral("503")));

        // BadRequest + 上下文超限子判定
        info = OpenAILLMClient::classifyError(
            400, QNetworkReply::ProtocolInvalidOperationError, QStringLiteral("Bad Request"),
            QByteArrayLiteral("{\"error\":{\"message\":\"maximum context length exceeded\"}}"));
        QCOMPARE(info.category, LLMErrorCategory::BadRequest);
        QVERIFY(!info.retryable);
        QVERIFY(info.contextLimitSuspected);
        QVERIFY(!info.visionDowngradeHint);

        // Timeout（网络层超时）
        info = OpenAILLMClient::classifyError(0, QNetworkReply::TimeoutError, QStringLiteral("timed out"), empty);
        QCOMPARE(info.category, LLMErrorCategory::Timeout);
        QVERIFY(info.retryable);

        // Network
        info = OpenAILLMClient::classifyError(0, QNetworkReply::ConnectionRefusedError,
                                              QStringLiteral("Connection refused"), empty);
        QCOMPARE(info.category, LLMErrorCategory::Network);
        QVERIFY(info.retryable);
        QCOMPARE(info.httpStatus, 0);

        // Malformed（HTTP 200 但解析失败）
        info = OpenAILLMClient::classifyError(0, QNetworkReply::NoError, QStringLiteral("illegal value"),
                                              QByteArrayLiteral("not-json"));
        QCOMPARE(info.category, LLMErrorCategory::Malformed);
        QVERIFY(!info.retryable);
        QVERIFY(!info.detail.isEmpty());
        QVERIFY(info.detail.size() <= 2048);
    }

    void testRateLimitRetriesThenSucceeds() {
        LlmHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(429, QByteArrayLiteral("{\"error\":{\"message\":\"rate limited\"}}"));
        stub.script(200, LlmHttpStub::successBody());

        OpenAILLMClient client;
        configure(client, stub);
        ClientProbe probe;
        probe.attach(client);

        client.sendRequest(simpleConversation(), {});
        QVERIFY2(waitFor([&probe]() { return probe.responses == 1; }, 15000), "expected a successful response");

        QCOMPARE(probe.retries, 1);
        QCOMPARE(probe.errors, 0);
        QCOMPARE(stub.requestCount, 2);
        QCOMPARE(probe.lastRetryInfo.category, LLMErrorCategory::RateLimit);
        QCOMPARE(probe.lastRetryAttempt, 1);
        QCOMPARE(probe.lastRetryDelayMs, 500);
        QVERIFY(probe.lastResponse.success);
        QCOMPARE(probe.lastResponse.errorCategory, LLMErrorCategory::None);
        QCOMPARE(probe.lastResponse.promptTokens, 3);
        QCOMPARE(probe.lastResponse.completionTokens, 2);
    }

    void testServerErrorsExhaustRetries() {
        LlmHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(500, QByteArrayLiteral("{\"error\":{\"message\":\"boom\"}}"));

        OpenAILLMClient client;
        configure(client, stub);
        ClientProbe probe;
        probe.attach(client);

        client.sendRequest(simpleConversation(), {});
        QVERIFY2(waitFor([&probe]() { return probe.errors == 1; }, 30000), "expected a final classified failure");

        QCOMPARE(probe.responses, 0);
        QCOMPARE(probe.retries, 3); // 首次之外最多 3 次
        QCOMPARE(stub.requestCount, 4);
        QCOMPARE(probe.lastError.category, LLMErrorCategory::Server);
        QCOMPARE(probe.lastError.attempts, 4);
        QCOMPARE(probe.lastError.httpStatus, 500);
    }

    void testAuthNotRetried() {
        LlmHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(401, QByteArrayLiteral("{\"error\":{\"message\":\"invalid api key\"}}"));

        OpenAILLMClient client;
        configure(client, stub);
        ClientProbe probe;
        probe.attach(client);

        client.sendRequest(simpleConversation(), {});
        QVERIFY2(waitFor([&probe]() { return probe.errors == 1; }, 10000), "expected an auth failure");

        QCOMPARE(probe.retries, 0);
        QCOMPARE(stub.requestCount, 1);
        QCOMPARE(probe.lastError.category, LLMErrorCategory::Auth);
        QVERIFY(probe.lastError.message.contains(QStringLiteral("API key")));
    }

    void testBadRequestContextHintNotRetried() {
        LlmHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(400, QByteArrayLiteral("{\"error\":{\"message\":\"maximum context length exceeded\"}}"));

        OpenAILLMClient client;
        configure(client, stub);
        ClientProbe probe;
        probe.attach(client);

        client.sendRequest(simpleConversation(), {});
        QVERIFY2(waitFor([&probe]() { return probe.errors == 1; }, 10000), "expected a bad-request failure");

        QCOMPARE(probe.retries, 0);
        QCOMPARE(stub.requestCount, 1);
        QCOMPARE(probe.lastError.category, LLMErrorCategory::BadRequest);
        QVERIFY(probe.lastError.contextLimitSuspected);
        QVERIFY(!probe.lastError.retryable);
    }

    void testVisionDowngradeHint() {
        LlmHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(400, QByteArrayLiteral("{\"error\":{\"message\":\"Invalid content type for image_url\"}}"));

        OpenAILLMClient client;
        configure(client, stub);
        ClientProbe probe;
        probe.attach(client);
        QVERIFY(client.supportsImageInput()); // 降级前：默认声明支持图片

        client.sendRequest(simpleConversation(), {});
        QVERIFY2(waitFor([&probe]() { return probe.errors == 1; }, 10000), "expected a vision rejection");

        QCOMPARE(probe.lastError.category, LLMErrorCategory::BadRequest);
        QVERIFY(probe.lastError.visionDowngradeHint);
        QVERIFY(!probe.lastError.contextLimitSuspected);
        QCOMPARE(probe.retries, 0);
        // R5（设计 §6.6 / §7.5 第 8 条）：服务端拒绝图片后，本客户端会话不再附图
        QVERIFY2(!client.supportsImageInput(), "vision capability must be downgraded for this client session");
        // 用户显式重新启用视觉后恢复
        client.setVisionEnabled(true);
        QVERIFY(client.supportsImageInput());
    }

    void testMalformedResponseNotRetried() {
        LlmHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(200, QByteArrayLiteral("not-json"));

        OpenAILLMClient client;
        configure(client, stub);
        ClientProbe probe;
        probe.attach(client);

        client.sendRequest(simpleConversation(), {});
        QVERIFY2(waitFor([&probe]() { return probe.errors == 1; }, 10000), "expected a malformed-response failure");

        QCOMPARE(probe.responses, 0);
        QCOMPARE(probe.retries, 0);
        QCOMPARE(stub.requestCount, 1);
        QCOMPARE(probe.lastError.category, LLMErrorCategory::Malformed);
    }

    void testAbortCancelsPendingRetry() {
        LlmHttpStub stub;
        QVERIFY(stub.listen());
        stub.script(429, QByteArrayLiteral("{\"error\":{\"message\":\"rate limited\"}}"));

        OpenAILLMClient client;
        configure(client, stub);
        ClientProbe probe;
        probe.attach(client);

        client.sendRequest(simpleConversation(), {});
        QVERIFY2(waitFor([&probe]() { return probe.retries == 1; }, 10000), "expected a scheduled retry");
        QCOMPARE(stub.requestCount, 1);

        client.abortRequest();
        QTest::qWait(900); // 超过 500ms 退避，重试必须已被取消

        QCOMPARE(stub.requestCount, 1);
        QCOMPARE(probe.errors, 0);
        QCOMPARE(probe.responses, 0);
    }

    void testNetworkErrorRetriesTwice() {
        // 先监听再关闭：端口上无服务 → 连接被拒绝（Network 类别，2 次重试）
        quint16 port = 0;
        {
            QTcpServer probeServer;
            QVERIFY(probeServer.listen(QHostAddress::LocalHost, 0));
            port = probeServer.serverPort();
            probeServer.close();
        }

        OpenAILLMClient client;
        client.setEndpoint(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(port));
        client.setApiKey(QStringLiteral("test-key"));
        ClientProbe probe;
        probe.attach(client);

        client.sendRequest(simpleConversation(), {});
        QVERIFY2(waitFor([&probe]() { return probe.errors == 1; }, 30000), "expected a final network failure");

        QCOMPARE(probe.retries, 2);
        QCOMPARE(probe.lastError.category, LLMErrorCategory::Network);
        QCOMPARE(probe.lastError.attempts, 3);
    }
};

QTEST_MAIN(TestAgentRetry)
#include "test_agentretry.moc"
