#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/ToolSchema.h"
#include "core/common/Logger.h"
#include "core/common/UserManager.h"
#include "core/engine/RunEngine.h"
#include "core/manager/GlobalVarManager.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest/QtTest>
#include <algorithm>

using namespace DeepLux;

namespace {

class ModbusReadPeer final {
public:
    ~ModbusReadPeer() {
        if (m_thread.isRunning()) {
            m_thread.quit();
            if (!m_thread.wait(3000))
                qFatal("ModbusReadPeer worker did not stop within 3000 ms");
        }
    }

    bool listen() {
        m_worker = new QObject;
        m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
        bool listening = false;
        QMetaObject::invokeMethod(
            m_worker,
            [this, &listening] {
                m_server = new QTcpServer(m_worker);
                QObject::connect(m_server, &QTcpServer::newConnection, m_worker, [this] {
                    while (QTcpSocket* socket = m_server->nextPendingConnection()) {
                        auto* buffer = new QByteArray;
                        QObject::connect(socket, &QTcpSocket::readyRead, m_worker, [this, socket, buffer] {
                            *buffer += socket->readAll();
                            while (buffer->size() >= 12) {
                                const QByteArray request = buffer->left(12);
                                buffer->remove(0, 12);
                                const auto* bytes = reinterpret_cast<const uchar*>(request.constData());
                                const quint16 transactionId = qFromBigEndian<quint16>(bytes);
                                const quint8 unitId = static_cast<quint8>(bytes[6]);
                                bool rejectRead = false;
                                {
                                    QMutexLocker locker(&m_mutex);
                                    m_requests.append(request);
                                    rejectRead = m_failNextRead;
                                    m_failNextRead = false;
                                }
                                QByteArray response;
                                response.resize(rejectRead ? 9 : 13);
                                auto* responseBytes = reinterpret_cast<uchar*>(response.data());
                                qToBigEndian(transactionId, responseBytes);
                                qToBigEndian<quint16>(0, responseBytes + 2);
                                qToBigEndian<quint16>(rejectRead ? 3 : 7, responseBytes + 4);
                                responseBytes[6] = unitId;
                                responseBytes[7] = rejectRead ? 0x83 : 0x03;
                                if (rejectRead) {
                                    responseBytes[8] = 2;
                                } else {
                                    responseBytes[8] = 4;
                                    qToBigEndian<quint16>(0x1234, responseBytes + 9);
                                    qToBigEndian<quint16>(0xABCD, responseBytes + 11);
                                }
                                socket->write(response);
                                socket->flush();
                            }
                        });
                        QObject::connect(socket, &QObject::destroyed, m_worker, [buffer] { delete buffer; });
                    }
                });
                listening = m_server->listen(QHostAddress::LocalHost, 0);
                if (listening) {
                    QMutexLocker locker(&m_mutex);
                    m_port = m_server->serverPort();
                }
            },
            Qt::BlockingQueuedConnection);
        return listening;
    }

    quint16 port() const {
        QMutexLocker locker(&m_mutex);
        return m_port;
    }

    QList<QByteArray> requests() const {
        QMutexLocker locker(&m_mutex);
        return m_requests;
    }

    void rejectNextRead() {
        QMutexLocker locker(&m_mutex);
        m_failNextRead = true;
    }

private:
    QThread m_thread;
    QObject* m_worker = nullptr;
    QTcpServer* m_server = nullptr;
    mutable QMutex m_mutex;
    quint16 m_port = 0;
    QList<QByteArray> m_requests;
    bool m_failNextRead = false;
};

} // namespace

class TestAgentCorePluginsG8 : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appDataDir;
    QByteArray m_previousAppDataDir;
    bool m_hadAppDataDir = false;

    static bool installPlugin(const QString& root, const QString& directoryName, const QString& library,
                              const QString& metadata) {
        const QString directory = QDir(root).filePath(directoryName);
        if (!QDir().mkpath(directory))
            return false;
        return QFile::copy(library, QDir(directory).filePath(QFileInfo(library).fileName())) &&
               QFile::copy(metadata, QDir(directory).filePath(QStringLiteral("metadata.json")));
    }

    static QJsonObject runToTerminal(AgentActor& actor) {
        const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"),
                                                      QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")},
                                                                  {QStringLiteral("wait"), false}});
        if (started.value(QStringLiteral("status")).toString() != QLatin1String("started"))
            return started;

        const QString runId = started.value(QStringLiteral("runId")).toString();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            const QJsonObject result =
                actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
            const QString status = result.value(QStringLiteral("status")).toString();
            if (status == QLatin1String("completed") || status == QLatin1String("failed") ||
                status == QLatin1String("cancelled") || status == QLatin1String("uncertain"))
                return result;
            QTest::qWait(5);
        }
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("test_timeout")}};
    }

    static void clearProject() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        GlobalVarManager::instance().clearAllVariables();
    }

private slots:
    void initTestCase() {
        QVERIFY(m_appDataDir.isValid());
        m_hadAppDataDir = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
        m_previousAppDataDir = qgetenv("DEEPLUX_APP_DATA_DIR");
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());

        const QString pluginRoot = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins"));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("PLCReadPlugin"), QStringLiteral(G8_PLCREAD_PLUGIN),
                              QStringLiteral(G8_PLCREAD_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MathPlugin"), QStringLiteral(G8_MATH_PLUGIN),
                              QStringLiteral(G8_MATH_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("CreateString"), QStringLiteral(G8_CREATESTRING_PLUGIN),
                              QStringLiteral(G8_CREATESTRING_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("SplitString"), QStringLiteral(G8_SPLITSTRING_PLUGIN),
                              QStringLiteral(G8_SPLITSTRING_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("StrFormatPlugin"), QStringLiteral(G8_STRFORMAT_PLUGIN),
                              QStringLiteral(G8_STRFORMAT_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("SystemTime"), QStringLiteral(G8_SYSTEMTIME_PLUGIN),
                              QStringLiteral(G8_SYSTEMTIME_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("WriteText"), QStringLiteral(G8_WRITETEXT_PLUGIN),
                              QStringLiteral(G8_WRITETEXT_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("TimeSlice"), QStringLiteral(G8_TIMESLICE_PLUGIN),
                              QStringLiteral(G8_TIMESLICE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("Condition"), QStringLiteral(G8_CONDITION_PLUGIN),
                              QStringLiteral(G8_CONDITION_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("Delay"), QStringLiteral(G8_DELAY_PLUGIN),
                              QStringLiteral(G8_DELAY_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("DataCheck"), QStringLiteral(G8_DATACHECK_PLUGIN),
                              QStringLiteral(G8_DATACHECK_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("TableOutPut"), QStringLiteral(G8_TABLEOUTPUT_PLUGIN),
                              QStringLiteral(G8_TABLEOUTPUT_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("SaveData"), QStringLiteral(G8_SAVEDATA_PLUGIN),
                              QStringLiteral(G8_SAVEDATA_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("DefectDetection"), QStringLiteral(G8_DEFECTDETECTION_PLUGIN),
                              QStringLiteral(G8_DEFECTDETECTION_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("LoadPointCloud"), QStringLiteral(G8_LOADPOINTCLOUD_PLUGIN),
                              QStringLiteral(G8_LOADPOINTCLOUD_METADATA)));
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().pluginPaths().contains(QDir(pluginRoot).absolutePath()));
        QCOMPARE(PluginManager::instance().availableModules().size(), 15);
        ToolSchema::instance().registerDefaultTools();
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        qRegisterMetaType<RunResult>("DeepLux::RunResult");
    }

    void cleanupTestCase() {
        RunEngine& engine = RunEngine::instance();
        if (engine.isBusy()) {
            engine.stop();
            QElapsedTimer timer;
            timer.start();
            while (engine.isBusy() && timer.elapsed() < 5000)
                QTest::qWait(10);
            if (engine.isBusy()) {
                QFAIL("RunEngine did not stop within 5 seconds; leaving project and plugin resources intact");
                return;
            }
        }
        clearProject();
        PluginManager::instance().shutdown();
        if (m_hadAppDataDir)
            qputenv("DEEPLUX_APP_DATA_DIR", m_previousAppDataDir);
        else
            qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void testAgentConfigurationReadsModbusAndReportsProtocolFailure() {
        ModbusReadPeer peer;
        QVERIFY(peer.listen());

        clearProject();
        AgentActor actor;
        const QJsonObject created = actor.executeTool(
            QStringLiteral("create_project"), QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 PLC read")}});
        QCOMPARE(created.value(QStringLiteral("status")).toString(), QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("PLC读取")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));

        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.plcread")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_plc_read")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        const QJsonObject configured = actor.executeTool(
            QStringLiteral("set_module_params"),
            QJsonObject{{QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("values"),
                         QJsonObject{{QStringLiteral("ipAddress"), QStringLiteral("127.0.0.1")},
                                     {QStringLiteral("port"), peer.port()},
                                     {QStringLiteral("slaveId"), 7},
                                     {QStringLiteral("startAddress"), 0x1234},
                                     {QStringLiteral("readCount"), 2},
                                     {QStringLiteral("outputVariable"), QStringLiteral("g8_plc_registers")},
                                     {QStringLiteral("timeout"), 500}}}});
        QCOMPARE(configured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));

        const QJsonObject successfulRun = runToTerminal(actor);
        QVERIFY2(successfulRun.value(QStringLiteral("status")).toString() == QLatin1String("completed"),
                 qPrintable(QStringLiteral("%1; Modbus requests=%2")
                                .arg(QString::fromUtf8(QJsonDocument(successfulRun).toJson(QJsonDocument::Compact)))
                                .arg(peer.requests().size())));
        QCOMPARE(peer.requests().size(), 1);
        const QByteArray request = peer.requests().first();
        QCOMPARE(request.size(), 12);
        const auto* requestBytes = reinterpret_cast<const uchar*>(request.constData());
        QCOMPARE(static_cast<quint8>(requestBytes[6]), quint8(7));
        QCOMPARE(static_cast<quint8>(requestBytes[7]), quint8(0x03));
        QCOMPARE(qFromBigEndian<quint16>(requestBytes + 8), quint16(0x1234));
        QCOMPARE(qFromBigEndian<quint16>(requestBytes + 10), quint16(2));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        const QJsonObject values =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), runId},
                                          {QStringLiteral("instanceId"), instanceId},
                                          {QStringLiteral("key"), QStringLiteral("g8_plc_registers")}});
        QCOMPARE(values.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(values.value(QStringLiteral("value")).toArray(), (QJsonArray{0x1234, 0xABCD}));

        peer.rejectNextRead();
        const QJsonObject failedRun = runToTerminal(actor);
        QCOMPARE(failedRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failedRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), instanceId);
        QVERIFY(error.value(QStringLiteral("message")).toString().contains(QStringLiteral("解析PLC响应失败")));
        QCOMPARE(peer.requests().size(), 2);
    }

    void testAgentConfigurationCalculatesAndRejectsDivisionByZero() {
        clearProject();
        AgentActor actor;
        const QJsonObject created = actor.executeTool(QStringLiteral("create_project"),
                                                      QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 math")}});
        QCOMPARE(created.value(QStringLiteral("status")).toString(), QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("数学运算")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));

        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.math")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_math")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        const auto configure = [&](const QString& operation, const QString& operandA, const QString& operandB) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), instanceId},
                            {QStringLiteral("values"),
                             QJsonObject{{QStringLiteral("operation"), operation},
                                         {QStringLiteral("operandA"), operandA},
                                         {QStringLiteral("operandB"), operandB},
                                         {QStringLiteral("outputVar"), QStringLiteral("g8_math_result")}}}});
        };
        struct MathModeCase {
            const char* operation;
            const char* operandA;
            const char* operandB;
            double expected;
        };
        const MathModeCase modes[] = {{"Add", "12", "3", 15.0},      {"Subtract", "12", "3", 9.0},
                                      {"Multiply", "12", "3", 36.0}, {"Divide", "12", "3", 4.0},
                                      {"Modulo", "14", "5", 4.0},    {"Power", "12", "3", 1728.0},
                                      {"Min", "12", "3", 3.0},       {"Max", "12", "3", 12.0}};
        for (const MathModeCase& mode : modes) {
            const QString operation = QString::fromLatin1(mode.operation);
            QCOMPARE(configure(operation, QString::fromLatin1(mode.operandA), QString::fromLatin1(mode.operandB))
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("params_set"));
            const QJsonObject successfulRun = runToTerminal(actor);
            QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
            const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
            QVERIFY(!runId.isEmpty());
            const QJsonObject result =
                actor.executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{QStringLiteral("runId"), runId},
                                              {QStringLiteral("instanceId"), instanceId},
                                              {QStringLiteral("key"), QStringLiteral("g8_math_result")}});
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(result.value(QStringLiteral("value")).toDouble(), mode.expected);
        }

        for (const QString& operation : {QStringLiteral("Divide"), QStringLiteral("Modulo")}) {
            QCOMPARE(configure(operation, QStringLiteral("14"), QStringLiteral("0"))
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("params_set"));
            const QJsonObject failedRun = runToTerminal(actor);
            QCOMPARE(failedRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
            const QString failedRunId = failedRun.value(QStringLiteral("runId")).toString();
            QVERIFY(!failedRunId.isEmpty());
            const QJsonObject error = failedRun.value(QStringLiteral("lastError")).toObject();
            QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), instanceId);
            QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("除数不能为0"));
            const QJsonObject unavailable =
                actor.executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{QStringLiteral("runId"), failedRunId},
                                              {QStringLiteral("instanceId"), instanceId},
                                              {QStringLiteral("key"), QStringLiteral("g8_math_result")}});
            QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
            QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
        }
    }

    void testAgentConfigurationCreatesStringAndRejectsEmptyOutputName() {
        clearProject();
        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 create string")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("创建字符串")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));
        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.createstring")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_create_string")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        const auto configure = [&](const QString& outputName) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), instanceId},
                            {QStringLiteral("values"),
                             QJsonObject{{QStringLiteral("stringSource"), QStringLiteral("Fixed")},
                                         {QStringLiteral("fixedString"), QStringLiteral("Lot-42-检测")},
                                         {QStringLiteral("outputVarName"), outputName}}}});
        };
        QCOMPARE(configure(QStringLiteral("g8_lot_code")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("params_set"));
        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        const QJsonObject value = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("g8_lot_code")}});
        QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(value.value(QStringLiteral("value")).toString(), QStringLiteral("Lot-42-检测"));
        const QJsonObject length = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("string_length")}});
        QCOMPARE(length.value(QStringLiteral("value")).toInt(), 9);

        const QJsonObject invalid = configure(QStringLiteral("  "));
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        QVERIFY(invalid.value(QStringLiteral("error")).toString().contains(QStringLiteral("Output variable name")));
    }

    void testAgentConfigurationSplitsStringAndReportsEmptyInput() {
        clearProject();
        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 split string")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("分割字符串")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));
        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.splitstring")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_split_string")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        const auto configure = [&](const QString& inputString) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{
                    {QStringLiteral("instanceId"), instanceId},
                    {QStringLiteral("values"), QJsonObject{{QStringLiteral("inputString"), inputString},
                                                           {QStringLiteral("separator"), QStringLiteral(",")},
                                                           {QStringLiteral("useRegex"), false},
                                                           {QStringLiteral("outputPrefix"), QStringLiteral("g8_part")},
                                                           {QStringLiteral("maxSplits"), 0}}}});
        };
        QCOMPARE(configure(QStringLiteral("A-17,B-29,C-41")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("params_set"));
        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        const QJsonObject parts = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("split_result")}});
        QCOMPARE(parts.value(QStringLiteral("value")).toString(), QStringLiteral("A-17|B-29|C-41"));
        const QJsonObject secondPart = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("g8_part_1")}});
        QCOMPARE(secondPart.value(QStringLiteral("value")).toString(), QStringLiteral("B-29"));

        QCOMPARE(configure(QString()).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject failedRun = runToTerminal(actor);
        QCOMPARE(failedRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failedRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), instanceId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("输入字符串为空"));
    }

    void testAgentConfigurationFormatsStringAndRejectsEmptyOutputName() {
        clearProject();
        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 string format")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("字符串格式化")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));
        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.strformat")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_str_format")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        const QJsonArray inputs{QStringLiteral("Lot-42"), QStringLiteral("3"), QStringLiteral("0.5")};
        const auto configure = [&](const QString& outputName) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), instanceId},
                            {QStringLiteral("values"),
                             QJsonObject{{QStringLiteral("format"), QStringLiteral("lot=%s|qty=%d|ratio=%f")},
                                         {QStringLiteral("inputVariables"), inputs},
                                         {QStringLiteral("outputVariable"), outputName}}}});
        };
        QCOMPARE(configure(QStringLiteral("g8_formatted_lot")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("params_set"));
        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QJsonObject result =
            actor.executeTool(QStringLiteral("get_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("g8_formatted_lot")}});
        QCOMPARE(result.value(QStringLiteral("value")).toString(), QStringLiteral("lot=Lot-42|qty=3|ratio=0.5"));

        const QJsonObject invalid = configure(QString());
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(invalid.value(QStringLiteral("error")).toString(),
                 QStringLiteral("Output variable name cannot be empty"));
    }

    void testAgentConfigurationReadsSystemTimeAndRejectsEmptyFormat() {
        clearProject();
        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 system time")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("SystemTime")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));
        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.systemtime")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_system_time")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        const auto configure = [&](const QString& format) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), instanceId},
                            {QStringLiteral("values"), QJsonObject{{QStringLiteral("timeFormat"), format}}}});
        };
        QCOMPARE(configure(QStringLiteral("yyyyMMdd HHmmss")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("params_set"));
        const qint64 beforeRun = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject successfulRun = runToTerminal(actor);
        const qint64 afterRun = QDateTime::currentMSecsSinceEpoch();
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        const QJsonObject timeText = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("time_string")}});
        const QJsonObject timestamp = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("time_stamp")}});
        QVERIFY(QRegularExpression(QStringLiteral("^\\d{8} \\d{6}$"))
                    .match(timeText.value(QStringLiteral("value")).toString())
                    .hasMatch());
        const qint64 timestampValue = static_cast<qint64>(timestamp.value(QStringLiteral("value")).toDouble());
        QVERIFY(timestampValue >= beforeRun);
        QVERIFY(timestampValue <= afterRun);

        const QJsonObject invalid = configure(QString());
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(invalid.value(QStringLiteral("error")).toString(), QStringLiteral("时间格式不能为空"));
    }

    void testAgentReportsHymsonSdkUnavailableForDefectDetection() {
        clearProject();
        const QString pointCloudPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-defectdetection.ply"));
        QFile pointCloudFile(pointCloudPath);
        QVERIFY(pointCloudFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream stream(&pointCloudFile);
        stream << "ply\nformat ascii 1.0\nelement vertex 4\n"
                  "property float x\nproperty float y\nproperty float z\nend_header\n"
                  "0 0 0\n1 0 0\n1 1 0\n0 1 0\n";
        pointCloudFile.close();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 DefectDetection")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("DefectDetection")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("LoadPointCloud")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.loadpointcloud")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_load_point_cloud")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(actor
                     .executeTool(QStringLiteral("set_module_params"),
                                  QJsonObject{{QStringLiteral("instanceId"), sourceId},
                                              {QStringLiteral("values"),
                                               QJsonObject{{QStringLiteral("filePath"), pointCloudPath}}}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));

        const QJsonObject added = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.defectdetection")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_defect_detection")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        const QJsonObject flow = actor.executeTool(QStringLiteral("get_flow_state"), QJsonObject());
        const QJsonArray modules = flow.value(QStringLiteral("modules")).toArray();
        QVERIFY(!modules.isEmpty());
        QJsonArray outputs;
        for (const QJsonValue& moduleValue : modules) {
            const QJsonObject module = moduleValue.toObject();
            if (module.value(QStringLiteral("id")).toString() == instanceId) {
                outputs = module.value(QStringLiteral("ports")).toObject().value(QStringLiteral("outputs")).toArray();
                break;
            }
        }
        QStringList outputIds;
        for (const QJsonValue& output : outputs)
            outputIds.append(output.toObject().value(QStringLiteral("id")).toString());
        QVERIFY(outputIds.contains(QStringLiteral("defect_point_cloud")));
        QVERIFY(outputIds.contains(QStringLiteral("has_defects")));
        QVERIFY(outputIds.contains(QStringLiteral("defect_count")));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), sourceId},
                                              {QStringLiteral("toId"), instanceId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        const QJsonObject configured =
            actor.executeTool(QStringLiteral("set_module_params"),
                              QJsonObject{{QStringLiteral("instanceId"), instanceId},
                                          {QStringLiteral("values"), QJsonObject{{QStringLiteral("radius"), 0.08}}}});
        QCOMPARE(configured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));

        const QJsonObject failedRun = runToTerminal(actor);
        QCOMPARE(failedRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failedRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), instanceId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(),
                 QStringLiteral("HymsonVision3D SDK 未启用，缺陷检测不可用"));
        const QList<LogEntry> pluginLogs = Logger::instance().logs(QStringLiteral("DefectDetection"));
        QVERIFY(!pluginLogs.isEmpty());
        QVERIFY(std::any_of(pluginLogs.cbegin(), pluginLogs.cend(), [](const LogEntry& entry) {
            return entry.message == QStringLiteral("HymsonVision3D SDK 未启用，缺陷检测不可用");
        }));
    }

    void testAgentConfiguresWriteTextAndPropagatesOpenFailure() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        clearProject();
        AgentActor& actor = *controller.actor();

        const QString outputPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("agent-write-text.txt"));
        const QString content = QStringLiteral("G8 approved Agent output");
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 WriteText")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("WriteText")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));
        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.writetext")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_write_text")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        const QJsonObject configured = actor.executeTool(
            QStringLiteral("set_module_params"),
            QJsonObject{{QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("values"), QJsonObject{{QStringLiteral("filePath"), outputPath},
                                                               {QStringLiteral("textContent"), content},
                                                               {QStringLiteral("appendMode"), false}}}});
        QCOMPARE(configured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));

        const QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(!QFileInfo::exists(outputPath));

        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        const QJsonObject approved = controller.handleToolCall(
            QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(approved.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult success = qvariant_cast<RunResult>(runFinished.takeFirst().at(0));
        QVERIFY2(success.success, qPrintable(success.errorMessage));
        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(output.readAll()), content + QLatin1Char('\n'));
        output.close();

        const QString missingDirectory = QDir(m_appDataDir.path()).filePath(QStringLiteral("missing-parent"));
        const QString missingPath = QDir(missingDirectory).filePath(QStringLiteral("failure.txt"));
        const QJsonObject invalid = actor.executeTool(
            QStringLiteral("set_module_params"),
            QJsonObject{{QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("values"), QJsonObject{{QStringLiteral("filePath"), missingPath}}}});
        QCOMPARE(invalid.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject failedPending = controller.handleToolCall(
            QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(failedPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult failure = qvariant_cast<RunResult>(runFinished.takeFirst().at(0));
        QVERIFY(!failure.success);
        QCOMPARE(failure.failedModule, instanceId);
        QVERIFY(failure.errorMessage.contains(missingPath));
        QVERIFY(!QDir(missingDirectory).exists());
        QVERIFY(!QFileInfo::exists(missingPath));
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        controller.clearConversation();
        clearProject();
    }

    void testAgentRunsTimeSliceAndRejectsStopWithoutStartInput() {
        clearProject();
        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 TimeSlice")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("TimeSlice")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));

        const auto addSlice = [&actor](const QString& name) {
            return actor.executeTool(
                QStringLiteral("add_module"),
                QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.timeslice")},
                            {QStringLiteral("instanceName"), name}});
        };
        const QJsonObject start = addSlice(QStringLiteral("g8_slice_start"));
        const QJsonObject stop = addSlice(QStringLiteral("g8_slice_stop"));
        QCOMPARE(start.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(stop.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString startId = start.value(QStringLiteral("instanceId")).toString();
        const QString stopId = stop.value(QStringLiteral("instanceId")).toString();
        const auto configure = [&actor](const QString& instanceId, const QString& mode) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), instanceId},
                            {QStringLiteral("values"),
                             QJsonObject{{QStringLiteral("mode"), mode},
                                         {QStringLiteral("sliceName"), QStringLiteral("g8_agent_slice")}}}});
        };
        QCOMPARE(configure(startId, QStringLiteral("Start")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(configure(stopId, QStringLiteral("Stop")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), startId},
                                              {QStringLiteral("toId"), stopId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        const QJsonObject startOutput =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), runId},
                                          {QStringLiteral("instanceId"), startId},
                                          {QStringLiteral("key"), QStringLiteral("timeslice_start_time")}});
        QCOMPARE(startOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(startOutput.value(QStringLiteral("value")).toDouble() > 0.0);
        const QJsonObject elapsed =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), runId},
                                          {QStringLiteral("instanceId"), stopId},
                                          {QStringLiteral("key"), QStringLiteral("timeslice_elapsed_ms")}});
        QCOMPARE(elapsed.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(elapsed.value(QStringLiteral("value")).toDouble() >= 0.0);

        clearProject();
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 TimeSlice missing input")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        const QJsonObject stopOnly = addSlice(QStringLiteral("g8_slice_stop_only"));
        QCOMPARE(stopOnly.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString stopOnlyId = stopOnly.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(configure(stopOnlyId, QStringLiteral("Stop")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("params_set"));
        const QJsonObject failedRun = runToTerminal(actor);
        QCOMPARE(failedRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failedRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), stopOnlyId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(),
                 QStringLiteral("Stop 缺少 timeslice_start_time 输入：请确认上游 Start 节点已连接"));
        clearProject();
    }

    void testAgentEvaluatesConditionFromUpstreamAndReportsMissingVariable() {
        clearProject();
        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 Condition")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        const auto ensurePluginLoaded = [&actor](const QString& pluginName) {
            const QString status = actor
                                       .executeTool(QStringLiteral("load_module_plugin"),
                                                    QJsonObject{{QStringLiteral("plugin"), pluginName}})
                                       .value(QStringLiteral("status"))
                                       .toString();
            return status == QLatin1String("loaded") || status == QLatin1String("already_loaded");
        };
        QVERIFY(ensurePluginLoaded(QStringLiteral("创建字符串")));
        QVERIFY(ensurePluginLoaded(QStringLiteral("条件判断")));
        const QJsonObject producer =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.createstring")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_condition_source")}});
        const QJsonObject condition =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.condition")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_condition_check")}});
        QCOMPARE(producer.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(condition.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString producerId = producer.value(QStringLiteral("instanceId")).toString();
        const QString conditionId = condition.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(actor
                     .executeTool(
                         QStringLiteral("set_module_params"),
                         QJsonObject{{QStringLiteral("instanceId"), producerId},
                                     {QStringLiteral("values"), QJsonObject{{QStringLiteral("outputVarName"),
                                                                             QStringLiteral("g8_condition_value")}}}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const auto configureProducer = [&actor, &producerId](const QString& fixedString) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), producerId},
                            {QStringLiteral("values"), QJsonObject{{QStringLiteral("fixedString"), fixedString}}}});
        };
        const auto configureCondition = [&actor, &conditionId](const QString& variableName, const QString& operation,
                                                               const QString& compareValue) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), conditionId},
                            {QStringLiteral("values"), QJsonObject{{QStringLiteral("variableName"), variableName},
                                                                   {QStringLiteral("operator"), operation},
                                                                   {QStringLiteral("compareValue"), compareValue}}}});
        };
        const auto isParameterUpdateAccepted = [](const QString& status) {
            return status == QLatin1String("params_set") || status == QLatin1String("no_op");
        };

        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), producerId},
                                              {QStringLiteral("toId"), conditionId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        struct ComparatorCase {
            const char* operation;
            const char* inputValue;
            const char* compareValue;
            bool expected;
        };
        const ComparatorCase comparatorCases[] = {
            {"NotEmpty", "PASS-42", "", true},     {"Empty", "", "", true},
            {"Equal", "PASS-42", "PASS-42", true}, {"NotEqual", "PASS-42", "NO-MATCH", true},
            {"Contains", "PASS-42", "ASS-", true}, {"GreaterThan", "42", "9", true},
            {"LessThan", "2", "10", true},         {"StartsWith", "PASS-42", "PASS", true},
            {"EndsWith", "PASS-42", "42", true},
        };
        for (const ComparatorCase& comparatorCase : comparatorCases) {
            QVERIFY(isParameterUpdateAccepted(configureProducer(QString::fromLatin1(comparatorCase.inputValue))
                                                  .value(QStringLiteral("status"))
                                                  .toString()));
            QVERIFY(isParameterUpdateAccepted(configureCondition(QStringLiteral("g8_condition_value"),
                                                                 QString::fromLatin1(comparatorCase.operation),
                                                                 QString::fromLatin1(comparatorCase.compareValue))
                                                  .value(QStringLiteral("status"))
                                                  .toString()));
            const QJsonObject run = runToTerminal(actor);
            QCOMPARE(run.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
            const QJsonObject output =
                actor.executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{QStringLiteral("runId"), run.value(QStringLiteral("runId")).toString()},
                                              {QStringLiteral("instanceId"), conditionId},
                                              {QStringLiteral("key"), QStringLiteral("condition_result")}});
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(output.value(QStringLiteral("value")).toBool(), comparatorCase.expected);
        }

        QCOMPARE(
            configureCondition(QStringLiteral("g8_condition_value"), QStringLiteral("Equal"), QStringLiteral("PASS-42"))
                .value(QStringLiteral("status"))
                .toString(),
            QStringLiteral("params_set"));
        const QJsonObject matchedRun = runToTerminal(actor);
        QCOMPARE(matchedRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString matchedRunId = matchedRun.value(QStringLiteral("runId")).toString();
        const QJsonObject matchedOutput =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), matchedRunId},
                                          {QStringLiteral("instanceId"), conditionId},
                                          {QStringLiteral("key"), QStringLiteral("condition_result")}});
        QCOMPARE(matchedOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(matchedOutput.value(QStringLiteral("value")).toBool());

        QCOMPARE(configureCondition(QStringLiteral("g8_condition_value"), QStringLiteral("Equal"),
                                    QStringLiteral("NO-MATCH"))
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject mismatchedRun = runToTerminal(actor);
        QCOMPARE(mismatchedRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QJsonObject mismatchedOutput = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), mismatchedRun.value(QStringLiteral("runId")).toString()},
                        {QStringLiteral("instanceId"), conditionId},
                        {QStringLiteral("key"), QStringLiteral("condition_result")}});
        QCOMPARE(mismatchedOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(!mismatchedOutput.value(QStringLiteral("value")).toBool());

        QCOMPARE(configureCondition(QString(), QStringLiteral("Equal"), QStringLiteral("PASS-42"))
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject invalidRun = runToTerminal(actor);
        QCOMPARE(invalidRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = invalidRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), conditionId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("变量名不能为空"));
        clearProject();
    }

    void testAgentRunsConfiguredDelayAndRejectsZeroDuration() {
        clearProject();
        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 Delay")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        const auto ensurePluginLoaded = [&actor](const QString& pluginName) {
            const QString status = actor
                                       .executeTool(QStringLiteral("load_module_plugin"),
                                                    QJsonObject{{QStringLiteral("plugin"), pluginName}})
                                       .value(QStringLiteral("status"))
                                       .toString();
            return status == QLatin1String("loaded") || status == QLatin1String("already_loaded");
        };
        QVERIFY(ensurePluginLoaded(QStringLiteral("TimeSlice")));
        QVERIFY(ensurePluginLoaded(QStringLiteral("延时")));

        const auto addModule = [&actor](const QString& pluginId, const QString& instanceName) {
            return actor.executeTool(
                QStringLiteral("add_module"),
                QJsonObject{{QStringLiteral("plugin"), pluginId}, {QStringLiteral("instanceName"), instanceName}});
        };
        const QJsonObject start =
            addModule(QStringLiteral("com.deeplux.plugin.timeslice"), QStringLiteral("g8_delay_start"));
        const QJsonObject delay = addModule(QStringLiteral("com.deeplux.plugin.delay"), QStringLiteral("g8_delay"));
        const QJsonObject stop =
            addModule(QStringLiteral("com.deeplux.plugin.timeslice"), QStringLiteral("g8_delay_stop"));
        QCOMPARE(start.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(delay.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(stop.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString startId = start.value(QStringLiteral("instanceId")).toString();
        const QString delayId = delay.value(QStringLiteral("instanceId")).toString();
        const QString stopId = stop.value(QStringLiteral("instanceId")).toString();
        const auto setParams = [&actor](const QString& instanceId, const QJsonObject& values) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), instanceId}, {QStringLiteral("values"), values}});
        };
        QCOMPARE(setParams(startId, QJsonObject{{QStringLiteral("mode"), QStringLiteral("Start")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(
            setParams(delayId, QJsonObject{{QStringLiteral("delayMs"), 40}}).value(QStringLiteral("status")).toString(),
            QStringLiteral("params_set"));
        QCOMPARE(setParams(stopId, QJsonObject{{QStringLiteral("mode"), QStringLiteral("Stop")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        for (const auto& edge : {qMakePair(startId, delayId), qMakePair(delayId, stopId)}) {
            const QJsonObject connected = actor.executeTool(
                QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), edge.first},
                                                               {QStringLiteral("toId"), edge.second},
                                                               {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                               {QStringLiteral("toPort"), QStringLiteral("image")}});
            QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
        }

        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QJsonObject elapsed = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), successfulRun.value(QStringLiteral("runId")).toString()},
                        {QStringLiteral("instanceId"), stopId},
                        {QStringLiteral("key"), QStringLiteral("timeslice_elapsed_ms")}});
        QCOMPARE(elapsed.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(elapsed.value(QStringLiteral("value")).toDouble() >= 40.0);
        QVERIFY(elapsed.value(QStringLiteral("value")).toDouble() < 5000.0);

        const QJsonObject invalid = setParams(delayId, QJsonObject{{QStringLiteral("delayMs"), 0}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(invalid.value(QStringLiteral("error")).toString(),
                 QStringLiteral("Delay time must be greater than 0"));
        clearProject();
    }

    void testAgentChecksUpstreamValueAndReportsMissingCheckData() {
        clearProject();
        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 DataCheck")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        const auto ensurePluginLoaded = [&actor](const QString& pluginName) {
            const QString status = actor
                                       .executeTool(QStringLiteral("load_module_plugin"),
                                                    QJsonObject{{QStringLiteral("plugin"), pluginName}})
                                       .value(QStringLiteral("status"))
                                       .toString();
            return status == QLatin1String("loaded") || status == QLatin1String("already_loaded");
        };
        QVERIFY(ensurePluginLoaded(QStringLiteral("创建字符串")));
        QVERIFY(ensurePluginLoaded(QStringLiteral("DataCheck")));

        const QJsonObject producer =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.createstring")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_check_data_source")}});
        const QJsonObject checker =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.datacheck")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_data_check")}});
        QCOMPARE(producer.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(checker.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString producerId = producer.value(QStringLiteral("instanceId")).toString();
        const QString checkerId = checker.value(QStringLiteral("instanceId")).toString();
        const auto setParams = [&actor](const QString& instanceId, const QJsonObject& values) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), instanceId}, {QStringLiteral("values"), values}});
        };
        QCOMPARE(setParams(producerId, QJsonObject{{QStringLiteral("fixedString"), QStringLiteral("42")},
                                                   {QStringLiteral("outputVarName"), QStringLiteral("check_data")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(checkerId, QJsonObject{{QStringLiteral("checkType"), QStringLiteral("Range")},
                                                  {QStringLiteral("minValue"), 40.0},
                                                  {QStringLiteral("maxValue"), 45.0}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), producerId},
                                              {QStringLiteral("toId"), checkerId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        const auto readCheckOutput = [&actor, &checkerId](const QString& runId, const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), checkerId},
                                                 {QStringLiteral("key"), key}});
        };
        const QJsonObject passingRun = runToTerminal(actor);
        QCOMPARE(passingRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString passingRunId = passingRun.value(QStringLiteral("runId")).toString();
        QCOMPARE(readCheckOutput(passingRunId, QStringLiteral("check_passed")).value(QStringLiteral("value")).toBool(),
                 true);

        QCOMPARE(setParams(producerId, QJsonObject{{QStringLiteral("fixedString"), QStringLiteral("99")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject rejectedValueRun = runToTerminal(actor);
        QCOMPARE(rejectedValueRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString rejectedValueRunId = rejectedValueRun.value(QStringLiteral("runId")).toString();
        QCOMPARE(
            readCheckOutput(rejectedValueRunId, QStringLiteral("check_passed")).value(QStringLiteral("value")).toBool(),
            false);
        QCOMPARE(readCheckOutput(rejectedValueRunId, QStringLiteral("check_error"))
                     .value(QStringLiteral("value"))
                     .toString(),
                 QStringLiteral("值 99 超出范围 [40, 45]"));

        QCOMPARE(setParams(producerId, QJsonObject{{QStringLiteral("outputVarName"), QStringLiteral("not_check_data")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject missingInputRun = runToTerminal(actor);
        QCOMPARE(missingInputRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = missingInputRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), checkerId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("未提供校验数据"));
        clearProject();
    }

    void testAgentConfiguresTableOutputAndRejectsInvalidDimensions() {
        clearProject();
        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 Table output")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("TableOutPut")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));
        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.tableoutput")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_table_output")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        const auto setParams = [&actor, &instanceId](const QJsonObject& values) {
            return actor.executeTool(
                QStringLiteral("set_module_params"),
                QJsonObject{{QStringLiteral("instanceId"), instanceId}, {QStringLiteral("values"), values}});
        };
        QCOMPARE(setParams(QJsonObject{{QStringLiteral("rowCount"), 2}, {QStringLiteral("colCount"), 3}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        const auto output = [&actor, &instanceId, &runId](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), instanceId},
                                                 {QStringLiteral("key"), key}});
        };
        QCOMPARE(output(QStringLiteral("table_rows")).value(QStringLiteral("value")).toInt(), 2);
        QCOMPARE(output(QStringLiteral("table_cols")).value(QStringLiteral("value")).toInt(), 3);
        QCOMPARE(output(QStringLiteral("table_output")).value(QStringLiteral("value")).toString(),
                 QStringLiteral("\t\t\n\t\t\n"));

        const QJsonObject invalid = setParams(QJsonObject{{QStringLiteral("rowCount"), 0}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(invalid.value(QStringLiteral("error")).toString(), QStringLiteral("行数必须在1到100之间"));
        clearProject();
    }

    void testAgentConfiguresSaveDataAndReportsWriteFailure() {
        clearProject();
        AgentActor actor;
        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 SaveData")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("load_module_plugin"),
                                  QJsonObject{{QStringLiteral("plugin"), QStringLiteral("SaveData")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("loaded"));
        const QString producerLoadStatus =
            actor
                .executeTool(QStringLiteral("load_module_plugin"),
                             QJsonObject{{QStringLiteral("plugin"), QStringLiteral("创建字符串")}})
                .value(QStringLiteral("status"))
                .toString();
        QVERIFY(producerLoadStatus == QLatin1String("loaded") || producerLoadStatus == QLatin1String("already_loaded"));
        const QJsonObject producer =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.createstring")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_save_data_source")}});
        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.savedata")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_save_data")}});
        QCOMPARE(producer.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString producerId = producer.value(QStringLiteral("instanceId")).toString();
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(actor
                     .executeTool(
                         QStringLiteral("set_module_params"),
                         QJsonObject{{QStringLiteral("instanceId"), producerId},
                                     {QStringLiteral("values"),
                                      QJsonObject{{QStringLiteral("fixedString"), QStringLiteral("payload-42")},
                                                  {QStringLiteral("outputVarName"), QStringLiteral("g8_payload")}}}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), producerId},
                                              {QStringLiteral("toId"), instanceId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));
        const QString outputPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-output.json"));
        const auto configure = [&](const QString& path) {
            return actor.executeTool(QStringLiteral("set_module_params"),
                                     QJsonObject{{QStringLiteral("instanceId"), instanceId},
                                                 {QStringLiteral("values"),
                                                  QJsonObject{{QStringLiteral("filePath"), path},
                                                              {QStringLiteral("fileFormat"), QStringLiteral("json")},
                                                              {QStringLiteral("appendMode"), false}}}});
        };
        QCOMPARE(configure(outputPath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));

        QSignalSpy runFinished(&RunEngine::instance(), &RunEngine::runFinished);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                                        QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult savedResult = qvariant_cast<RunResult>(runFinished.takeFirst().at(0));
        QVERIFY2(savedResult.success, qPrintable(savedResult.errorMessage));
        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        const QJsonDocument saved = QJsonDocument::fromJson(output.readAll(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(saved.isObject());
        QCOMPARE(saved.object().value(QStringLiteral("g8_payload")).toString(), QStringLiteral("payload-42"));
        QCOMPARE(saved.object().value(QStringLiteral("string_length")).toInt(), 10);

        QVERIFY(QFile::remove(outputPath));
        QVERIFY(QDir().mkpath(outputPath));
        pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(QFileInfo(outputPath).isDir());
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult failedResult = qvariant_cast<RunResult>(runFinished.takeFirst().at(0));
        QVERIFY(!failedResult.success);
        QCOMPARE(failedResult.failedModule, instanceId);
        QVERIFY(failedResult.errorMessage.contains(QStringLiteral("保存数据失败")));
        QVERIFY(QFileInfo(outputPath).isDir());

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        clearProject();
    }
};

QTEST_MAIN(TestAgentCorePluginsG8)
#include "test_agent_core_plugins_g8.moc"
