// 测试需要访问 setLogDirForTest/setNowForTest 等测试钩子（DEEPLUX_TEST_VISIBILITY）。
// 用 DEEPLUX_BUILD_TESTS 让宏展开为 public（不能用 `#define private public`，
// MSVC STL 的 xkeycheck.h 禁止宏化关键字）。
#define DEEPLUX_BUILD_TESTS 1
#include "core/agent/AgentAuditLog.h"
#include "core/agent/AgentController.h"
#include "core/agent/FailedRequestLog.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/agent/OpenAILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/manager/ConfigManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
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

QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QList<QJsonObject> readAuditRecords(const QString& path) {
    QList<QJsonObject> records;
    for (const QByteArray& line : readAll(path).split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            records.append(doc.object());
    }
    return records;
}

QJsonObject readFailedFile(const QString& path) {
    const QJsonDocument doc = QJsonDocument::fromJson(readAll(path));
    return doc.isObject() ? doc.object() : QJsonObject();
}

AgentActionLogEntry makeEntry(const QString& params = QStringLiteral("{\"mode\":\"once\",\"wait\":true}")) {
    AgentActionLogEntry entry;
    entry.timestamp = QDateTime(QDate(2026, 10, 27), QTime(22, 41, 12, 345));
    entry.actor = QStringLiteral("Agent");
    entry.action = QStringLiteral("run_flow");
    entry.params = params;
    entry.result = QStringLiteral("success");
    entry.undoable = false;
    entry.durationMs = 132;
    entry.errorCategory = QStringLiteral("Malformed");
    entry.toolCallId = QStringLiteral("call_1");
    return entry;
}

/// 最小 HTTP 服务：对每个请求返回固定状态/Content-Type/正文（用于驱动真实客户端失败路径）
class HttpStub : public QObject {
public:
    HttpStub(const QByteArray& statusLine, const QByteArray& contentType, const QByteArray& body)
        : m_statusLine(statusLine), m_contentType(contentType), m_body(body) {
        connect(&m_server, &QTcpServer::newConnection, this, &HttpStub::onNewConnection);
    }

    bool listen() {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString endpoint() const {
        return QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(m_server.serverPort());
    }
    int requests = 0;

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
                ++requests;
                const QByteArray response = m_statusLine + "\r\nContent-Type: " + m_contentType +
                                            "\r\nContent-Length: " + QByteArray::number(m_body.size()) +
                                            "\r\nConnection: close\r\n\r\n" + m_body;
                socket->write(response);
                socket->flush();
                socket->disconnectFromHost();
                buffer->clear();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    }

    QTcpServer m_server;
    QByteArray m_statusLine;
    QByteArray m_contentType;
    QByteArray m_body;
};

AgentConversation simpleConversation() {
    AgentConversation ctx;
    AgentMessage user;
    user.role = QStringLiteral("user");
    user.content = QStringLiteral("please describe this image");
    ctx.messages.append(user);
    return ctx;
}

} // namespace

/**
 * 第二迭代下半 t18：审计 JSONL 落盘（§3）+ LLM 失败请求报文落盘（§4）
 * 全部使用临时目录 + 固定时钟，不读写真实 ~/.deeplux/logs。
 */
class TestAgentAudit : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        AgentAuditLog::instance().resetForTest();
        FailedRequestLog::instance().resetForTest();
    }

    void cleanupTestCase() {
        AgentAuditLog::instance().shutdown();
    }

    void init() {
        AgentAuditLog::instance().resetForTest();
        FailedRequestLog::instance().resetForTest();
        // B5：既有用例验证"记录能力"，显式开启；默认关闭由独立用例覆盖
        FailedRequestLog::instance().setEnabledForTest(true);
    }

    // ---------------- 审计 JSONL（设计 §3） ----------------

    void testAuditWritesAllFields() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString dir = temp.path();
        AgentAuditLog& log = AgentAuditLog::instance();
        log.setLogDirForTest(dir);
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        log.append(makeEntry());
        log.flush();

        const QString path = QDir(dir).filePath(QStringLiteral("agent-audit-20261027.jsonl"));
        QVERIFY2(QFileInfo::exists(path), qPrintable(log.currentFilePath()));
        const QList<QJsonObject> records = readAuditRecords(path);
        QCOMPARE(records.size(), 1);
        const QJsonObject record = records.first();
        QCOMPARE(record.value(QStringLiteral("v")).toInt(), 1);
        QVERIFY(record.value(QStringLiteral("ts")).toString().startsWith(QStringLiteral("2026-10-27T22:41:12.345")));
        QCOMPARE(record.value(QStringLiteral("actor")).toString(), QStringLiteral("Agent"));
        QCOMPARE(record.value(QStringLiteral("action")).toString(), QStringLiteral("run_flow"));
        QCOMPARE(record.value(QStringLiteral("params")).toObject().value(QStringLiteral("mode")).toString(),
                 QStringLiteral("once"));
        QCOMPARE(record.value(QStringLiteral("result")).toString(), QStringLiteral("success"));
        QCOMPARE(record.value(QStringLiteral("undoable")).toBool(), false);
        QCOMPARE(record.value(QStringLiteral("durationMs")).toInt(), 132);
        QCOMPARE(record.value(QStringLiteral("errorCategory")).toString(), QStringLiteral("Malformed"));
        QCOMPARE(record.value(QStringLiteral("toolCallId")).toString(), QStringLiteral("call_1"));
        // -1 / 空字段省略
        AgentActionLogEntry bare = makeEntry(QStringLiteral("not json"));
        bare.durationMs = -1;
        bare.errorCategory.clear();
        bare.toolCallId.clear();
        log.append(bare);
        log.flush();
        const QList<QJsonObject> all = readAuditRecords(path);
        QCOMPARE(all.size(), 2);
        QVERIFY(!all.last().contains(QStringLiteral("durationMs")));
        QVERIFY(!all.last().contains(QStringLiteral("errorCategory")));
        QVERIFY(!all.last().contains(QStringLiteral("toolCallId")));
        QCOMPARE(all.last().value(QStringLiteral("params")).toObject().value(QStringLiteral("raw")).toString(),
                 QStringLiteral("not json"));
    }

    void testAuditDayRotation() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString dir = temp.path();
        AgentAuditLog& log = AgentAuditLog::instance();
        log.setLogDirForTest(dir);
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(23, 59, 0)));
        log.append(makeEntry());
        log.setNowForTest(QDateTime(QDate(2026, 10, 28), QTime(0, 1, 0)));
        log.append(makeEntry());
        log.flush();

        const QString day1 = QDir(dir).filePath(QStringLiteral("agent-audit-20261027.jsonl"));
        const QString day2 = QDir(dir).filePath(QStringLiteral("agent-audit-20261028.jsonl"));
        QVERIFY(QFileInfo::exists(day1));
        QVERIFY(QFileInfo::exists(day2));
        QCOMPARE(readAuditRecords(day1).size(), 1);
        QCOMPARE(readAuditRecords(day2).size(), 1);
        QVERIFY(log.currentFilePath().contains(QStringLiteral("20261028")));
    }

    void testAuditPartRotation() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString dir = temp.path();
        AgentAuditLog& log = AgentAuditLog::instance();
        log.setLogDirForTest(dir);
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        log.setMaxFileBytesForTest(1024);

        const QString blob = QString(600, QLatin1Char('p'));
        for (int i = 0; i < 4; ++i)
            log.append(makeEntry(QStringLiteral("{\"blob\":\"%1\",\"i\":%2}").arg(blob).arg(i)));
        log.flush();

        const QStringList parts = QDir(dir).entryList({QStringLiteral("agent-audit-20261027*.jsonl")}, QDir::Files);
        QVERIFY2(parts.size() >= 2, qPrintable(parts.join(QStringLiteral(", "))));
        int total = 0;
        for (const QString& name : parts) {
            const QFileInfo info(QDir(dir).filePath(name));
            QVERIFY2(info.size() <= 1024, qPrintable(QStringLiteral("%1 size=%2").arg(name).arg(info.size())));
            total += readAuditRecords(info.absoluteFilePath()).size();
        }
        QCOMPARE(total, 4);
    }

    void testAuditRetention() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString dir = temp.path();
        const QString oldPath = QDir(dir).filePath(QStringLiteral("agent-audit-20261001.jsonl"));
        QFile oldFile(oldPath);
        QVERIFY(oldFile.open(QIODevice::WriteOnly));
        oldFile.write("{\"v\":1}\n");
        oldFile.close();
        const QString recentPath = QDir(dir).filePath(QStringLiteral("agent-audit-20261020.jsonl"));
        QFile recentFile(recentPath);
        QVERIFY(recentFile.open(QIODevice::WriteOnly));
        recentFile.write("{\"v\":1}\n");
        recentFile.close();

        AgentAuditLog& log = AgentAuditLog::instance();
        log.setLogDirForTest(dir);
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        log.append(makeEntry());
        log.flush();

        QVERIFY2(!QFileInfo::exists(oldPath), "15 天前的审计文件必须被清理");
        QVERIFY2(QFileInfo::exists(recentPath), "保留期内的审计文件不得删除");
    }

    void testAuditWriteFailureDisablesAfterThree() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString notADirectory = QDir(temp.path()).filePath(QStringLiteral("blocked"));
        QFile blocker(notADirectory);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("x");
        blocker.close();

        AgentAuditLog& log = AgentAuditLog::instance();
        log.setLogDirForTest(notADirectory);
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        log.append(makeEntry());
        log.append(makeEntry());
        log.append(makeEntry());
        const int droppedAfterThree = log.droppedRecords();
        QCOMPARE(droppedAfterThree, 3);
        log.append(makeEntry()); // 已自禁用：不再尝试、不增加计数
        QCOMPARE(log.droppedRecords(), droppedAfterThree);

        // 跨天后自动恢复（设计 §3.5：自禁用只持续到下一个自然日）
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 28), QTime(12, 0, 0)));
        log.append(makeEntry());
        log.flush();
        QVERIFY2(QFileInfo::exists(log.currentFilePath()), "跨天后应恢复写盘");
        QCOMPARE(log.droppedRecords(), droppedAfterThree);
    }

    void testAuditRedactsSensitiveParams() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString dir = temp.path();
        AgentAuditLog& log = AgentAuditLog::instance();
        log.setLogDirForTest(dir);
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        log.append(makeEntry(QStringLiteral("{\"apiKey\":\"sk-secret-123\",\"Authorization\":\"Bearer abc.def\","
                                            "\"image\":\"data:image/png;base64,QUJDREVGR0g=\"}")));
        log.flush();
        const QByteArray text = readAll(log.currentFilePath());
        QVERIFY2(!text.contains("sk-secret-123"), text.constData());
        QVERIFY2(!text.contains("Bearer abc.def"), text.constData());
        QVERIFY2(!text.contains("QUJDREVGR0g="), text.constData());
        const QJsonObject record = readAuditRecords(log.currentFilePath()).first();
        QCOMPARE(record.value(QStringLiteral("params")).toObject().value(QStringLiteral("apiKey")).toString(),
                 QStringLiteral("***"));
    }

    void testAuditRedactsCredentialVariants() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        AgentAuditLog& log = AgentAuditLog::instance();
        log.setEnabledForTest(true);
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        log.append(makeEntry(QStringLiteral("{\"nested\":{\"access_token\":\"audit-access-123\","
                                            "\"clientSecret\":\"audit-client-456\"},"
                                            "\"text\":\"password='audit phrase secret'\"}")));
        log.flush();
        const QByteArray text = readAll(log.currentFilePath());
        QVERIFY(!text.isEmpty());
        QVERIFY(!text.contains("audit-access-123"));
        QVERIFY(!text.contains("audit-client-456"));
        QVERIFY(!text.contains("audit phrase secret"));
    }

    void testAuditRecordSizeCap() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString dir = temp.path();
        AgentAuditLog& log = AgentAuditLog::instance();
        log.setLogDirForTest(dir);
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        const QJsonObject hugeParams{{QStringLiteral("blob"), QString(30000, QLatin1Char('x'))}};
        log.append(makeEntry(QString::fromUtf8(QJsonDocument(hugeParams).toJson(QJsonDocument::Compact))));
        log.flush();
        const QByteArray line = readAll(log.currentFilePath()).trimmed();
        QVERIFY2(line.size() <= 16 * 1024, qPrintable(QStringLiteral("line=%1").arg(line.size())));
        const QJsonObject record = QJsonDocument::fromJson(line).object();
        QCOMPARE(record.value(QStringLiteral("params")).toObject().value(QStringLiteral("raw")).toString(),
                 QStringLiteral("…[truncated]"));
    }

    void testAuditControllerSignalWrites() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString dir = temp.path();
        AgentAuditLog& log = AgentAuditLog::instance();
        log.setLogDirForTest(dir);
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        QVERIFY(log.initialize(&AgentController::instance()));
        AgentController::instance().logAction(makeEntry());
        log.flush();
        QCOMPARE(readAuditRecords(log.currentFilePath()).size(), 1);
        log.shutdown();
    }

    // ---------------- LLM 失败请求报文（设计 §4） ----------------

    void testBadRequestViaClient() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        FailedRequestLog& log = FailedRequestLog::instance();
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));

        HttpStub stub("HTTP/1.1 400 Bad Request", "application/json",
                      "{\"error\":{\"message\":\"Invalid image_url content type\"}}");
        QVERIFY(stub.listen());
        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        client.setApiKey(QStringLiteral("sk-test-123"));
        LLMErrorInfo captured;
        connect(&client, &ILLMClient::errorOccurredClassified, this,
                [&captured](const LLMErrorInfo& info) { captured = info; });
        bool errored = false;
        connect(&client, &ILLMClient::errorOccurred, this, [&errored](const QString&) { errored = true; });
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&errored]() { return errored; }));
        QCOMPARE(captured.category, LLMErrorCategory::BadRequest);
        QVERIFY(captured.visionDowngradeHint);

        const QString path = log.currentFilePath();
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        const QJsonObject root = readFailedFile(path);
        QCOMPARE(root.value(QStringLiteral("v")).toInt(), 1);
        const QJsonArray records = root.value(QStringLiteral("records")).toArray();
        QCOMPARE(records.size(), 1);
        const QJsonObject record = records.first().toObject();
        QCOMPARE(record.value(QStringLiteral("category")).toString(), QStringLiteral("BadRequest"));
        QCOMPARE(record.value(QStringLiteral("httpStatus")).toInt(), 400);
        QVERIFY(record.value(QStringLiteral("visionDowngradeHint")).toBool());
        QVERIFY(record.value(QStringLiteral("request")).toObject().value(QStringLiteral("messageCount")).toInt() >= 1);
    }

    void testMalformedViaClient() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        FailedRequestLog& log = FailedRequestLog::instance();
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));

        HttpStub stub("HTTP/1.1 200 OK", "text/plain", "this is not json");
        QVERIFY(stub.listen());
        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        bool errored = false;
        connect(&client, &ILLMClient::errorOccurred, this, [&errored](const QString&) { errored = true; });
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&errored]() { return errored; }));
        const QJsonObject root = readFailedFile(log.currentFilePath());
        const QJsonArray records = root.value(QStringLiteral("records")).toArray();
        QVERIFY(!records.isEmpty());
        QCOMPARE(records.first().toObject().value(QStringLiteral("category")).toString(), QStringLiteral("Malformed"));
    }

    void testAuthNotWritten() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        FailedRequestLog& log = FailedRequestLog::instance();
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));

        HttpStub stub("HTTP/1.1 401 Unauthorized", "application/json",
                      "{\"error\":{\"message\":\"invalid api key sk-auth-should-not-persist\"}}");
        QVERIFY(stub.listen());
#ifndef QT_NO_DEBUG_OUTPUT
        QTest::ignoreMessage(
            QtDebugMsg,
            QRegularExpression(QStringLiteral(
                "^OpenAILLMClient: Sending request to \"http://127\\.0\\.0\\.1:[0-9]+/v1/chat/completions\" "
                "model= \"gpt-4o\" bytes= [0-9]+ tools= 0 stream= true$")));
#endif
#ifndef QT_NO_WARNING_OUTPUT
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral(
                                 "^\\[OpenAILLMClient\\] request failed \"Auth\" httpStatus= 401 attempts= 1$")));
#endif
        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint() + QStringLiteral("?api_key=sk-url-should-not-persist"));
        bool errored = false;
        connect(&client, &ILLMClient::errorOccurred, this, [&errored](const QString&) { errored = true; });
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&errored]() { return errored; }));
        QVERIFY2(!QFileInfo::exists(log.currentFilePath()), "Auth 失败不得落盘");
    }

    void testTransientCategoriesNotWritten() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        FailedRequestLog& log = FailedRequestLog::instance();
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        for (LLMErrorCategory category :
             {LLMErrorCategory::RateLimit, LLMErrorCategory::Server, LLMErrorCategory::Network,
              LLMErrorCategory::Timeout, LLMErrorCategory::None}) {
            LLMErrorInfo info;
            info.category = category;
            info.httpStatus = 0;
            info.detail = QStringLiteral("transient");
            log.record(info, QByteArray(), QStringLiteral("https://api.example.com/v1/chat/completions"),
                       QStringLiteral("gpt-4o"), {});
        }
        QVERIFY2(!QFileInfo::exists(log.currentFilePath()), "瞬时错误与中止不得落盘");
    }

    void testNoSecretsOnDisk() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        FailedRequestLog& log = FailedRequestLog::instance();
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));

        const QString longBase64(5000, QLatin1Char('A'));
        LLMErrorInfo info;
        info.category = LLMErrorCategory::BadRequest;
        info.httpStatus = 400;
        info.detail = QStringLiteral("HTTP 400: reject | Authorization: Bearer sk-test-123 apiKey=sk-secret-456 | "
                                     "image data:image/png;base64,QUJDREVGR0g=") +
                      longBase64;

        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("use Authorization: Bearer sk-test-123 and apiKey: sk-secret-456");
        AgentImageAttachment image;
        image.data = QByteArray(128 * 1024, 'B');
        image.mimeType = QStringLiteral("image/png");
        user.images.append(image);
        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        assistant.content = QStringLiteral("ok");
        assistant.toolCalls = QJsonArray{QJsonObject{{"id", "call_1"}}};

        const QByteArray requestBody =
            "{\"messages\":[{\"role\":\"user\",\"content\":\"Authorization: Bearer sk-test-123\"}],"
            "\"api_key\":\"sk-secret-456\"}";
        log.record(info, requestBody,
                   QStringLiteral("https://api.example.com/v1/chat/completions?api_key=sk-url-999&token=abc"),
                   QStringLiteral("gpt-4o"), {user, assistant});

        const QString path = log.currentFilePath();
        QVERIFY2(QFileInfo::exists(path), "含敏感串但可脱敏的报文应落盘");
        const QByteArray text = readAll(path);
        QVERIFY2(!text.contains("sk-test-123"), text.constData());
        QVERIFY2(!text.contains("sk-secret-456"), text.constData());
        QVERIFY2(!text.contains("sk-url-999"), text.constData());
        QVERIFY2(!text.contains("Bearer sk-"), text.constData());
        QVERIFY2(!text.contains("QUJDREVGR0g="), text.constData());
        QVERIFY2(!text.contains(QString(64, QLatin1Char('A')).toUtf8()), text.constData());
        const QJsonObject record = readFailedFile(path).value(QStringLiteral("records")).toArray().first().toObject();
        QVERIFY(!record.value(QStringLiteral("endpoint")).toString().contains(QLatin1Char('?')));
        const QJsonArray applied = record.value(QStringLiteral("redactionApplied")).toArray();
        QVERIFY(applied.contains(QStringLiteral("authorization")));
        QVERIFY(applied.contains(QStringLiteral("apiKey")));
        QVERIFY(applied.contains(QStringLiteral("imageBase64")));
    }

    void testFailedRequestRedactsCredentialVariants() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        FailedRequestLog& log = FailedRequestLog::instance();
        log.setEnabledForTest(true);
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        LLMErrorInfo info;
        info.category = LLMErrorCategory::BadRequest;
        info.httpStatus = 400;
        info.detail = QStringLiteral("refresh_token=failed-refresh-123 password='failed phrase secret' "
                                     "bearer failed-bearer-789");
        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("access_token=failed-access-456");
        log.record(info, QByteArray(), QStringLiteral("https://example.invalid"), QStringLiteral("model"), {user});
        const QByteArray text = readAll(log.currentFilePath());
        QVERIFY(!text.isEmpty());
        QVERIFY(!text.contains("failed-refresh-123"));
        QVERIFY(!text.contains("failed-access-456"));
        QVERIFY(!text.contains("failed phrase secret"));
        QVERIFY(!text.contains("failed-bearer-789"));
    }

    void testSizeCapsAndDailyCap() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        FailedRequestLog& log = FailedRequestLog::instance();
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        log.setRecordMaxBytesForTest(16384);
        log.setMaxFileBytesForTest(65536);

        LLMErrorInfo info;
        info.category = LLMErrorCategory::BadRequest;
        info.httpStatus = 400;
        info.detail = QString(2000, QLatin1Char('d'));
        QList<AgentMessage> messages;
        for (int i = 0; i < 12; ++i) {
            AgentMessage message;
            message.role = QStringLiteral("user");
            message.content = QString(5000, QLatin1Char('m')) + QString::number(i);
            messages.append(message);
        }
        for (int i = 0; i < 30; ++i)
            log.record(info, QByteArray(8000, 'b'), QStringLiteral("https://api.example.com/v1/chat/completions"),
                       QStringLiteral("gpt-4o"), messages);

        const QString path = log.currentFilePath();
        QVERIFY(QFileInfo::exists(path));
        const QFileInfo infoFile(path);
        QVERIFY2(infoFile.size() <= 65536, qPrintable(QStringLiteral("file=%1").arg(infoFile.size())));
        const QJsonArray records = readFailedFile(path).value(QStringLiteral("records")).toArray();
        QVERIFY2(!records.isEmpty(), "至少写入首条");
        QVERIFY2(records.size() < 30, "达到单日上限后必须停止追加");
        QVERIFY2(QJsonDocument::fromJson(readAll(path)).isObject(), "文件必须保持合法 JSON");
    }

    void testWriteFailureDoesNotAffectClient() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString blocked = QDir(temp.path()).filePath(QStringLiteral("blocked"));
        QFile blocker(blocked);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("x");
        blocker.close();

        FailedRequestLog& log = FailedRequestLog::instance();
        log.setLogDirForTest(blocked);
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));

        HttpStub stub("HTTP/1.1 400 Bad Request", "application/json", "{\"error\":{\"message\":\"bad parameter\"}}");
        QVERIFY(stub.listen());
        OpenAILLMClient client;
        client.setEndpoint(stub.endpoint());
        bool errored = false;
        QString message;
        connect(&client, &ILLMClient::errorOccurred, this, [&](const QString& text) {
            errored = true;
            message = text;
        });
        client.sendRequest(simpleConversation(), {});
        QVERIFY(waitFor([&errored]() { return errored; }));
        QVERIFY(!message.isEmpty());
        QVERIFY2(log.droppedRecords() > 0, "写失败应计入 droppedRecords");

        // 跨天 + 目录恢复后应重新写盘（自禁用不跨自然日）
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 28), QTime(12, 0, 0)));
        LLMErrorInfo info;
        info.category = LLMErrorCategory::BadRequest;
        info.httpStatus = 400;
        info.detail = QStringLiteral("bad");
        log.record(info, QByteArray(), QStringLiteral("https://api.example.com/v1/chat/completions"),
                   QStringLiteral("gpt-4o"), {});
        QVERIFY2(QFileInfo::exists(log.currentFilePath()), "跨天后失败报文日志应恢复");
    }

    void testFailedRetention() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString oldPath = QDir(temp.path()).filePath(QStringLiteral("llm-failed-20261001.json"));
        QFile oldFile(oldPath);
        QVERIFY(oldFile.open(QIODevice::WriteOnly));
        oldFile.write("{\"v\":1,\"records\":[]}");
        oldFile.close();

        FailedRequestLog& log = FailedRequestLog::instance();
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));
        LLMErrorInfo info;
        info.category = LLMErrorCategory::BadRequest;
        info.httpStatus = 400;
        info.detail = QStringLiteral("bad");
        log.record(info, QByteArray(), QStringLiteral("https://api.example.com/v1/chat/completions"),
                   QStringLiteral("gpt-4o"), {});
        QVERIFY2(!QFileInfo::exists(oldPath), "15 天前的失败报文文件必须被清理");
        QVERIFY(QFileInfo::exists(log.currentFilePath()));
    }

    // B5：失败正文默认关闭；显式 true/false 均被尊重；显式值可持久化
    void testFailedRequestPrivacyDefaultsAndExplicitConfig() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", temp.path().toLocal8Bit());
        ConfigManager& cfg = ConfigManager::instance();
        QVERIFY(cfg.initialize());

        FailedRequestLog& log = FailedRequestLog::instance();
        log.clearEnabledOverrideForTest();
        log.resetForTest();

        cfg.removeGroupKey(QStringLiteral("agent"), QStringLiteral("failedRequestLogEnabled"));
        QCOMPARE(log.enabledForTest(), false); // 未显式配置 → 默认关闭

        cfg.setGroupValue(QStringLiteral("agent"), QStringLiteral("failedRequestLogEnabled"), true);
        QCOMPARE(log.enabledForTest(), true);

        cfg.setGroupValue(QStringLiteral("agent"), QStringLiteral("failedRequestLogEnabled"), false);
        QCOMPARE(log.enabledForTest(), false);

        QVERIFY(cfg.save());
        const QJsonObject saved = QJsonDocument::fromJson(readAll(cfg.configFilePath())).object();
        QCOMPARE(
            saved.value(QStringLiteral("agent")).toObject().value(QStringLiteral("failedRequestLogEnabled")).toBool(),
            false);

        // 显式开启后持久化（"重启"读取同一键仍为 true）
        cfg.setGroupValue(QStringLiteral("agent"), QStringLiteral("failedRequestLogEnabled"), true);
        QVERIFY(cfg.save());
        const QJsonObject reopened = QJsonDocument::fromJson(readAll(cfg.configFilePath())).object();
        QCOMPARE(reopened.value(QStringLiteral("agent"))
                     .toObject()
                     .value(QStringLiteral("failedRequestLogEnabled"))
                     .toBool(),
                 true);
    }

    // B5：关闭后模拟 BadRequest，磁盘不得新增正文
    void testFailedRequestDisabledWritesNoBody() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        FailedRequestLog& log = FailedRequestLog::instance();
        log.setEnabledForTest(false);
        log.setLogDirForTest(temp.path());
        log.setNowForTest(QDateTime(QDate(2026, 10, 27), QTime(12, 0, 0)));

        AgentMessage message;
        message.role = QStringLiteral("user");
        message.content = QStringLiteral("sensitive customer payload");
        LLMErrorInfo info;
        info.category = LLMErrorCategory::BadRequest;
        info.httpStatus = 400;
        info.detail = QStringLiteral("bad request detail");
        log.record(info, QByteArrayLiteral("{}"), QStringLiteral("https://api.example.com/v1/chat/completions"),
                   QStringLiteral("gpt-4o"), {message});

        const QStringList files = QDir(temp.path()).entryList(QStringList{QStringLiteral("llm-failed-*")}, QDir::Files);
        QVERIFY2(files.isEmpty(), qPrintable(files.join(QStringLiteral(","))));
        QVERIFY(readAll(log.currentFilePath()).isEmpty());
    }
};

QTEST_MAIN(TestAgentAudit)
#include "test_agentaudit.moc"
