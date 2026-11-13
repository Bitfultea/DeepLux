#include <QDataStream>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <QtTest/QtTest>
#include <atomic>
#include <core/agent/AgentActor.h>
#include <core/agent/ToolSchema.h>
#include <core/engine/RunEngine.h>
#include <core/model/ImageData.h>
#include <core/model/Project.h>
#include <functional>
#include <limits>
#include <plugins/communication/PLCReadPlugin/PLCReadPlugin.h>
#include <plugins/communication/PLCWritePlugin/PLCWritePlugin.h>
#include <thread>
#include <utility>

using namespace DeepLux;

namespace {

struct ModbusRequest {
    quint16 transactionId = 0;
    quint16 protocolId = 0;
    quint16 length = 0;
    quint8 unitId = 0;
    quint8 function = 0;
    quint16 address = 0;
    quint16 quantity = 0;
    quint8 byteCount = 0;
    QVector<quint16> values;
};

bool parseRequest(const QByteArray& frame, ModbusRequest& request) {
    if (frame.size() < 12)
        return false;
    QDataStream stream(frame);
    stream.setByteOrder(QDataStream::BigEndian);
    stream >> request.transactionId >> request.protocolId >> request.length >> request.unitId >> request.function >>
        request.address >> request.quantity;
    if (stream.status() != QDataStream::Ok)
        return false;
    if (request.function == 0x10) {
        stream >> request.byteCount;
        if (request.byteCount != request.quantity * 2 || frame.size() != 13 + request.byteCount)
            return false;
        for (int i = 0; i < request.quantity; ++i) {
            quint16 value = 0;
            stream >> value;
            request.values.append(value);
        }
    }
    return stream.status() == QDataStream::Ok;
}

QByteArray buildReadResponse(const ModbusRequest& request, const QVector<quint16>& values,
                             bool wrongTransaction = false) {
    QByteArray frame;
    QDataStream stream(&frame, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    const quint8 byteCount = static_cast<quint8>(values.size() * 2);
    stream << static_cast<quint16>(request.transactionId + (wrongTransaction ? 1 : 0)) << quint16(0)
           << static_cast<quint16>(3 + values.size() * 2) << request.unitId << quint8(0x03) << byteCount;
    for (quint16 value : values)
        stream << value;
    return frame;
}

QByteArray buildWriteEcho(const QByteArray& request, bool wrongTransaction = false, bool wrongAddress = false) {
    QByteArray response = request.left(12);
    response[4] = 0;
    response[5] = 6;
    if (wrongTransaction)
        response[1] = static_cast<char>(response[1] + 1);
    if (wrongAddress)
        response[9] = static_cast<char>(response[9] + 1);
    return response;
}

QByteArray buildWriteException(const QByteArray& request) {
    ModbusRequest parsed;
    if (!parseRequest(request, parsed))
        return {};
    QByteArray response;
    QDataStream stream(&response, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << parsed.transactionId << quint16(0) << quint16(3) << parsed.unitId << quint8(0x90) << quint8(2);
    return response;
}

bool runPLCReadInWorker(const std::function<bool()>& run) {
    std::atomic_bool finished{false};
    bool result = false;
    std::thread worker([&] {
        result = run();
        finished.store(true, std::memory_order_release);
    });
    QElapsedTimer wait;
    wait.start();
    while (!finished.load(std::memory_order_acquire) && wait.elapsed() < 5000)
        QTest::qWait(1);
    worker.join();
    return finished.load(std::memory_order_acquire) && result;
}

class ModbusTcpPeer final : public QObject {
public:
    using ReplyHandler = std::function<QByteArray(const QByteArray&)>;

    ModbusTcpPeer() {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections()) {
                if (m_socket)
                    m_socket->deleteLater();
                m_socket = m_server.nextPendingConnection();
                ++m_connectionCount;
                connect(m_socket, &QTcpSocket::readyRead, this, [this] { receive(); });
            }
        });
    }

    bool listen() {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    quint16 port() const {
        return m_server.serverPort();
    }
    void setReplyHandler(ReplyHandler handler) {
        m_replyHandler = std::move(handler);
    }
    void splitNextResponseAt(int byteCount) {
        m_splitAt = byteCount;
    }
    int requestCount() const {
        return m_requestCount;
    }
    int connectionCount() const {
        return m_connectionCount;
    }
    void closeListener() {
        m_server.close();
    }
    QByteArray lastRequest() const {
        return m_lastRequest;
    }

private:
    void receive() {
        m_buffer += m_socket->readAll();
        while (m_buffer.size() >= 6) {
            const auto* bytes = reinterpret_cast<const uchar*>(m_buffer.constData());
            const int frameSize = 6 + qFromBigEndian<quint16>(bytes + 4);
            if (m_buffer.size() < frameSize)
                return;
            m_lastRequest = m_buffer.left(frameSize);
            m_buffer.remove(0, frameSize);
            ++m_requestCount;
            if (m_replyHandler) {
                const QByteArray response = m_replyHandler(m_lastRequest);
                if (!response.isEmpty()) {
                    if (m_splitAt > 0 && m_splitAt < response.size()) {
                        m_socket->write(response.left(m_splitAt));
                        const QByteArray remainder = response.mid(m_splitAt);
                        QTimer::singleShot(10, this, [this, remainder] {
                            if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
                                m_socket->write(remainder);
                        });
                        m_splitAt = 0;
                    } else {
                        m_socket->write(response);
                    }
                    m_socket->flush();
                }
            }
        }
    }

    QTcpServer m_server;
    QTcpSocket* m_socket = nullptr;
    QByteArray m_buffer;
    QByteArray m_lastRequest;
    int m_requestCount = 0;
    int m_connectionCount = 0;
    int m_splitAt = 0;
    ReplyHandler m_replyHandler;
};

} // namespace

class TestCommunicationPlugins : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
    }
    void plcReadRoundTrip();
    void plcReadRejectsMismatchedResponse_data();
    void plcReadRejectsMismatchedResponse();
    void plcReadTimeoutDoesNotRetry();
    void plcWriteRoundTrip();
    void plcWriteRejectsMismatchedResponse_data();
    void plcWriteRejectsMismatchedResponse();
    void plcWriteTimeoutDoesNotRetry();
    void plcWriteTypedOutcomeUncertain_data();
    void plcWriteTypedOutcomeUncertain();
    void plcWriteTypedConnectFailureIsNotUncertain();
    void plcWriteCycleTimeoutIsNotReplayedAndReportedUncertain();
    void plcWriteVariableScalarRoundTrip();
    void plcWriteVariableArrayRoundTrip();
    void plcWriteVariableRejectsInvalidInput_data();
    void plcWriteVariableRejectsInvalidInput();
};

void TestCommunicationPlugins::plcWriteTypedOutcomeUncertain_data() {
    QTest::addColumn<QString>("responseKind");
    QTest::newRow("no-response") << QStringLiteral("none");
    QTest::newRow("modbus-exception") << QStringLiteral("exception");
    QTest::newRow("invalid-echo") << QStringLiteral("invalid");
    QTest::newRow("valid-echo") << QStringLiteral("valid");
}

void TestCommunicationPlugins::plcWriteTypedOutcomeUncertain() {
    QFETCH(QString, responseKind);
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    peer.setReplyHandler([responseKind](const QByteArray& request) {
        if (responseKind == QStringLiteral("exception"))
            return buildWriteException(request);
        if (responseKind == QStringLiteral("invalid"))
            return buildWriteEcho(request, true);
        if (responseKind == QStringLiteral("valid"))
            return buildWriteEcho(request);
        return QByteArray{};
    });

    PLCWritePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 9},
                                 {"startAddress", 100},
                                 {"values", QJsonArray{0x1234}},
                                 {"timeout", 100}});
    PortValueMap inputs;
    PortValueMap outputs;
    ExecutionContext context;
    context.runId = QStringLiteral("uncertain-plc-write");
    const ExecutionResult result = plugin.execute(inputs, outputs, context);
    QCOMPARE(peer.requestCount(), 1);
    if (responseKind == QStringLiteral("valid")) {
        QVERIFY(result.success);
        QCOMPARE(result.errorCode, ExecError::None);
    } else {
        QVERIFY(!result.success);
        QCOMPARE(result.errorCode, ExecError::SideEffectUncertain);
        QVERIFY(result.userMessage.contains(QStringLiteral("无法确认")));
    }
    plugin.shutdown();
}

void TestCommunicationPlugins::plcWriteTypedConnectFailureIsNotUncertain() {
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    const quint16 closedPort = peer.port();
    peer.closeListener();

    PLCWritePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", closedPort},
                                 {"slaveId", 9},
                                 {"startAddress", 100},
                                 {"values", QJsonArray{0x1234}},
                                 {"timeout", 100}});
    PortValueMap outputs;
    ExecutionContext context;
    const ExecutionResult result = plugin.execute(PortValueMap{}, outputs, context);
    QVERIFY(!result.success);
    QVERIFY(result.errorCode != ExecError::SideEffectUncertain);
    plugin.shutdown();
}

void TestCommunicationPlugins::plcWriteCycleTimeoutIsNotReplayedAndReportedUncertain() {
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    peer.setReplyHandler([](const QByteArray&) { return QByteArray{}; });

    RunEngine& engine = RunEngine::instance();
    engine.stop();
    engine.clearModules();
    Project project;
    ModuleInstance instance;
    instance.id = QStringLiteral("plc_write_uncertain_cycle");
    instance.moduleId = QStringLiteral("com.deeplux.plugin.plcwrite");
    instance.params = QJsonObject{{"ipAddress", "127.0.0.1"}, {"port", peer.port()},          {"slaveId", 9},
                                  {"startAddress", 100},      {"values", QJsonArray{0x1234}}, {"timeout", 100}};
    project.addModule(instance);
    QVERIFY(engine.loadProject(&project, [](const ModuleInstance&) { return new PLCWritePlugin(); }));

    QSignalSpy finishedSpy(&engine, &RunEngine::runFinished);
    QSignalSpy stoppedSpy(&engine, &RunEngine::cycleStopped);
    QString sessionId;
    QVERIFY(engine.startCycle(&sessionId, 5, true, true));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(stoppedSpy.count(), 1, 1000);
    QTest::qWait(100); // 超过三个周期，确保超时失败没有重放 PLCWrite。

    QCOMPARE(peer.requestCount(), 1);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(!engine.isCycleMode());
    QVERIFY(!engine.isBusy());
    const RunObservation observation = engine.runObservation(sessionId);
    QVERIFY(observation.finished);
    QVERIFY(!observation.result.success);
    QCOMPARE(observation.result.errorCode, ExecError::SideEffectUncertain);
    QCOMPARE(observation.result.failedModule, instance.id);
    QCOMPARE(observation.lastCompletedRunId, observation.lastIterationRunId);

    AgentActor actor;
    const QJsonObject result =
        actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), sessionId}});
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
    QCOMPARE(result.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
    QVERIFY(!result.value(QStringLiteral("retryable")).toBool());
    QCOMPARE(result.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("code")).toInt(),
             ExecError::SideEffectUncertain);
    engine.clearModules();
}

void TestCommunicationPlugins::plcReadRoundTrip() {
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    bool requestValid = false;
    const QVector<quint16> expected{0x1234, 0xABCD};
    peer.setReplyHandler([&](const QByteArray& frame) {
        ModbusRequest request;
        requestValid = parseRequest(frame, request) && frame.size() == 12 && request.protocolId == 0 &&
                       request.transactionId == 1 && request.unitId == 7 && request.function == 0x03 &&
                       request.address == 42 && request.quantity == expected.size() && request.length == 6;
        return buildReadResponse(request, expected);
    });
    peer.splitNextResponseAt(3);

    PLCReadPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 7},
                                 {"startAddress", 42},
                                 {"readCount", 2},
                                 {"outputVariable", "registers"},
                                 {"timeout", 300}});
    ImageData input;
    ImageData output;
    QVERIFY(runPLCReadInWorker([&] { return plugin.execute(input, output); }));
    QVERIFY(requestValid);
    QCOMPARE(peer.requestCount(), 1);
    const QJsonArray registers = output.data(QStringLiteral("registers")).toJsonArray();
    QCOMPARE(registers.size(), 2);
    QCOMPARE(registers.at(0).toInt(), 0x1234);
    QCOMPARE(registers.at(1).toInt(), 0xABCD);
    QCOMPARE(output.data(QStringLiteral("registers_count")).toInt(), 2);
    plugin.shutdown();
}

void TestCommunicationPlugins::plcReadRejectsMismatchedResponse_data() {
    QTest::addColumn<QString>("mismatch");
    QTest::newRow("transaction") << QStringLiteral("transaction");
    QTest::newRow("byte-count") << QStringLiteral("byte-count");
}

void TestCommunicationPlugins::plcReadRejectsMismatchedResponse() {
    QFETCH(QString, mismatch);
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    peer.setReplyHandler([&](const QByteArray& frame) {
        ModbusRequest request;
        if (!parseRequest(frame, request))
            return QByteArray{};
        return mismatch == QStringLiteral("transaction") ? buildReadResponse(request, {0x1111, 0x2222}, true)
                                                         : buildReadResponse(request, {0x1111, 0x2222, 0x3333});
    });

    PLCReadPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 7},
                                 {"startAddress", 42},
                                 {"readCount", 2},
                                 {"outputVariable", "registers"},
                                 {"timeout", 300}});
    QSignalSpy errors(&plugin, &ModuleBase::errorOccurred);
    ImageData input;
    ImageData output;
    QVERIFY(!runPLCReadInWorker([&] { return plugin.execute(input, output); }));
    QVERIFY(errors.count() >= 1);
    QVERIFY(!output.data(QStringLiteral("registers")).isValid());
    QCOMPARE(peer.requestCount(), 1);
    plugin.shutdown();
}

void TestCommunicationPlugins::plcReadTimeoutDoesNotRetry() {
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    peer.setReplyHandler([](const QByteArray&) { return QByteArray{}; });
    PLCReadPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 7},
                                 {"startAddress", 42},
                                 {"readCount", 2},
                                 {"timeout", 100}});
    QSignalSpy errors(&plugin, &ModuleBase::errorOccurred);
    ImageData input;
    ImageData output;
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(!runPLCReadInWorker([&] { return plugin.execute(input, output); }));
    QVERIFY(errors.count() >= 1);
    QVERIFY(elapsed.elapsed() <= 350);
    QCOMPARE(peer.requestCount(), 1);
    plugin.shutdown();
}

void TestCommunicationPlugins::plcWriteRoundTrip() {
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    bool requestValid = false;
    peer.setReplyHandler([&](const QByteArray& frame) {
        ModbusRequest request;
        requestValid = parseRequest(frame, request) && request.protocolId == 0 && request.unitId == 9 &&
                       request.transactionId == 1 && request.function == 0x10 && request.address == 100 &&
                       request.quantity == 2 && request.byteCount == 4 &&
                       request.values == QVector<quint16>({0x1234, 0xABCD}) && request.length == 11 &&
                       frame.size() == 17;
        return buildWriteEcho(frame);
    });
    peer.splitNextResponseAt(4);

    PLCWritePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 9},
                                 {"startAddress", 100},
                                 {"values", QJsonArray{0x1234, 0xABCD}},
                                 {"timeout", 300}});
    ImageData input;
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QVERIFY(requestValid);
    QCOMPARE(peer.requestCount(), 1);
    QVERIFY(output.data(QStringLiteral("plc_write_success")).toBool());
    QCOMPARE(output.data(QStringLiteral("plc_write_count")).toInt(), 2);
    plugin.shutdown();
}

void TestCommunicationPlugins::plcWriteRejectsMismatchedResponse_data() {
    QTest::addColumn<QString>("mismatch");
    QTest::newRow("transaction") << QStringLiteral("transaction");
    QTest::newRow("echo-address") << QStringLiteral("echo-address");
}

void TestCommunicationPlugins::plcWriteRejectsMismatchedResponse() {
    QFETCH(QString, mismatch);
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    peer.setReplyHandler([&](const QByteArray& frame) {
        return buildWriteEcho(frame, mismatch == QStringLiteral("transaction"),
                              mismatch == QStringLiteral("echo-address"));
    });

    PLCWritePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 9},
                                 {"startAddress", 100},
                                 {"values", QJsonArray{0x1234, 0xABCD}},
                                 {"timeout", 300}});
    QSignalSpy errors(&plugin, &ModuleBase::errorOccurred);
    ImageData input;
    ImageData output;
    QVERIFY(!plugin.execute(input, output));
    QVERIFY(errors.count() >= 1);
    QVERIFY(!output.data(QStringLiteral("plc_write_success")).isValid());
    QCOMPARE(peer.requestCount(), 1);
    plugin.shutdown();
}

void TestCommunicationPlugins::plcWriteTimeoutDoesNotRetry() {
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    peer.setReplyHandler([](const QByteArray&) { return QByteArray{}; });
    PLCWritePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 9},
                                 {"startAddress", 100},
                                 {"values", QJsonArray{0x1234, 0xABCD}},
                                 {"timeout", 100}});
    QSignalSpy errors(&plugin, &ModuleBase::errorOccurred);
    ImageData input;
    ImageData output;
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(!plugin.execute(input, output));
    QVERIFY(errors.count() >= 1);
    QVERIFY(elapsed.elapsed() <= 350);
    QCOMPARE(peer.requestCount(), 1);
    QVERIFY(!output.data(QStringLiteral("plc_write_success")).isValid());
    plugin.shutdown();
}

void TestCommunicationPlugins::plcWriteVariableScalarRoundTrip() {
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    bool requestValid = false;
    peer.setReplyHandler([&](const QByteArray& frame) {
        ModbusRequest request;
        requestValid = parseRequest(frame, request) && request.function == 0x10 && request.address == 65535 &&
                       request.quantity == 1 && request.values == QVector<quint16>{0x4567};
        return buildWriteEcho(frame);
    });

    PLCWritePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 9},
                                 {"startAddress", 65535},
                                 {"valueSource", "variable"},
                                 {"values", "register"},
                                 {"timeout", 300}});
    ImageData input;
    input.setData(QStringLiteral("register"), 0x4567);
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QVERIFY(requestValid);
    QCOMPARE(peer.connectionCount(), 1);
    QCOMPARE(peer.requestCount(), 1);
    QCOMPARE(output.data(QStringLiteral("plc_write_count")).toInt(), 1);
    plugin.shutdown();
}

void TestCommunicationPlugins::plcWriteVariableArrayRoundTrip() {
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());
    QVector<quint16> expected;
    QJsonArray values;
    for (int i = 0; i < 123; ++i) {
        expected.append(static_cast<quint16>(i));
        values.append(i);
    }
    bool requestValid = false;
    peer.setReplyHandler([&](const QByteArray& frame) {
        ModbusRequest request;
        requestValid = parseRequest(frame, request) && request.function == 0x10 && request.address == 65413 &&
                       request.quantity == 123 && request.values == expected;
        return buildWriteEcho(frame);
    });

    PLCWritePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 9},
                                 {"startAddress", 65413},
                                 {"valueSource", "variable"},
                                 {"values", "registers"},
                                 {"timeout", 300}});
    ImageData input;
    input.setData(QStringLiteral("registers"), values);
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QVERIFY(requestValid);
    QCOMPARE(peer.connectionCount(), 1);
    QCOMPARE(peer.requestCount(), 1);
    QCOMPARE(output.data(QStringLiteral("plc_write_count")).toInt(), 123);
    plugin.shutdown();
}

void TestCommunicationPlugins::plcWriteVariableRejectsInvalidInput_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<int>("address");
    QTest::addColumn<double>("overrideAddress");
    QTest::newRow("missing-variable") << QStringLiteral("missing") << 100 << 0.0;
    QTest::newRow("empty-array") << QStringLiteral("empty") << 100 << 0.0;
    QTest::newRow("string-scalar") << QStringLiteral("string") << 100 << 0.0;
    QTest::newRow("bool-scalar") << QStringLiteral("bool") << 100 << 0.0;
    QTest::newRow("fractional-scalar") << QStringLiteral("fraction") << 100 << 0.0;
    QTest::newRow("negative-scalar") << QStringLiteral("negative") << 100 << 0.0;
    QTest::newRow("overflow-scalar") << QStringLiteral("overflow") << 100 << 0.0;
    QTest::newRow("nan-scalar") << QStringLiteral("nan") << 100 << 0.0;
    QTest::newRow("infinite-scalar") << QStringLiteral("infinite") << 100 << 0.0;
    QTest::newRow("array-string-item") << QStringLiteral("array-string") << 100 << 0.0;
    QTest::newRow("array-fractional-item") << QStringLiteral("array-fraction") << 100 << 0.0;
    QTest::newRow("array-overflow-item") << QStringLiteral("array-overflow") << 100 << 0.0;
    QTest::newRow("array-too-many") << QStringLiteral("too-many") << 100 << 0.0;
    QTest::newRow("address-overflow") << QStringLiteral("address-overflow") << 65535 << 0.0;
    QTest::newRow("address-negative-set-param") << QStringLiteral("address-override") << 65535 << -1.0;
    QTest::newRow("address-overflow-set-param") << QStringLiteral("address-override") << 100 << 65536.0;
    QTest::newRow("address-fraction-set-param") << QStringLiteral("address-override") << 100 << 12.5;
}

void TestCommunicationPlugins::plcWriteVariableRejectsInvalidInput() {
    QFETCH(QString, kind);
    QFETCH(int, address);
    QFETCH(double, overrideAddress);
    ModbusTcpPeer peer;
    QVERIFY(peer.listen());

    PLCWritePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"ipAddress", "127.0.0.1"},
                                 {"port", peer.port()},
                                 {"slaveId", 9},
                                 {"startAddress", address},
                                 {"valueSource", "variable"},
                                 {"values", "registers"},
                                 {"timeout", 300}});
    if (kind == QStringLiteral("address-override"))
        plugin.setParam(QStringLiteral("startAddress"), overrideAddress);
    ImageData input;
    if (kind == QStringLiteral("empty"))
        input.setData(QStringLiteral("registers"), QJsonArray{});
    else if (kind == QStringLiteral("string"))
        input.setData(QStringLiteral("registers"), QStringLiteral("123"));
    else if (kind == QStringLiteral("bool"))
        input.setData(QStringLiteral("registers"), true);
    else if (kind == QStringLiteral("fraction"))
        input.setData(QStringLiteral("registers"), 1.5);
    else if (kind == QStringLiteral("negative"))
        input.setData(QStringLiteral("registers"), -1);
    else if (kind == QStringLiteral("overflow"))
        input.setData(QStringLiteral("registers"), 65536);
    else if (kind == QStringLiteral("nan"))
        input.setData(QStringLiteral("registers"), std::numeric_limits<double>::quiet_NaN());
    else if (kind == QStringLiteral("infinite"))
        input.setData(QStringLiteral("registers"), std::numeric_limits<double>::infinity());
    else if (kind == QStringLiteral("array-string"))
        input.setData(QStringLiteral("registers"), QJsonArray{QStringLiteral("12")});
    else if (kind == QStringLiteral("array-fraction"))
        input.setData(QStringLiteral("registers"), QJsonArray{1.5});
    else if (kind == QStringLiteral("array-overflow"))
        input.setData(QStringLiteral("registers"), QJsonArray{65536});
    else if (kind == QStringLiteral("too-many")) {
        QJsonArray values;
        for (int i = 0; i < 124; ++i)
            values.append(i);
        input.setData(QStringLiteral("registers"), values);
    } else if (kind == QStringLiteral("address-overflow"))
        input.setData(QStringLiteral("registers"), QJsonArray{1, 2});

    QSignalSpy errors(&plugin, &ModuleBase::errorOccurred);
    ImageData output;
    QVERIFY(!plugin.execute(input, output));
    QVERIFY(errors.count() >= 1);
    QCOMPARE(peer.connectionCount(), 0);
    QCOMPARE(peer.requestCount(), 0);
    plugin.shutdown();
}

QTEST_MAIN(TestCommunicationPlugins)
#include "test_communicationplugins.moc"
