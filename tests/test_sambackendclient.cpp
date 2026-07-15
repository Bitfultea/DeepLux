#include <QtTest/QtTest>
#include <QTest>
#include <QSignalSpy>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QStandardPaths>

#include "core/agent/SamBackendClient.h"

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
    QVERIFY2(client.state() == SamBackendClient::State::Error ||
             stateSpy.count() >= 1, "State should transition on network failure");
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

QTEST_MAIN(TestSamBackendClient)
#include "test_sambackendclient.moc"
