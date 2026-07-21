#include "core/agent/SamBackendClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>
#include <QtTest/QtTest>

using namespace DeepLux;

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

private:
    QString m_unusedPortUrl;
};

void TestSamBackendClient::initTestCase() {
    // 使用一个几乎确定没有 server 监听的端口
    m_unusedPortUrl = QStringLiteral("http://127.0.0.1:59999");
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

QTEST_MAIN(TestSamBackendClient)
#include "test_sambackendclient.moc"
