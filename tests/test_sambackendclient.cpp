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

// 阶段 5 产出已抽取为共享 helper（SAM 批1）：tests/helpers/SamTestServer.h
#include "helpers/SamTestServer.h"

using DeepLuxTest::SamTestServer;
using DeepLuxTest::waitFor;

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
    void httpServiceConcurrentUnloadSetImageKeepsTimeout();
    void httpServiceConsecutiveSetImageLatestWins();
    void oversizedDeclaredResponseIsAbortedBeforeBuffering();
    // 批3 复核二轮（P1-3）：predict 响应严格解析失败关闭
    void predictResponseStrictParsing();

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
    QCOMPARE(client.currentEmbeddingId(), QStringLiteral("emb-1"));
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
    QCOMPARE(server.lastPredictBody.value("embedding_id").toString(), QStringLiteral("emb-1"));
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
    QVERIFY2(waitFor([&] { return client.currentEmbeddingId() == QStringLiteral("emb-1"); }),
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

void TestSamBackendClient::httpServiceConcurrentUnloadSetImageKeepsTimeout() {
    // 阶段 5 复核回归：图像切换路径会连续调用 unloadImage() 与 setImage()
    // （见 SamAnnotatorDialog 图像变化处理）。两个请求共用超时计时器时，
    // 先到的响应会取消超时保护，使挂起的另一个请求永久卡死。
    // 现在要求：任一请求完成后，只要仍有挂起请求，超时保护必须保持有效。
    SamTestServer server;
    QVERIFY2(server.listen(), "test HTTP server must listen");

    SamBackendClient client;
    client.setServerUrl(server.url());
    client.setTimeoutMs(300);

    // 先建立会话（正常响应）
    client.setImage(QStringLiteral("/tmp/deeplux_sam_test_image.png"));
    QVERIFY2(waitFor([&] { return client.currentEmbeddingId() == QStringLiteral("emb-1"); }),
             "setImage must succeed before concurrency scenario");

    // 模拟图像切换：/unload_image 正常响应，紧随其后的 /set_image 挂起
    server.hangPaths.insert(QStringLiteral("/set_image"));
    client.unloadImage();
    client.setImage(QStringLiteral("/tmp/deeplux_sam_next_image.png"));

    QSignalSpy errorSpy(&client, &SamBackendClient::errorOccurred);
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(waitFor([&] { return client.state() == SamBackendClient::State::Error; }, 5000),
             "pending setImage must still be protected by a timeout after the unload reply settled");
    QVERIFY2(timer.elapsed() < 3000, qPrintable(QString("timeout fired too late: %1ms").arg(timer.elapsed())));
    QVERIFY2(errorSpy.count() >= 1, "hung setImage must surface a timeout error");
}

void TestSamBackendClient::httpServiceConsecutiveSetImageLatestWins() {
    // 连续快速切换图像：第一个 set_image 被延迟，第二个紧随其后发出。
    // latest-wins 要求第二个请求生效；延迟的第一个响应（无论先到还是后到）
    // 都不得覆盖第二个的 embedding，也不得误删第二个请求。
    SamTestServer server;
    QVERIFY2(server.listen(), "test HTTP server must listen");
    server.delayMs[QStringLiteral("/set_image")] = 300; // 第一个 set_image 延迟

    SamBackendClient client;
    client.setServerUrl(server.url());
    QSignalSpy embeddingSpy(&client, &SamBackendClient::embeddingReady);
    QSignalSpy errorSpy(&client, &SamBackendClient::errorOccurred);

    client.setImage(QStringLiteral("/img/a.png")); // 延迟 300ms
    QTest::qWait(50);                              // 让第一个请求在途
    server.delayMs.clear();                        // 之后的 set_image 立即响应
    client.setImage(QStringLiteral("/img/b.png")); // 取消第一个，第二个立即返回

    // 第二个请求的 embedding 生效，且是唯一生效的 embedding
    QVERIFY2(waitFor([&] { return client.currentEmbeddingId() == QStringLiteral("emb-2"); }, 3000),
             "latest setImage must win");
    QCOMPARE(client.state(), SamBackendClient::State::Ready);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(embeddingSpy.count(), 1); // 被取消的第一个不应产生 embedding
    QCOMPARE(embeddingSpy.takeFirst().at(0).toString(), QString("emb-2"));
}

void TestSamBackendClient::oversizedDeclaredResponseIsAbortedBeforeBuffering() {
    SamTestServer server;
    QVERIFY2(server.listen(), "test HTTP server must listen");
    server.responseContentLengths.insert(QStringLiteral("/health"), 64LL * 1024 * 1024 + 1);

    SamBackendClient client;
    client.setServerUrl(server.url());
    QSignalSpy errorSpy(&client, &SamBackendClient::errorOccurred);
    client.healthCheck();

    QVERIFY2(waitFor([&] { return client.state() == SamBackendClient::State::Error; }),
             "oversized response must fail before its body is buffered");
    QVERIFY(errorSpy.count() >= 1);
    QVERIFY(errorSpy.takeFirst().at(0).toString().contains(QStringLiteral("尺寸上限")));
}

// 批3 复核二轮（P1-3）：畸形/缺字段 predict 响应必须失败关闭——
// 旧实现 toArray()/toDouble() 把字符串、布尔值与缺失字段静默转零，
// "三个错误坐标仍可成为有效多边形"
void TestSamBackendClient::predictResponseStrictParsing() {
    SamTestServer server;
    QVERIFY2(server.listen(), "test HTTP server must listen");
    server.usePredictOverride = true;

    const QJsonObject valid{{"polygon", QJsonArray{QJsonArray{1.0, 2.0}, QJsonArray{3.0, 4.0}, QJsonArray{5.0, 6.0}}},
                            {"bbox", QJsonArray{1, 2, 3, 4}},
                            {"score", 0.9}};

    // 每个用例独立客户端：驱动 setImage→embeddingReady→predict，断言 error 且无 predictionReady
    const auto driveExpectError = [&](const QJsonObject& response, const QString& expectSubstr) {
        server.predictResponseOverride = response;
        SamBackendClient client;
        client.setServerUrl(server.url());
        client.setTimeoutMs(2000);
        QSignalSpy errorSpy(&client, &SamBackendClient::errorOccurred);
        QSignalSpy predictSpy(&client, &SamBackendClient::predictionReady);
        QObject::connect(&client, &SamBackendClient::embeddingReady, &client,
                         [&client]() { client.predict({QPointF(1, 2)}, {}, QRectF()); });
        client.setImage(QStringLiteral("/tmp/deeplux_strict_parse.png"));
        QVERIFY2(waitFor([&] { return errorSpy.count() > 0 || predictSpy.count() > 0; }),
                 qPrintable(QStringLiteral("no outcome for: %1").arg(expectSubstr)));
        QCOMPARE(predictSpy.count(), 0); // 不得伪造预测结果
        QVERIFY(errorSpy.count() >= 1);
        QVERIFY2(
            errorSpy.first().at(0).toString().contains(expectSubstr),
            qPrintable(QStringLiteral("expect '%1', got: %2").arg(expectSubstr, errorSpy.first().at(0).toString())));
    };

    // 未知 status → 拒绝
    QJsonObject r = valid;
    r["status"] = QStringLiteral("weird");
    driveExpectError(r, QStringLiteral("未知状态"));
    // ok=false + error 字段（真实 server "embedding not found" 形态）→ 拒绝并透传原因
    driveExpectError(QJsonObject{{"ok", false}, {"error", QStringLiteral("embedding not found")}},
                     QStringLiteral("embedding not found"));
    // 非空 error 字段（无 status）→ 拒绝
    driveExpectError(QJsonObject{{"error", QStringLiteral("boom")}}, QStringLiteral("boom"));
    // polygon 缺失 / 非数组 / 元素形态错 / 元素长度错
    driveExpectError(QJsonObject{{"bbox", QJsonArray{1, 2, 3, 4}}, {"score", 0.9}}, QStringLiteral("polygon"));
    r = valid;
    r["polygon"] = QStringLiteral("abc");
    driveExpectError(r, QStringLiteral("polygon"));
    r = valid;
    r["polygon"] = QJsonArray{QJsonArray{QStringLiteral("a"), QStringLiteral("b")}};
    driveExpectError(r, QStringLiteral("polygon"));
    r = valid;
    r["polygon"] = QJsonArray{QJsonArray{1.0, 2.0, 3.0}};
    driveExpectError(r, QStringLiteral("polygon"));
    // bbox 长度错 / 非数组 → 拒绝
    r = valid;
    r["bbox"] = QJsonArray{1, 2, 3};
    driveExpectError(r, QStringLiteral("bbox"));
    r = valid;
    r["bbox"] = true;
    driveExpectError(r, QStringLiteral("bbox"));
    // score 缺失 / 越界 / 非数值 → 拒绝
    r = valid;
    r.remove(QStringLiteral("score"));
    driveExpectError(r, QStringLiteral("score"));
    r = valid;
    r["score"] = 1.5;
    driveExpectError(r, QStringLiteral("score"));
    r = valid;
    r["score"] = QStringLiteral("high");
    driveExpectError(r, QStringLiteral("score"));
    // mask_rle / model_name / mask_png_base64 类型错误 → 拒绝
    r = valid;
    r["mask_rle"] = 42;
    driveExpectError(r, QStringLiteral("mask_rle"));
    r = valid;
    r["model_name"] = 7;
    driveExpectError(r, QStringLiteral("model_name"));
    r = valid;
    r["mask_png_base64"] = QJsonArray{1};
    driveExpectError(r, QStringLiteral("mask_png_base64"));
    // 批3 复核三轮（P2-4）：status/ok/error 存在即强制类型——status:true 经
    // toString() 为空串会被当"字段缺失"，带合法 polygon 即误发 predictionReady
    r = valid;
    r["status"] = true;
    driveExpectError(r, QStringLiteral("status"));
    r = valid;
    r["ok"] = QStringLiteral("false");
    driveExpectError(r, QStringLiteral("ok"));
    r = valid;
    r["error"] = 42;
    driveExpectError(r, QStringLiteral("error"));

    // 对照：无 status（真实 sam_server 成功响应不带 status）+ 字段合法 → 成功
    server.predictResponseOverride = valid;
    SamBackendClient client;
    client.setServerUrl(server.url());
    client.setTimeoutMs(2000);
    QSignalSpy errorSpy(&client, &SamBackendClient::errorOccurred);
    QSignalSpy predictSpy(&client, &SamBackendClient::predictionReady);
    QObject::connect(&client, &SamBackendClient::embeddingReady, &client,
                     [&client]() { client.predict({QPointF(1, 2)}, {}, QRectF()); });
    client.setImage(QStringLiteral("/tmp/deeplux_strict_parse.png"));
    QVERIFY2(waitFor([&] { return predictSpy.count() > 0 || errorSpy.count() > 0; }),
             "status-less valid response must succeed (real sam_server shape)");
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(predictSpy.count(), 1);
    const auto args = predictSpy.first();
    QCOMPARE(args.at(0).value<QList<QPointF>>().size(), 3);
    QCOMPARE(args.at(2).toDouble(), 0.9);
    server.usePredictOverride = false;
}

QTEST_MAIN(TestSamBackendClient)
#include "test_sambackendclient.moc"
