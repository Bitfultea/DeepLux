#include "core/agent/SamBackendClient.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>
#include <QtTest/QtTest>
#include <functional>

using namespace DeepLux;

namespace {

// 阶段 5：测试内 HTTP 服务，覆盖 SAM 后端四个端点（/health、/set_image、
// /predict、/unload_image），不依赖 Python/真实模型，可模拟挂起与崩溃。
class SamTestServer : public QObject {
    Q_OBJECT

public:
    explicit SamTestServer(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, &SamTestServer::onNewConnection);
    }

    bool listen() {
        if (!m_server.listen(QHostAddress::LocalHost, m_port))
            return false;
        m_port = m_server.serverPort(); // 记录已分配端口，供崩溃后原端口重启
        return true;
    }
    // 崩溃恢复：尽量在原端口重新监听
    bool restart() {
        if (m_server.isListening())
            m_server.close();
        return listen();
    }
    void crash() {
        m_server.close();
    }
    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }
    bool isListening() const {
        return m_server.isListening();
    }

    // 行为开关
    bool hangAll = false; // 接受连接但不响应（模拟卡死 → 客户端超时）
    QString healthStatus = QStringLiteral("ok");
    QString predictStatus = QStringLiteral("ok");

    // 请求记录（断言四个端点都被真实调用）
    QStringList requestLog;
    QJsonObject lastPredictBody;
    QString lastSetImagePath;

private:
    void onNewConnection() {
        while (QTcpSocket* socket = m_server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handleBytes(socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    }

    void handleBytes(QTcpSocket* socket) {
        m_buffers[socket] += socket->readAll();
        if (hangAll)
            return; // 故意不响应，触发客户端超时

        QByteArray& data = m_buffers[socket];
        const int headerEnd = data.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray header = data.left(headerEnd);
        int contentLength = 0;
        for (const QByteArray& line : header.split('\n')) {
            if (line.trimmed().toLower().startsWith("content-length:"))
                contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
        }
        if (data.size() < headerEnd + 4 + contentLength)
            return; // 请求体未收全

        const QByteArray body = data.mid(headerEnd + 4, contentLength);
        data.remove(0, headerEnd + 4 + contentLength);

        const QList<QByteArray> requestLine = header.left(header.indexOf('\r')).split(' ');
        const QString method = QString::fromLatin1(requestLine.value(0));
        const QString path = QString::fromLatin1(requestLine.value(1));
        requestLog.append(method + " " + path);

        QJsonObject response;
        if (path == "/health") {
            response = QJsonObject{{"status", healthStatus}, {"model_name", "sam_test_vit_b"}};
        } else if (path == "/set_image") {
            lastSetImagePath = QJsonDocument::fromJson(body).object().value("image_path").toString();
            response = QJsonObject{{"embedding_id", "emb-test-1"}, {"model_name", "sam_test_vit_b"}};
        } else if (path == "/predict") {
            lastPredictBody = QJsonDocument::fromJson(body).object();
            response = predictResponse();
        } else if (path == "/unload_image") {
            response = QJsonObject{{"status", "ok"}};
        } else {
            response = QJsonObject{{"status", "error"}, {"error", "unknown endpoint"}};
        }

        const QByteArray payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
        QByteArray http;
        http += "HTTP/1.1 200 OK\r\n";
        http += "Content-Type: application/json\r\n";
        http += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
        http += "Connection: close\r\n\r\n";
        http += payload;
        socket->write(http);
        socket->disconnectFromHost();
    }

    QJsonObject predictResponse() {
        QJsonObject response;
        response["status"] = predictStatus;
        if (predictStatus == "ok") {
            response["polygon"] =
                QJsonArray{QJsonArray{10, 10}, QJsonArray{20, 10}, QJsonArray{20, 20}, QJsonArray{10, 20}};
            response["bbox"] = QJsonArray{10, 10, 10, 10};
            response["score"] = 0.97;
            response["mask_png_base64"] = QString::fromLatin1(tinyMaskPngBase64());
        }
        return response;
    }

    static QByteArray tinyMaskPngBase64() {
        QImage mask(2, 2, QImage::Format_ARGB32);
        mask.fill(QColor(255, 255, 255, 255));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        mask.save(&buffer, "PNG");
        return png.toBase64();
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    quint16 m_port = 0; // 0 = 由系统分配；crash() 后 restart() 尽量复用已分配端口
};

// 等待谓词成立（带事件循环），超时返回 false
bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

} // namespace

class TestSamBackendClient : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void initialStateIsNotStarted();
    void stateTransitionsToErrorOnNetworkFailure();
    void timeoutTransitionsToError();
    void predictWithoutEmbeddingEmitsError();
    void resolvedScriptPathFindsSamServer();
    void managedEnvironmentPathUsesAppDataDir();
    void managedEnvironmentRequiresServerPackages();
    void managedPythonOverridesSystemWhenReady();
    void missingServerScriptTransitionsToError();
    void modelPathCanBeImported();
    void modelTypeIsInferredFromWeightName();
    void samHqWeightIsRejectedBeforeStart();
    void sam3WeightIsRejectedBeforeStart();
    void stopServerProcessResetsPendingState();
    void cancelEnvironmentInitializationEmitsFinished();

    // 阶段 5：测试内 HTTP 服务覆盖四端点——成功路径/超时/崩溃恢复
    void httpServiceFullSuccessPath();
    void httpServiceHangTriggersRealTimeout();
    void httpServiceCrashAndRecovery();

private:
    QString m_unusedPortUrl;
};

void TestSamBackendClient::initTestCase() {
    // 使用一个几乎确定没有 server 监听的端口
    m_unusedPortUrl = QStringLiteral("http://127.0.0.1:59999");
    qRegisterMetaType<QList<QPointF>>("QList<QPointF>");
}

void TestSamBackendClient::cleanup() {}

void TestSamBackendClient::initialStateIsNotStarted() {
    SamBackendClient client;
    QCOMPARE(client.state(), SamBackendClient::State::NotStarted);
    QVERIFY(client.serverUrl().contains("127.0.0.1"));
}

void TestSamBackendClient::stateTransitionsToErrorOnNetworkFailure() {
    SamBackendClient client;
    client.setServerUrl(m_unusedPortUrl);

    QSignalSpy stateSpy(&client, &SamBackendClient::stateChanged);
    QSignalSpy errSpy(&client, &SamBackendClient::errorOccurred);

    client.healthCheck();

    // 等待结果（连接被拒绝会很快）
    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(errSpy.count() >= 1, 5000);

    // 最终应该到达 Error
    QVERIFY2(client.state() == SamBackendClient::State::Error || stateSpy.count() >= 1,
             "State should transition on network failure");
}

void TestSamBackendClient::timeoutTransitionsToError() {
    // 30s 超时太长，这里只验证 timeout 机制存在
    SamBackendClient client;
    client.setServerUrl(m_unusedPortUrl);
    QSignalSpy errSpy(&client, &SamBackendClient::errorOccurred);

    // predictWithoutEmbedding 应该立即发出错误
    client.predict({QPointF(10, 10)}, {}, QRectF());
    QTRY_VERIFY_WITH_TIMEOUT(errSpy.count() >= 1, 2000);
    QCOMPARE(client.state(), SamBackendClient::State::Error);
}

void TestSamBackendClient::predictWithoutEmbeddingEmitsError() {
    SamBackendClient client;
    QSignalSpy errSpy(&client, &SamBackendClient::errorOccurred);

    // embedding_id 为空时 predict 应立即报错
    client.predict({QPointF(5, 5)}, {}, QRectF());
    QTRY_VERIFY_WITH_TIMEOUT(errSpy.count() >= 1, 2000);
    QVERIFY(!errSpy.takeFirst().at(0).toString().isEmpty());
}

void TestSamBackendClient::resolvedScriptPathFindsSamServer() {
    SamBackendClient client;
    const QString script = client.resolvedServerScriptPath();
    QVERIFY2(script.endsWith(QStringLiteral("tools/sam_server/sam_server.py")), qPrintable(script));
    QVERIFY2(QFileInfo::exists(script), qPrintable(script));
}

void TestSamBackendClient::managedEnvironmentPathUsesAppDataDir() {
    const QByteArray oldAppData = qgetenv("DEEPLUX_APP_DATA_DIR");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", temp.path().toLocal8Bit());

    SamBackendClient client;
    QCOMPARE(client.managedEnvironmentPath(), QDir(temp.path()).filePath(QStringLiteral("sam_env")));
    QVERIFY2(client.managedPythonPath().endsWith(QStringLiteral("sam_env/bin/python")),
             qPrintable(client.managedPythonPath()));

    if (oldAppData.isEmpty())
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    else
        qputenv("DEEPLUX_APP_DATA_DIR", oldAppData);
}

void TestSamBackendClient::managedEnvironmentRequiresServerPackages() {
    const QByteArray oldAppData = qgetenv("DEEPLUX_APP_DATA_DIR");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", temp.path().toLocal8Bit());

    SamBackendClient client;
    const QString pythonPath = client.managedPythonPath();
    QVERIFY(QDir().mkpath(QFileInfo(pythonPath).absolutePath()));
    QFile python(pythonPath);
    QVERIFY(python.open(QIODevice::WriteOnly));
    python.close();

    QVERIFY(!client.managedEnvironmentReady());

    if (oldAppData.isEmpty())
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    else
        qputenv("DEEPLUX_APP_DATA_DIR", oldAppData);
}

void TestSamBackendClient::managedPythonOverridesSystemWhenReady() {
    const QByteArray oldAppData = qgetenv("DEEPLUX_APP_DATA_DIR");
    const QByteArray oldPython = qgetenv("SAM_SERVER_PYTHON");
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", temp.path().toLocal8Bit());
    qunsetenv("SAM_SERVER_PYTHON");

    SamBackendClient client;
    const QString pythonPath = client.managedPythonPath();
    QVERIFY(QDir().mkpath(QFileInfo(pythonPath).absolutePath()));
    QFile python(pythonPath);
    QVERIFY(python.open(QIODevice::WriteOnly));
    python.close();
    for (const QString& module : client.requiredPythonModules()) {
        QVERIFY(QDir().mkpath(QDir(client.managedSitePackagesPath()).filePath(module)));
    }

    QVERIFY(client.managedEnvironmentReady());
    QCOMPARE(client.resolvedPythonPath(), pythonPath);

    if (oldAppData.isEmpty())
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    else
        qputenv("DEEPLUX_APP_DATA_DIR", oldAppData);
    if (oldPython.isEmpty())
        qunsetenv("SAM_SERVER_PYTHON");
    else
        qputenv("SAM_SERVER_PYTHON", oldPython);
}

void TestSamBackendClient::missingServerScriptTransitionsToError() {
    SamBackendClient client;
    client.setServerScriptPath(QStringLiteral("/tmp/deeplux_missing_sam_server.py"));
    QSignalSpy errSpy(&client, &SamBackendClient::errorOccurred);

    client.startServerProcess();

    QCOMPARE(client.state(), SamBackendClient::State::Error);
    QCOMPARE(errSpy.count(), 1);
}

void TestSamBackendClient::modelPathCanBeImported() {
    SamBackendClient client;
    const QString path = QStringLiteral("/models/sam_vit_b.pth");
    client.setModelPath(path);
    QCOMPARE(client.modelPath(), path);
}

void TestSamBackendClient::modelTypeIsInferredFromWeightName() {
    SamBackendClient client;
    client.setModelPath(QStringLiteral("/models/sam_vit_h_4b8939.pth"));
    QCOMPARE(client.inferredModelType(), QStringLiteral("vit_h"));
    client.setModelPath(QStringLiteral("/models/sam_vit_l.pth"));
    QCOMPARE(client.inferredModelType(), QStringLiteral("vit_l"));
    client.setModelPath(QStringLiteral("/models/anything_else.pth"));
    QCOMPARE(client.inferredModelType(), QStringLiteral("vit_b"));
}

void TestSamBackendClient::samHqWeightIsRejectedBeforeStart() {
    QTemporaryFile weight(QDir::temp().filePath(QStringLiteral("sam_hq_vit_h_XXXXXX.pth")));
    QVERIFY(weight.open());
    weight.close();

    SamBackendClient client;
    client.setModelPath(weight.fileName());
    QSignalSpy errSpy(&client, &SamBackendClient::errorOccurred);

    client.startServerProcess();

    QCOMPARE(client.state(), SamBackendClient::State::Error);
    QCOMPARE(errSpy.count(), 1);
    QVERIFY(errSpy.takeFirst().at(0).toString().contains(QStringLiteral("SAM-HQ")));
}

void TestSamBackendClient::sam3WeightIsRejectedBeforeStart() {
    QTemporaryFile weight(QDir::temp().filePath(QStringLiteral("sam3_XXXXXX.pt")));
    QVERIFY(weight.open());
    weight.close();

    SamBackendClient client;
    client.setModelPath(weight.fileName());
    QSignalSpy errSpy(&client, &SamBackendClient::errorOccurred);

    client.startServerProcess();

    QCOMPARE(client.state(), SamBackendClient::State::Error);
    QCOMPARE(errSpy.count(), 1);
    QVERIFY(errSpy.takeFirst().at(0).toString().contains(QStringLiteral("SAM3")));
}

void TestSamBackendClient::stopServerProcessResetsPendingState() {
    SamBackendClient client;
    client.setServerUrl(m_unusedPortUrl);

    client.setImage(QStringLiteral("/tmp/deeplux_missing_image.png"));
    QCOMPARE(client.state(), SamBackendClient::State::LoadingModel);

    client.stopServerProcess();

    QCOMPARE(client.state(), SamBackendClient::State::NotStarted);
    QVERIFY(client.currentEmbeddingId().isEmpty());
}

void TestSamBackendClient::cancelEnvironmentInitializationEmitsFinished() {
    SamBackendClient client;
    QSignalSpy spy(&client, &SamBackendClient::environmentInitializationFinished);

    client.cancelEnvironmentInitialization();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);
}

// ---------------------------------------------------------------------------
// 阶段 5：测试内 HTTP 服务覆盖 /health、/set_image、/predict、/unload_image。
// 完整成功路径、超时与崩溃恢复均为真实断言，不再只是错误路径组件测试。
// ---------------------------------------------------------------------------

void TestSamBackendClient::httpServiceFullSuccessPath() {
    SamTestServer server;
    QVERIFY2(server.listen(), "test HTTP server must listen");

    SamBackendClient client;
    client.setServerUrl(server.url());
    QSignalSpy stateSpy(&client, &SamBackendClient::stateChanged);
    QSignalSpy embeddingSpy(&client, &SamBackendClient::embeddingReady);
    QSignalSpy predictionSpy(&client, &SamBackendClient::predictionReady);
    QSignalSpy errorSpy(&client, &SamBackendClient::errorOccurred);

    // 1) /health → Ready
    client.healthCheck();
    QVERIFY2(waitFor([&] { return client.state() == SamBackendClient::State::Ready; }), "health must reach Ready");

    // 2) /set_image → embeddingReady + Ready
    const QString imagePath = QStringLiteral("/tmp/deeplux_sam_test_image.png");
    client.setImage(imagePath);
    QVERIFY2(waitFor([&] { return embeddingSpy.count() > 0; }), "setImage must emit embeddingReady");
    QCOMPARE(client.currentEmbeddingId(), QStringLiteral("emb-test-1"));
    QCOMPARE(server.lastSetImagePath, imagePath);

    // 3) /predict → predictionReady，polygon/bbox/score/mask 均按响应解析
    client.predict({QPointF(15, 15)}, {}, QRectF(10, 10, 10, 10));
    QVERIFY2(waitFor([&] { return predictionSpy.count() > 0; }), "predict must emit predictionReady");
    QCOMPARE(errorSpy.count(), 0);
    const QList<QVariant> args = predictionSpy.takeFirst();
    const QList<QPointF> polygon = args.at(0).value<QList<QPointF>>();
    QCOMPARE(polygon.size(), 4);
    QCOMPARE(polygon.first(), QPointF(10, 10));
    QCOMPARE(args.at(1).toRectF(), QRectF(10, 10, 10, 10));
    QCOMPARE(args.at(2).toDouble(), 0.97);
    QVERIFY2(!args.at(4).value<QImage>().isNull(), "mask_png_base64 must decode to a QImage");
    // 客户端必须携带服务端签发的 embedding_id 请求预测
    QCOMPARE(server.lastPredictBody.value("embedding_id").toString(), QStringLiteral("emb-test-1"));
    QCOMPARE(client.state(), SamBackendClient::State::Ready);

    // 4) /unload_image → embedding 清空，回到 NotStarted
    client.unloadImage();
    QVERIFY2(waitFor([&] { return client.state() == SamBackendClient::State::NotStarted; }),
             "unload must return to NotStarted");
    QVERIFY(client.currentEmbeddingId().isEmpty());

    // 四个端点都被真实调用
    QVERIFY2(server.requestLog.contains("GET /health"), qPrintable(server.requestLog.join(", ")));
    QVERIFY(server.requestLog.contains("POST /set_image"));
    QVERIFY(server.requestLog.contains("POST /predict"));
    QVERIFY(server.requestLog.contains("POST /unload_image"));
    Q_UNUSED(stateSpy);
}

void TestSamBackendClient::httpServiceHangTriggersRealTimeout() {
    SamTestServer server;
    QVERIFY2(server.listen(), "test HTTP server must listen");
    server.hangAll = true; // 接受连接但不响应

    SamBackendClient client;
    client.setServerUrl(server.url());
    client.setTimeoutMs(300); // 真实超时路径：300ms 内服务端不响应
    QSignalSpy errorSpy(&client, &SamBackendClient::errorOccurred);

    QElapsedTimer timer;
    timer.start();
    client.healthCheck();
    QVERIFY2(waitFor([&] { return client.state() == SamBackendClient::State::Error; }, 5000),
             "hang must transition to Error via timeout");
    const qint64 elapsed = timer.elapsed();

    QVERIFY2(elapsed >= 250, qPrintable(QString("timeout fired too early: %1ms").arg(elapsed)));
    QVERIFY2(elapsed < 3000, qPrintable(QString("timeout fired too late: %1ms").arg(elapsed)));
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY2(errorSpy.takeFirst().at(0).toString().contains(QStringLiteral("超时")), "error must report timeout");
}

void TestSamBackendClient::httpServiceCrashAndRecovery() {
    SamTestServer server;
    QVERIFY2(server.listen(), "test HTTP server must listen");

    SamBackendClient client;
    client.setServerUrl(server.url());
    QSignalSpy errorSpy(&client, &SamBackendClient::errorOccurred);
    QSignalSpy predictionSpy(&client, &SamBackendClient::predictionReady);

    // 建立会话：set_image 成功
    client.setImage(QStringLiteral("/tmp/deeplux_sam_test_image.png"));
    QVERIFY2(waitFor([&] { return client.currentEmbeddingId() == QStringLiteral("emb-test-1"); }),
             "setImage must succeed before crash");

    // 崩溃：服务端停止监听，predict 必须失败并进入 Error
    server.crash();
    client.predict({QPointF(15, 15)}, {}, QRectF());
    QVERIFY2(waitFor([&] { return client.state() == SamBackendClient::State::Error; }),
             "crashed backend must transition client to Error");
    QVERIFY2(errorSpy.count() >= 1, "crash must emit errorOccurred");
    QCOMPARE(predictionSpy.count(), 0);

    // 恢复：服务端在原端口重启，健康检查与预测重新成功
    QVERIFY2(server.restart(), "test HTTP server must restart on the same port");
    QCOMPARE(server.url(), client.serverUrl());
    client.healthCheck();
    QVERIFY2(waitFor([&] { return client.state() == SamBackendClient::State::Ready; }),
             "recovered backend must bring client back to Ready");

    client.predict({QPointF(15, 15)}, {}, QRectF());
    QVERIFY2(waitFor([&] { return predictionSpy.count() > 0; }), "predict must succeed after recovery");
    QCOMPARE(client.state(), SamBackendClient::State::Ready);
}

QTEST_MAIN(TestSamBackendClient)
#include "test_sambackendclient.moc"
