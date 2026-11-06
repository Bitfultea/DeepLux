#include "core/agent/AgentBridge.h"
#include "core/agent/ToolSchema.h"
#include "core/platform/Platform.h"
#include "mcp/AgentBridgeClient.h"
#include "mcp/McpStdioServer.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest/QtTest>
#include <atomic>
#include <csignal>
#include <memory>

using namespace DeepLux;
using namespace DeepLux::Mcp;

namespace {

/**
 * 线程化假桥：QLocalServer 在独立线程跑事件循环，主线程里的同步客户端
 * （waitForConnected/waitForReadyRead）才能收到响应；协议与 AgentBridge v1.0 一致。
 */
class FakeBridge : public QThread {
    Q_OBJECT

public:
    explicit FakeBridge(const QString& serverName, const QString& token = QString(), QObject* parent = nullptr)
        : QThread(parent), m_serverName(serverName), m_token(token) {}

    bool startServer() {
        start();
        return m_started.tryAcquire(1, 5000) && m_listening.load();
    }

    void stopServer() {
        if (!isRunning())
            return;
        quit();
        wait(3000);
    }

    void setSilent(bool silent) {
        m_silent.store(silent);
    }
    void setHoldResponses(bool hold) {
        m_hold.store(hold);
    }
    void releaseHeldResponse() {
        m_release.release();
    }
    void setErrorResponse(const QString& message) {
        QMutexLocker locker(&m_mutex);
        m_error = message;
    }
    void setResultPayload(const QJsonObject& payload) {
        QMutexLocker locker(&m_mutex);
        m_result = payload;
    }

    int connectionCount() const {
        return m_connections.load();
    }
    int requestCount() const {
        return m_requests.load();
    }
    QStringList receivedToolNames() const {
        QMutexLocker locker(&m_mutex);
        return m_receivedTools;
    }
    QStringList receivedTypes() const {
        QMutexLocker locker(&m_mutex);
        return m_receivedTypes;
    }
    QStringList receivedOrigins() const {
        QMutexLocker locker(&m_mutex);
        return m_receivedOrigins;
    }
    QStringList receivedVersions() const {
        QMutexLocker locker(&m_mutex);
        return m_receivedVersions;
    }

protected:
    void run() override {
        QLocalServer server;
        m_listening.store(server.listen(m_serverName));
        m_started.release();
        if (!m_listening.load())
            return;

        connect(&server, &QLocalServer::newConnection, &server, [this, &server]() {
            while (QLocalSocket* socket = server.nextPendingConnection()) {
                m_connections.fetch_add(1);
                auto buffer = std::make_shared<QByteArray>();
                auto authenticated = std::make_shared<bool>(m_token.isEmpty());
                connect(socket, &QLocalSocket::readyRead, socket, [this, socket, buffer, authenticated]() {
                    buffer->append(socket->readAll());
                    int index = -1;
                    while ((index = buffer->indexOf('\n')) >= 0) {
                        const QByteArray line = buffer->left(index);
                        buffer->remove(0, index + 1);
                        if (line.trimmed().isEmpty())
                            continue;
                        m_requests.fetch_add(1);
                        handleFrame(socket, QJsonDocument::fromJson(line).object(), authenticated.get());
                    }
                });
                connect(socket, &QLocalSocket::disconnected, socket, [socket]() { socket->deleteLater(); });
            }
        });

        exec();
        server.close();
    }

private:
    void sendFrame(QLocalSocket* socket, const QString& type, const QJsonValue& id, const QJsonObject& payload) {
        // 取消测试会在假桥持有期间关闭客户端连接；不要向已断开套接字写入
        if (!socket || socket->state() != QLocalSocket::ConnectedState)
            return;
        const QJsonObject frame{{QStringLiteral("version"), QStringLiteral("1.0")},
                                {QStringLiteral("type"), type},
                                {QStringLiteral("id"), id},
                                {QStringLiteral("payload"), payload}};
        socket->write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
    }

    void handleFrame(QLocalSocket* socket, const QJsonObject& frame, bool* authenticated) {
        const QString type = frame.value(QStringLiteral("type")).toString();
        {
            QMutexLocker locker(&m_mutex);
            m_receivedTypes << type;
        }

        if (!m_token.isEmpty() && !*authenticated) {
            if (type == QStringLiteral("auth") &&
                frame.value(QStringLiteral("payload")).toObject().value(QStringLiteral("token")).toString() ==
                    m_token) {
                *authenticated = true;
                sendFrame(socket, QStringLiteral("result"), frame.value(QStringLiteral("id")),
                          QJsonObject{{QStringLiteral("status"), QStringLiteral("authenticated")}});
            } else {
                sendFrame(socket, QStringLiteral("error"), frame.value(QStringLiteral("id")),
                          QJsonObject{{QStringLiteral("message"), QStringLiteral("Invalid auth token")}});
            }
            return;
        }

        if (type != QStringLiteral("tool_call"))
            return;

        const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
        {
            QMutexLocker locker(&m_mutex);
            m_receivedTools << payload.value(QStringLiteral("tool")).toString();
            m_receivedOrigins << payload.value(QStringLiteral("origin")).toString();
            m_receivedVersions << frame.value(QStringLiteral("version")).toString();
        }
        if (m_silent.load())
            return;
        // 取消测试：挂起响应，直到测试显式释放（最多 5s 防挂死）
        if (m_hold.load())
            m_release.tryAcquire(1, 5000);

        QString error;
        QJsonObject resultPayload;
        {
            QMutexLocker locker(&m_mutex);
            error = m_error;
            resultPayload = m_result;
        }
        if (!error.isEmpty()) {
            sendFrame(socket, QStringLiteral("error"), frame.value(QStringLiteral("id")),
                      QJsonObject{{QStringLiteral("message"), error}});
            return;
        }
        sendFrame(socket, QStringLiteral("result"), frame.value(QStringLiteral("id")), resultPayload);
    }

    QString m_serverName;
    QString m_token;
    QSemaphore m_started;
    std::atomic_bool m_listening{false};
    std::atomic_bool m_silent{false};
    std::atomic_bool m_hold{false};
    QSemaphore m_release;
    std::atomic_int m_connections{0};
    std::atomic_int m_requests{0};
    mutable QMutex m_mutex;
    QString m_error;
    QJsonObject m_result;
    QStringList m_receivedTools;
    QStringList m_receivedTypes;
    QStringList m_receivedOrigins;
    QStringList m_receivedVersions;
};

QString uniqueBridgeName() {
    return QStringLiteral("deeplux_mcp_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

} // namespace

class TestMcpBridge : public QObject {
    Q_OBJECT

private:
    static QByteArray frame(const QJsonObject& request) {
        return QJsonDocument(request).toJson(QJsonDocument::Compact);
    }

    static QByteArray rpc(McpStdioServer& server, const QJsonObject& request) {
        return server.processLine(frame(request));
    }

    static QJsonObject parse(const QByteArray& line) {
        return QJsonDocument::fromJson(line).object();
    }

    static QJsonObject toolsCall(McpStdioServer& server, const QString& name,
                                 const QJsonObject& arguments = QJsonObject(), const QJsonValue& id = 1) {
        return parse(rpc(
            server, QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                {QStringLiteral("id"), id},
                                {QStringLiteral("method"), QStringLiteral("tools/call")},
                                {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), name},
                                                                       {QStringLiteral("arguments"), arguments}}}}));
    }

    static QJsonObject resultOf(const QJsonObject& response) {
        return response.value(QStringLiteral("result")).toObject();
    }

    static QJsonObject errorOf(const QJsonObject& response) {
        return response.value(QStringLiteral("error")).toObject();
    }

    static QJsonObject textPayloadOf(const QJsonObject& toolResult) {
        const QJsonArray content = toolResult.value(QStringLiteral("content")).toArray();
        if (content.isEmpty())
            return QJsonObject();
        return parse(content.first().toObject().value(QStringLiteral("text")).toString().toUtf8());
    }

    static void configureBridge(McpStdioServer& server, const QString& socketName, const QString& token = QString(),
                                int timeoutMs = 5000) {
        server.bridgeClient()->setSocketCandidates(QStringList{socketName});
        server.bridgeClient()->setAuthToken(token);
        server.bridgeClient()->setOperationTimeoutMs(timeoutMs);
    }

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
#ifndef Q_OS_WIN
        // 取消测试中假桥可能向已关闭套接字写入；测试进程忽略 SIGPIPE
        std::signal(SIGPIPE, SIG_IGN);
#endif
    }

    void testInitializeAndNotification() {
        McpStdioServer server;
        const QJsonObject response = parse(
            rpc(server, QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                    {QStringLiteral("id"), 7},
                                    {QStringLiteral("method"), QStringLiteral("initialize")},
                                    {QStringLiteral("params"),
                                     QJsonObject{{QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05")}}}}));
        QCOMPARE(response.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
        QCOMPARE(response.value(QStringLiteral("id")).toInt(), 7);
        const QJsonObject result = resultOf(response);
        QCOMPARE(result.value(QStringLiteral("protocolVersion")).toString(), QStringLiteral("2024-11-05"));
        const QJsonObject serverInfo = result.value(QStringLiteral("serverInfo")).toObject();
        QCOMPARE(serverInfo.value(QStringLiteral("name")).toString(), QStringLiteral("deeplux-mcp-bridge"));
        QCOMPARE(serverInfo.value(QStringLiteral("version")).toString(), QStringLiteral(DEEPLUX_VERSION_STRING));
        QCOMPARE(result.value(QStringLiteral("capabilities"))
                     .toObject()
                     .value(QStringLiteral("tools"))
                     .toObject()
                     .value(QStringLiteral("listChanged"))
                     .toBool(),
                 false);

        // 通知不得产生响应
        QVERIFY(server.processLine("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}").isEmpty());
    }

    void testToolsList() {
        McpStdioServer server;
        const QJsonObject response =
            parse(rpc(server, QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                          {QStringLiteral("id"), QStringLiteral("list-1")},
                                          {QStringLiteral("method"), QStringLiteral("tools/list")}}));
        const QJsonArray tools = resultOf(response).value(QStringLiteral("tools")).toArray();
        const QList<ToolDefinition> definitions = ToolSchema::instance().allTools();
        QCOMPARE(tools.size(), definitions.size());
        QCOMPARE(tools.size(), 25);

        QMap<QString, QJsonObject> byName;
        for (const QJsonValue& value : tools) {
            const QJsonObject tool = value.toObject();
            byName.insert(tool.value(QStringLiteral("name")).toString(), tool);
            QVERIFY(!tool.value(QStringLiteral("description")).toString().trimmed().isEmpty());
            QVERIFY(tool.value(QStringLiteral("inputSchema")).isObject());
            QVERIFY(tool.value(QStringLiteral("annotations")).isObject());
        }
        QStringList schemaNames;
        for (const ToolDefinition& definition : definitions)
            schemaNames << definition.name;
        schemaNames.sort();
        QStringList listedNames = byName.keys();
        QCOMPARE(listedNames, schemaNames);

        // annotations 单源映射：readOnlyHint=readOnly、destructiveHint=dangerous
        const QJsonObject readOnlyTool = byName.value(QStringLiteral("get_flow_state"));
        QCOMPARE(
            readOnlyTool.value(QStringLiteral("annotations")).toObject().value(QStringLiteral("readOnlyHint")).toBool(),
            true);
        QCOMPARE(readOnlyTool.value(QStringLiteral("annotations"))
                     .toObject()
                     .value(QStringLiteral("destructiveHint"))
                     .toBool(),
                 false);
        const QJsonObject writeTool = byName.value(QStringLiteral("add_module"));
        QCOMPARE(
            writeTool.value(QStringLiteral("annotations")).toObject().value(QStringLiteral("readOnlyHint")).toBool(),
            false);
        const QJsonObject dangerousTool = byName.value(QStringLiteral("remove_module"));
        QCOMPARE(dangerousTool.value(QStringLiteral("annotations"))
                     .toObject()
                     .value(QStringLiteral("destructiveHint"))
                     .toBool(),
                 true);
        QCOMPARE(dangerousTool.value(QStringLiteral("annotations"))
                     .toObject()
                     .value(QStringLiteral("readOnlyHint"))
                     .toBool(),
                 false);
    }

    void testPingDoesNotTouchBridge() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name);
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name);
        const QJsonObject response =
            parse(rpc(server, QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                          {QStringLiteral("id"), 3},
                                          {QStringLiteral("method"), QStringLiteral("ping")}}));
        QVERIFY(resultOf(response).isEmpty());
        QCOMPARE(bridge.connectionCount(), 0);
        QCOMPARE(bridge.requestCount(), 0);
        bridge.stopServer();
    }

    void testToolCallForwardFrame() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name);
        bridge.setResultPayload(QJsonObject{{QStringLiteral("modules"), QJsonArray()}});
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name);
        const QJsonObject response = toolsCall(server, QStringLiteral("get_flow_state"));
        const QJsonObject result = resultOf(response);
        QCOMPARE(result.value(QStringLiteral("isError")).toBool(), false);
        QCOMPARE(result.value(QStringLiteral("content")).toArray().size(), 1);
        QCOMPARE(result.value(QStringLiteral("content"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("type"))
                     .toString(),
                 QStringLiteral("text"));
        QVERIFY(textPayloadOf(result).contains(QStringLiteral("modules")));

        QCOMPARE(bridge.receivedToolNames(), QStringList{QStringLiteral("get_flow_state")});
        QCOMPARE(bridge.receivedTypes(), (QStringList{QStringLiteral("tool_call")}));
        QCOMPARE(bridge.receivedOrigins().first(), QStringLiteral("mcp"));
        QCOMPARE(bridge.receivedVersions().first(), QStringLiteral("1.0"));
        QCOMPARE(bridge.requestCount(), 1);
        bridge.stopServer();
    }

    void testToolErrorIsError() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name);
        bridge.setResultPayload(QJsonObject{{QStringLiteral("error"), QStringLiteral("No project opened")}});
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name);
        const QJsonObject result = resultOf(toolsCall(server, QStringLiteral("get_flow_state")));
        QCOMPARE(result.value(QStringLiteral("isError")).toBool(), true);
        QVERIFY(textPayloadOf(result)
                    .value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("No project opened")));
        bridge.stopServer();
    }

    void testControllerStillAuthoritative() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name);
        bridge.setResultPayload(
            QJsonObject{{QStringLiteral("error"), QStringLiteral("Agent is busy processing another request")}});
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name);
        const QJsonObject result = resultOf(toolsCall(server, QStringLiteral("validate_workflow")));
        QCOMPARE(result.value(QStringLiteral("isError")).toBool(), true);
        QVERIFY(textPayloadOf(result).value(QStringLiteral("error")).toString().contains(QStringLiteral("busy")));
        bridge.stopServer();
    }

    void testWriteToolDeniedAndNotForwarded() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name);
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name);
        for (const QString& tool :
             {QStringLiteral("add_module"), QStringLiteral("run_flow"), QStringLiteral("save_project"),
              QStringLiteral("remove_module"), QStringLiteral("tune_node_and_verify")}) {
            const QJsonObject result = resultOf(toolsCall(server, tool));
            QVERIFY2(!result.isEmpty(), qPrintable(tool));
            QCOMPARE(result.value(QStringLiteral("isError")).toBool(), true);
            const QJsonObject payload = textPayloadOf(result);
            QCOMPARE(payload.value(QStringLiteral("code")).toString(), QStringLiteral("permission_denied"));
            QVERIFY(payload.value(QStringLiteral("hint")).toString().contains(QStringLiteral("GUI")));
        }
        // 核心不变量：写工具不建立 IPC 连接、不转发
        QCOMPARE(bridge.connectionCount(), 0);
        QCOMPARE(bridge.requestCount(), 0);
        bridge.stopServer();
    }

    void testReadOnlyToolsAllowed() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name);
        bridge.setResultPayload(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}});
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name);
        QStringList readOnlyTools;
        for (const ToolDefinition& definition : ToolSchema::instance().allTools()) {
            if (definition.readOnly)
                readOnlyTools << definition.name;
        }
        QVERIFY2(readOnlyTools.size() >= 10, qPrintable(QString::number(readOnlyTools.size())));
        for (const QString& tool : readOnlyTools) {
            const QJsonObject response = toolsCall(server, tool);
            QVERIFY2(!response.contains(QStringLiteral("error")), qPrintable(tool));
            QCOMPARE(resultOf(response).value(QStringLiteral("isError")).toBool(), false);
        }
        QStringList forwarded = bridge.receivedToolNames();
        forwarded.sort();
        readOnlyTools.sort();
        QCOMPARE(forwarded, readOnlyTools);
        bridge.stopServer();
    }

    void testUnknownToolNotPermissionDenied() {
        McpStdioServer server;
        const QJsonObject unknown = toolsCall(server, QStringLiteral("definitely_not_a_tool"));
        QCOMPARE(errorOf(unknown).value(QStringLiteral("code")).toInt(), -32602);
        QVERIFY(errorOf(unknown).value(QStringLiteral("message")).toString().contains(QStringLiteral("unknown tool")));

        const QJsonObject missingName =
            parse(rpc(server, QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                          {QStringLiteral("id"), 2},
                                          {QStringLiteral("method"), QStringLiteral("tools/call")},
                                          {QStringLiteral("params"), QJsonObject()}}));
        QCOMPARE(errorOf(missingName).value(QStringLiteral("code")).toInt(), -32602);
    }

    void testGuiNotRunning() {
        McpStdioServer server;
        configureBridge(server, uniqueBridgeName(), QString(), 2000);
        const QJsonObject response = toolsCall(server, QStringLiteral("get_flow_state"));
        QCOMPARE(errorOf(response).value(QStringLiteral("code")).toInt(), -32001);
        QVERIFY(errorOf(response).value(QStringLiteral("message")).toString().contains(QStringLiteral("not running")));
    }

    void testTimeout() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name);
        bridge.setSilent(true);
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name, QString(), 100);
        QElapsedTimer timer;
        timer.start();
        const QJsonObject response = toolsCall(server, QStringLiteral("get_flow_state"));
        QCOMPARE(errorOf(response).value(QStringLiteral("code")).toInt(), -32002);
        QVERIFY(timer.elapsed() < 3000);
        QVERIFY(errorOf(response).value(QStringLiteral("message")).toString().contains(QStringLiteral("timed out")));
        bridge.stopServer();
    }

    void testAuthHandshake() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name, QStringLiteral("secret-token"));
        bridge.setResultPayload(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}});
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name, QStringLiteral("wrong-token"), 3000);
        const QJsonObject denied = toolsCall(server, QStringLiteral("get_flow_state"));
        QCOMPARE(errorOf(denied).value(QStringLiteral("code")).toInt(), -32003);
        QVERIFY(!errorOf(denied).value(QStringLiteral("message")).toString().contains(QStringLiteral("secret-token")));

        server.bridgeClient()->setAuthToken(QStringLiteral("secret-token"));
        const QJsonObject allowed = toolsCall(server, QStringLiteral("get_flow_state"));
        QVERIFY2(!allowed.contains(QStringLiteral("error")),
                 qPrintable(QString::fromUtf8(QJsonDocument(allowed).toJson(QJsonDocument::Compact))));
        QCOMPARE(resultOf(allowed).value(QStringLiteral("isError")).toBool(), false);
        QVERIFY(bridge.receivedTypes().contains(QStringLiteral("auth")));
        QVERIFY(bridge.receivedTypes().contains(QStringLiteral("tool_call")));
        bridge.stopServer();
    }

    void testParseErrorAndMethodNotFound() {
        McpStdioServer server;
        const QJsonObject parseError = parse(server.processLine("{this is not json"));
        QCOMPARE(errorOf(parseError).value(QStringLiteral("code")).toInt(), -32700);
        QVERIFY(parseError.value(QStringLiteral("id")).isNull());

        const QJsonObject invalidRequest = parse(server.processLine("[1,2,3]"));
        QCOMPARE(errorOf(invalidRequest).value(QStringLiteral("code")).toInt(), -32600);

        const QJsonObject missingVersion = parse(server.processLine("{\"id\":1,\"method\":\"ping\"}"));
        QCOMPARE(errorOf(missingVersion).value(QStringLiteral("code")).toInt(), -32600);

        const QJsonObject notFound =
            parse(server.processLine("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"no/such/method\"}"));
        QCOMPARE(errorOf(notFound).value(QStringLiteral("code")).toInt(), -32601);
        QVERIFY(
            errorOf(notFound).value(QStringLiteral("message")).toString().contains(QStringLiteral("no/such/method")));
    }

    void testStdoutPurity() {
        McpStdioServer server;
        const QByteArray line = server.processLine("{\"jsonrpc\":\"2.0\",\"id\":\"p\",\"method\":\"ping\"}");
        QCOMPARE(line.count('\n'), 1);
        QVERIFY(line.endsWith('\n'));
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line.trimmed(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        QCOMPARE(document.object().value(QStringLiteral("id")).toString(), QStringLiteral("p"));
    }

    // ---------- B4：异步取消 / 分帧边界 / 队列上限 ----------

    void testAsyncCancelSuppressesLateResponse() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name);
        bridge.setResultPayload(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}});
        bridge.setHoldResponses(true);
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name, QString(), 3000);
        QSignalSpy responses(&server, &McpStdioServer::responseReady);

        server.ingestLine(frame(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), QStringLiteral("held-1")},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                                   {QStringLiteral("arguments"), QJsonObject()}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(bridge.requestCount(), 1, 3000);
        QCOMPARE(responses.count(), 0);

        // 取消必须在假桥释放前被消费；随后的 ping 证明事件循环仍在处理输入
        server.ingestLine("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
                          "\"params\":{\"requestId\":\"held-1\"}}");
        server.ingestLine("{\"jsonrpc\":\"2.0\",\"id\":\"ping-1\",\"method\":\"ping\"}");
        QTRY_COMPARE_WITH_TIMEOUT(responses.count(), 1, 3000);
        QCOMPARE(parse(responses.at(0).at(0).toByteArray()).value(QStringLiteral("id")).toString(),
                 QStringLiteral("ping-1"));

        // 释放迟到结果：不得输出
        bridge.setHoldResponses(false);
        bridge.releaseHeldResponse();
        QTest::qWait(300);
        QCOMPARE(responses.count(), 1);

        // 下一请求不受影响
        bridge.setResultPayload(QJsonObject{{QStringLiteral("status"), QStringLiteral("next")}});
        server.ingestLine(frame(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), QStringLiteral("next-1")},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                                   {QStringLiteral("arguments"), QJsonObject()}}}}));
        QTRY_COMPARE_WITH_TIMEOUT(responses.count(), 2, 3000);
        QCOMPARE(parse(responses.at(1).at(0).toByteArray()).value(QStringLiteral("id")).toString(),
                 QStringLiteral("next-1"));
        bridge.stopServer();
    }

    void testFramingRecoveryAndBounds() {
        McpStdioServer server;
        QSignalSpy responses(&server, &McpStdioServer::responseReady);

        // 错误帧后同一 chunk 中的合法 ping 仍可解析
        server.ingestChunk("{not-json}\n{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}\n");
        QCOMPARE(responses.count(), 2);
        QCOMPARE(parse(responses.at(0).at(0).toByteArray())
                     .value(QStringLiteral("error"))
                     .toObject()
                     .value(QStringLiteral("code"))
                     .toInt(),
                 -32700);
        QCOMPARE(parse(responses.at(1).at(0).toByteArray()).value(QStringLiteral("id")).toInt(), 9);

        // 超长无换行帧：有界丢弃，随后恢复
        responses.clear();
        server.ingestChunk(QByteArray(McpStdioServer::kMaxFrameBytes + 16, 'x'));
        QCOMPARE(responses.count(), 1);
        server.ingestChunk("\n");
        QCOMPARE(responses.count(), 1);
        server.ingestChunk("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"ping\"}\n");
        QCOMPARE(responses.count(), 2);
        QCOMPARE(parse(responses.at(1).at(0).toByteArray()).value(QStringLiteral("id")).toInt(), 10);

        // EOF 半帧：不产生响应
        responses.clear();
        server.ingestChunk("{\"jsonrpc\":\"2.0\",\"id\":11,");
        server.finishInput();
        QCOMPARE(responses.count(), 0);

        // id:null 是请求；无 id 是通知
        server.ingestChunk("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}\n");
        QCOMPARE(responses.count(), 1);
        const QJsonObject nullId = parse(responses.at(0).at(0).toByteArray());
        QVERIFY(nullId.contains(QStringLiteral("id")));
        QVERIFY(nullId.value(QStringLiteral("id")).isNull());
        server.ingestChunk("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
        QCOMPARE(responses.count(), 1);
    }

    void testQueueIsBounded() {
        const QString name = uniqueBridgeName();
        FakeBridge bridge(name);
        bridge.setResultPayload(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}});
        bridge.setHoldResponses(true);
        QVERIFY(bridge.startServer());

        McpStdioServer server;
        configureBridge(server, name, QString(), 3000);
        server.setQueueLimitsForTest(1, 1024 * 1024);
        QSignalSpy responses(&server, &McpStdioServer::responseReady);

        const auto call = [&server](const QString& id) {
            server.ingestLine(frame(QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), id},
                {QStringLiteral("method"), QStringLiteral("tools/call")},
                {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                                       {QStringLiteral("arguments"), QJsonObject()}}}}));
        };
        call(QStringLiteral("in-flight"));
        QTRY_COMPARE_WITH_TIMEOUT(bridge.requestCount(), 1, 3000);
        call(QStringLiteral("queued"));
        call(QStringLiteral("overflow"));

        QTRY_COMPARE_WITH_TIMEOUT(responses.count(), 1, 3000);
        const QJsonObject overflow = parse(responses.at(0).at(0).toByteArray());
        QCOMPARE(overflow.value(QStringLiteral("id")).toString(), QStringLiteral("overflow"));
        QCOMPARE(overflow.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32009);

        bridge.setHoldResponses(false);
        bridge.releaseHeldResponse();
        QTRY_COMPARE_WITH_TIMEOUT(responses.count(), 3, 5000);
        bridge.stopServer();
    }

    // 真实子进程黑盒：stdin 分帧、stdout 协议纯度、只读转发、写拒绝、取消
    void testRealBridgeSubprocess() {
#if defined(Q_OS_WIN)
        QSKIP("Windows named-pipe subprocess test requires an isolated pipe name");
#else
        QTemporaryDir dataDir;
        QVERIFY(dataDir.isValid());
        const QString socketPath = dataDir.filePath(QStringLiteral("agent.sock"));

        FakeBridge bridge(socketPath);
        bridge.setResultPayload(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}});
        QVERIFY(bridge.startServer());
        struct StopOnExit {
            FakeBridge& bridge;
            ~StopOnExit() {
                bridge.stopServer();
            }
        } stopOnExit{bridge};

        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("DEEPLUX_APP_DATA_DIR"), dataDir.path());
        process.setProcessEnvironment(environment);
        process.start(QStringLiteral(DEEPLUX_MCP_BRIDGE_PATH), QStringList());
        QVERIFY2(process.waitForStarted(5000), qPrintable(process.errorString()));

        const auto send = [&process](const QJsonObject& request) {
            process.write(frame(request) + '\n');
            process.waitForBytesWritten(2000);
        };
        const auto readResponse = [&process]() -> QJsonObject {
            QElapsedTimer timer;
            timer.start();
            while (!process.canReadLine() && timer.elapsed() < 5000) {
                if (!process.waitForReadyRead(200) && process.state() == QProcess::NotRunning)
                    break;
            }
            const QByteArray line = process.readLine();
            if (line.trimmed().isEmpty()) {
                qWarning() << "[subprocess-test] no response; state" << process.state() << "error"
                           << process.errorString() << "stderr" << QString::fromUtf8(process.readAllStandardError());
                return QJsonObject();
            }
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed(), &error);
            return error.error == QJsonParseError::NoError ? doc.object() : QJsonObject();
        };

        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 1},
                         {QStringLiteral("method"), QStringLiteral("initialize")},
                         {QStringLiteral("params"), QJsonObject()}});
        QCOMPARE(readResponse().value(QStringLiteral("id")).toInt(), 1);

        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 2},
                         {QStringLiteral("method"), QStringLiteral("ping")}});
        QCOMPARE(readResponse().value(QStringLiteral("id")).toInt(), 2);

        send(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), 3},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                                   {QStringLiteral("arguments"), QJsonObject()}}}});
        const QJsonObject readResult = parse(readResponse()
                                                 .value(QStringLiteral("result"))
                                                 .toObject()
                                                 .value(QStringLiteral("content"))
                                                 .toArray()
                                                 .first()
                                                 .toObject()
                                                 .value(QStringLiteral("text"))
                                                 .toString()
                                                 .toUtf8());
        QCOMPARE(readResult.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 4},
                         {QStringLiteral("method"), QStringLiteral("tools/call")},
                         {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("add_module")},
                                                                {QStringLiteral("arguments"), QJsonObject()}}}});
        const QJsonObject denied = readResponse().value(QStringLiteral("result")).toObject();
        QCOMPARE(denied.value(QStringLiteral("isError")).toBool(), true);
        QCOMPARE(bridge.requestCount(), 1); // 写工具未转发

        // 挂起 + 取消：ping 先返回，迟到结果不得输出
        bridge.setHoldResponses(true);
        send(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), 5},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                                   {QStringLiteral("arguments"), QJsonObject()}}}});
        QTRY_COMPARE_WITH_TIMEOUT(bridge.requestCount(), 2, 3000);

        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("method"), QStringLiteral("notifications/cancelled")},
                         {QStringLiteral("params"), QJsonObject{{QStringLiteral("requestId"), 5}}}});
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 6},
                         {QStringLiteral("method"), QStringLiteral("ping")}});
        QCOMPARE(readResponse().value(QStringLiteral("id")).toInt(), 6);

        bridge.setHoldResponses(false);
        bridge.releaseHeldResponse();
        bool lateSeen = false;
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 400) {
            if (!process.waitForReadyRead(100))
                continue;
            const QList<QByteArray> lines = process.readAll().split('\n');
            for (const QByteArray& line : lines) {
                if (line.trimmed().isEmpty())
                    continue;
                const QJsonObject object = parse(line);
                if (object.value(QStringLiteral("id")).toInt() == 5)
                    lateSeen = true;
            }
        }
        QVERIFY2(!lateSeen, "cancelled MCP request must not emit a late response");
        QCOMPARE(bridge.requestCount(), 2);

        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        bridge.stopServer();
#endif
    }

    void testSocketCandidatesMatchBridge() {
        AgentBridge& bridge = AgentBridge::instance();
        bridge.stop();
        if (!bridge.start())
            QSKIP("AgentBridge named pipe unavailable (another DeepLux instance may be running)");
        const QStringList candidates = AgentBridgeClient::defaultSocketCandidates();
        QVERIFY2(candidates.contains(bridge.serverName()), qPrintable(candidates.join(QStringLiteral(","))));
        bridge.stop();
    }
};

QTEST_GUILESS_MAIN(TestMcpBridge)
#include "test_mcpbridge.moc"
