#include <QtTest/QtTest>

#define private public
#include "core/agent/AgentBridge.h"
#undef private

#include "core/agent/AgentConnection.h"
#include "core/agent/AgentController.h"
#include "core/agent/ToolSchema.h"
#include "manager/ProjectManager.h"

#include <QLocalSocket>
#include <QTemporaryDir>

using namespace DeepLux;

class TestAgentBridge : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_hadAppDataDir = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
        m_previousAppDataDir = qgetenv("DEEPLUX_APP_DATA_DIR");
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        ToolSchema::instance().registerDefaultTools();
    }

    void init() {
        AgentBridge::instance().stop();
        AgentBridge::instance().setToolCallCallback(nullptr);
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void cleanup() {
        AgentBridge::instance().stop();
        AgentBridge::instance().setToolCallCallback(nullptr);
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void cleanupTestCase() {
        if (m_hadAppDataDir) {
            qputenv("DEEPLUX_APP_DATA_DIR", m_previousAppDataDir);
        } else {
            qunsetenv("DEEPLUX_APP_DATA_DIR");
        }
    }

    void testExecuteIsRejected() {
        auto response = process({
            {"version", "1.0"},
            {"type", "execute"},
            {"id", "exec-1"},
            {"payload", QJsonObject{{"command", "ls"}}},
        });
        QVERIFY(response.error);
        QCOMPARE(response.reqId, QString("exec-1"));
        QVERIFY(response.errorMessage.contains("deprecated"));
    }

    void testQueryPingAndSubscribe() {
        auto query = process({
            {"version", "1.0"},
            {"type", "query"},
            {"id", "query-1"},
            {"payload", QJsonObject{{"target", "system"}}},
        });
        QVERIFY(!query.error);
        QVERIFY(query.payload.contains("platform"));

        auto ping = process({
            {"version", "1.0"},
            {"type", "ping"},
            {"id", "ping-1"},
        });
        QVERIFY(!ping.error);
        QCOMPARE(ping.payload["type"].toString(), QString("pong"));

        auto subscribed = process({
            {"version", "1.0"},
            {"type", "subscribe"},
            {"id", "sub-1"},
            {"payload", QJsonObject{{"event", "run_finished"}}},
        });
        QVERIFY(!subscribed.error);
        QCOMPARE(subscribed.payload["status"].toString(), QString("subscribed"));
    }

    void testToolCallUsesCallback() {
        AgentBridge::instance().setToolCallCallback([](const QString& tool, const QJsonObject& params) {
            return QJsonObject{{"tool", tool}, {"plugin", params["plugin"].toString()}};
        });

        auto response = process({
            {"version", "1.0"},
            {"type", "tool_call"},
            {"id", "tool-1"},
            {"payload", QJsonObject{{"tool", "add_module"}, {"params", QJsonObject{{"plugin", "GrabImage"}}}}},
        });
        QVERIFY(!response.error);
        QCOMPARE(response.payload["tool"].toString(), QString("add_module"));
        QCOMPARE(response.payload["plugin"].toString(), QString("GrabImage"));
    }

    void testToolCallCanUseControllerPermissions() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentBridge::instance().setToolCallCallback([](const QString& tool, const QJsonObject& params) {
            return AgentController::instance().handleToolCall(tool, params);
        });

        auto response = process({
            {"version", "1.0"},
            {"type", "tool_call"},
            {"id", "tool-2"},
            {"payload", QJsonObject{{"tool", "add_module"}, {"params", QJsonObject{{"plugin", "GrabImage"}}}}},
        });
        QVERIFY(!response.error);
        QCOMPARE(response.payload["status"].toString(), QString("pending_confirmation"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
    }

    void testResponseWriteDoesNotHoldConnectionMutex() {
        AgentBridge& bridge = AgentBridge::instance();
        QVERIFY(bridge.start());

        QLocalSocket client;
        client.connectToServer(bridge.serverName());
        QVERIFY(client.waitForConnected(1000));
        QTRY_COMPARE_WITH_TIMEOUT(bridge.m_connections.size(), 1, 1000);

        AgentConnection* connection = bridge.m_connections.first();
        const QString clientId = connection->clientId();
        QObject::disconnect(connection, &AgentConnection::disconnected, &bridge, &AgentBridge::onClientDisconnected);

        bool disconnected = false;
        bool mutexWasAvailable = false;
        connect(connection, &AgentConnection::disconnected, this, [&]() {
            disconnected = true;
            mutexWasAvailable = bridge.m_connectionMutex.tryLock();
            if (mutexWasAvailable) {
                bridge.m_connectionMutex.unlock();
            }
        });

        client.abort();
        bridge.sendResponse(clientId, QStringLiteral("closed-client"), QJsonObject{{"ok", true}});

        QVERIFY(disconnected);
        QVERIFY2(mutexWasAvailable, "AgentBridge must not hold m_connectionMutex while writing to a socket");
    }

private:
    AgentBridge::ProtocolResponse process(const QJsonObject& request) const {
        return AgentBridge::instance().processMessage("test-client", request);
    }

    QTemporaryDir m_appDataDir;
    QByteArray m_previousAppDataDir;
    bool m_hadAppDataDir = false;
};

QTEST_MAIN(TestAgentBridge)
#include "test_agentbridge.moc"
