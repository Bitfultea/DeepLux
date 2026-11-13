#include "core/agent/OpenAILLMClient.h"
#include "core/agent/ToolSchema.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSharedPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

using namespace DeepLux;

class TestOpenAILLMClient : public QObject {
    Q_OBJECT

private slots:
    void networkErrorFromOldReplyCannotConsumeNextRequest() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        bool secondSent = false;
        const QByteArray secondResponse =
            QJsonDocument(
                QJsonObject{{QStringLiteral("choices"),
                             QJsonArray{QJsonObject{{QStringLiteral("message"),
                                                     QJsonObject{{QStringLiteral("content"),
                                                                  QStringLiteral("second request survived")}}}}}}})
                .toJson(QJsonDocument::Compact);
        int requestsSeen = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                const auto requestBuffer = QSharedPointer<QByteArray>::create();
                const auto handled = QSharedPointer<bool>::create(false);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, requestBuffer, handled]() {
                    if (*handled)
                        return;
                    requestBuffer->append(socket->readAll());
                    const int headerEnd = requestBuffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    int contentLength = 0;
                    for (const QByteArray& header : requestBuffer->left(headerEnd).split('\n')) {
                        const QByteArray normalized = header.trimmed().toLower();
                        if (normalized.startsWith("content-length:"))
                            contentLength = normalized.mid(normalized.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (requestBuffer->size() < headerEnd + 4 + contentLength)
                        return;
                    *handled = true;
                    ++requestsSeen;
                    if (!secondSent) {
                        socket->abort();
                        return;
                    }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(secondResponse.size()) + "\r\nConnection: close\r\n\r\n" +
                                  secondResponse);
                    socket->disconnectFromHost();
                });
            }
        });

        OpenAILLMClient client;
        client.setEndpoint(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort()));
        client.setToolsEnabled(false);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        int failures = 0;
        AgentResponse response;
        connect(&client, &ILLMClient::errorOccurred, &loop, [&](const QString&) {
            if (secondSent)
                return;
            secondSent = true;
            AgentConversation second;
            second.requestId = QStringLiteral("request-B");
            client.sendRequest(second, {});
        });
        connect(&client, &ILLMClient::requestFailed, &loop, [&](const QString& requestId, const QString&) {
            QCOMPARE(requestId, QStringLiteral("request-A"));
            ++failures;
        });
        connect(&client, &ILLMClient::responseReceived, &loop, [&](const AgentResponse& result) {
            response = result;
            loop.quit();
        });

        AgentConversation first;
        first.requestId = QStringLiteral("request-A");
        client.sendRequest(first, {});
        timeout.start(5000);
        loop.exec();

        QVERIFY(requestsSeen >= 2);
        QVERIFY(secondSent);
        QCOMPARE(failures, 1);
        QCOMPARE(response.requestId, QStringLiteral("request-B"));
        QCOMPARE(response.content, QStringLiteral("second request survived"));
    }

    void openAiCompatibleRequestAndToolResponseRoundTrip() {
        ToolSchema::instance().registerDefaultTools();
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        const QJsonObject responseArguments{{QStringLiteral("runId"), QStringLiteral("run-17")},
                                            {QStringLiteral("instanceId"), QStringLiteral("measure-1")},
                                            {QStringLiteral("preview"), true}};
        const QJsonObject responseFunction{
            {QStringLiteral("name"), QStringLiteral("get_module_output")},
            {QStringLiteral("arguments"),
             QString::fromUtf8(QJsonDocument(responseArguments).toJson(QJsonDocument::Compact))}};
        const QJsonObject responseToolCall{{QStringLiteral("id"), QStringLiteral("call_fixture_1")},
                                           {QStringLiteral("type"), QStringLiteral("function")},
                                           {QStringLiteral("function"), responseFunction}};
        const QJsonObject responseMessage{
            {QStringLiteral("role"), QStringLiteral("assistant")},
            {QStringLiteral("content"), QStringLiteral("Need the current run output.")},
            {QStringLiteral("reasoning_content"), QStringLiteral("inspect the output first")},
            {QStringLiteral("tool_calls"), QJsonArray{responseToolCall}}};
        const QByteArray responseBody =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), responseMessage}}}},
                    {QStringLiteral("usage"),
                     QJsonObject{{QStringLiteral("prompt_tokens"), 37}, {QStringLiteral("completion_tokens"), 12}}}})
                .toJson(QJsonDocument::Compact);
        QByteArray requestBytes;
        QByteArray networkError;
        AgentResponse response;
        bool responseReceived = false;
        bool fixtureResponded = false;

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&server, &QTcpServer::newConnection, &loop, [&]() {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, &loop, [&, socket]() {
                    requestBytes.append(socket->readAll());
                    if (fixtureResponded)
                        return;

                    const int headerEnd = requestBytes.indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    int contentLength = -1;
                    for (const QByteArray& header : requestBytes.left(headerEnd).split('\n')) {
                        const QByteArray normalized = header.trimmed().toLower();
                        if (normalized.startsWith("content-length:"))
                            contentLength = normalized.mid(normalized.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (contentLength < 0 || requestBytes.size() < headerEnd + 4 + contentLength)
                        return;

                    fixtureResponded = true;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(responseBody.size()) + "\r\nConnection: close\r\n\r\n" +
                                  responseBody);
                    socket->disconnectFromHost();
                });
            }
        });

        OpenAILLMClient client;
        client.setEndpoint(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort()));
        client.setApiKey(QStringLiteral("fixture-key"));
        client.setModel(QStringLiteral("fixture-model"));
        client.setTemperature(0.2);
        client.setMaxTokens(321);
        client.setToolsEnabled(true);
        client.setVisionEnabled(true);
        client.setReasoningEffort(QStringLiteral("high"));
        client.setReasoningEcho(ReasoningEcho::Full);

        connect(&client, &ILLMClient::responseReceived, &loop, [&](const AgentResponse& result) {
            response = result;
            responseReceived = true;
            loop.quit();
        });
        connect(&client, &ILLMClient::errorOccurred, &loop, [&](const QString& error) {
            networkError = error.toUtf8();
            loop.quit();
        });

        AgentConversation conversation;
        conversation.systemPrompt = QStringLiteral("Fixture system prompt");
        conversation.requestId = QStringLiteral("fixture-request-17");
        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("Inspect this image");
        user.images.append({QByteArray::fromHex("89504e470d0a1a0a"), QStringLiteral("image/png"), {}});
        conversation.messages.append(user);
        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        assistant.content = QStringLiteral("Checking the output.");
        assistant.reasoningContent = QStringLiteral("preserve this reasoning field");
        assistant.toolCalls.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("call_previous")},
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                                     {QStringLiteral("arguments"), QStringLiteral("{}")}}}});
        conversation.messages.append(assistant);
        AgentMessage tool;
        tool.role = QStringLiteral("tool");
        tool.toolCallId = QStringLiteral("call_previous");
        tool.content = QStringLiteral("flow state result");
        conversation.messages.append(tool);

        const ToolDefinition outputTool = ToolSchema::instance().findTool(QStringLiteral("get_module_output"));
        QVERIFY(!outputTool.name.isEmpty());
        client.sendRequest(conversation, {outputTool});
        timeout.start(5000);
        loop.exec();

        QVERIFY2(networkError.isEmpty(), networkError.constData());
        QVERIFY2(responseReceived, "provider fixture timed out before a response was parsed");
        QCOMPARE(response.requestId, QStringLiteral("fixture-request-17"));
        QVERIFY(fixtureResponded);
        QVERIFY(requestBytes.startsWith("POST /v1/chat/completions HTTP/1.1\r\n"));
        QVERIFY(requestBytes.toLower().contains("content-type: application/json"));
        QVERIFY(requestBytes.toLower().contains("authorization: bearer fixture-key"));

        const int bodyStart = requestBytes.indexOf("\r\n\r\n");
        QVERIFY(bodyStart >= 0);
        const QJsonDocument request = QJsonDocument::fromJson(requestBytes.mid(bodyStart + 4));
        QVERIFY(request.isObject());
        const QJsonObject body = request.object();
        QVERIFY(!body.contains(QStringLiteral("requestId")));
        QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("fixture-model"));
        QCOMPARE(body.value(QStringLiteral("temperature")).toDouble(), 0.2);
        QCOMPARE(body.value(QStringLiteral("max_tokens")).toInt(), 321);
        QCOMPARE(body.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("high"));
        QCOMPARE(body.value(QStringLiteral("tool_choice")).toString(), QStringLiteral("auto"));

        const QJsonArray sentTools = body.value(QStringLiteral("tools")).toArray();
        QCOMPARE(sentTools.size(), 1);
        const QJsonObject sentTool = sentTools.first().toObject();
        QCOMPARE(sentTool.value(QStringLiteral("type")).toString(), QStringLiteral("function"));
        const QJsonObject function = sentTool.value(QStringLiteral("function")).toObject();
        QCOMPARE(function.value(QStringLiteral("name")).toString(), QStringLiteral("get_module_output"));
        QCOMPARE(sentTool, outputTool.toOpenAIFunction());
        const QJsonObject parameters = function.value(QStringLiteral("parameters")).toObject();
        QCOMPARE(parameters.value(QStringLiteral("type")).toString(), QStringLiteral("object"));
        QCOMPARE(parameters.value(QStringLiteral("properties"))
                     .toObject()
                     .value(QStringLiteral("runId"))
                     .toObject()
                     .value(QStringLiteral("type"))
                     .toString(),
                 QStringLiteral("string"));
        QVERIFY(parameters.value(QStringLiteral("required")).toArray().contains(QStringLiteral("runId")));
        QVERIFY(parameters.value(QStringLiteral("required")).toArray().contains(QStringLiteral("instanceId")));

        const QJsonArray sentMessages = body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(sentMessages.size(), 4);
        QCOMPARE(sentMessages.at(0).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("system"));
        QCOMPARE(sentMessages.at(0).toObject().value(QStringLiteral("content")).toString(),
                 QStringLiteral("Fixture system prompt"));
        QCOMPARE(sentMessages.at(1).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("user"));
        const QJsonArray userContent = sentMessages.at(1).toObject().value(QStringLiteral("content")).toArray();
        QCOMPARE(userContent.size(), 2);
        QCOMPARE(userContent.at(0).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("text"));
        QCOMPARE(userContent.at(0).toObject().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Inspect this image"));
        const QString imageUrl = userContent.at(1)
                                     .toObject()
                                     .value(QStringLiteral("image_url"))
                                     .toObject()
                                     .value(QStringLiteral("url"))
                                     .toString();
        const QByteArray fixtureImage = QByteArray::fromHex("89504e470d0a1a0a");
        QVERIFY(imageUrl.startsWith(QStringLiteral("data:image/png;base64,")));
        QCOMPARE(QByteArray::fromBase64(imageUrl.mid(QStringLiteral("data:image/png;base64,").size()).toLatin1()),
                 fixtureImage);
        const QJsonObject previousAssistant = sentMessages.at(2).toObject();
        QCOMPARE(previousAssistant.value(QStringLiteral("role")).toString(), QStringLiteral("assistant"));
        QCOMPARE(previousAssistant.value(QStringLiteral("content")).toString(), QStringLiteral("Checking the output."));
        const QJsonArray previousCalls = previousAssistant.value(QStringLiteral("tool_calls")).toArray();
        QCOMPARE(previousCalls.size(), 1);
        QCOMPARE(previousCalls.first().toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("call_previous"));
        QCOMPARE(previousCalls.first()
                     .toObject()
                     .value(QStringLiteral("function"))
                     .toObject()
                     .value(QStringLiteral("arguments"))
                     .toString(),
                 QStringLiteral("{}"));
        QCOMPARE(sentMessages.at(2).toObject().value(QStringLiteral("reasoning_content")).toString(),
                 QStringLiteral("preserve this reasoning field"));
        QCOMPARE(sentMessages.at(3).toObject().value(QStringLiteral("tool_call_id")).toString(),
                 QStringLiteral("call_previous"));
        QCOMPARE(sentMessages.at(3).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("tool"));
        QCOMPARE(sentMessages.at(3).toObject().value(QStringLiteral("content")).toString(),
                 QStringLiteral("flow state result"));

        QVERIFY(response.success);
        QCOMPARE(response.content, QStringLiteral("Need the current run output."));
        QCOMPARE(response.reasoningContent, QStringLiteral("inspect the output first"));
        QCOMPARE(response.promptTokens, 37);
        QCOMPARE(response.completionTokens, 12);
        QCOMPARE(response.toolCalls.size(), 1);
        const QJsonObject returnedCall = response.toolCalls.first().toObject();
        QCOMPARE(returnedCall, responseToolCall);
        QCOMPARE(returnedCall.value(QStringLiteral("id")).toString(), QStringLiteral("call_fixture_1"));
        const QJsonObject returnedFunction = returnedCall.value(QStringLiteral("function")).toObject();
        QCOMPARE(returnedFunction.value(QStringLiteral("name")).toString(), QStringLiteral("get_module_output"));
        QVERIFY(returnedFunction.value(QStringLiteral("arguments")).isString());
        const QJsonObject returnedArguments =
            QJsonDocument::fromJson(returnedFunction.value(QStringLiteral("arguments")).toString().toUtf8()).object();
        QCOMPARE(returnedArguments, responseArguments);
        QCOMPARE(returnedArguments.value(QStringLiteral("runId")).toString(), QStringLiteral("run-17"));
        QCOMPARE(returnedArguments.value(QStringLiteral("instanceId")).toString(), QStringLiteral("measure-1"));
        QCOMPARE(returnedArguments.value(QStringLiteral("preview")).toBool(), true);
    }
};

QTEST_GUILESS_MAIN(TestOpenAILLMClient)
#include "test_openai_llm_client.moc"
