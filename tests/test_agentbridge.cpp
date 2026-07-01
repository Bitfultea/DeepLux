#include <QtTest/QtTest>

#define private public
#include "core/agent/AgentBridge.h"
#undef private

#include "core/agent/AgentController.h"
#include "core/agent/ToolSchema.h"
#include "manager/ProjectManager.h"

using namespace DeepLux;

class TestAgentBridge : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        ToolSchema::instance().registerDefaultTools();
    }

    void init()
    {
        AgentBridge::instance().stop();
        AgentBridge::instance().setToolCallCallback(nullptr);
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void cleanup()
    {
        AgentBridge::instance().stop();
        AgentBridge::instance().setToolCallCallback(nullptr);
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void testExecuteIsRejected()
    {
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

    void testQueryPingAndSubscribe()
    {
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

    void testToolCallUsesCallback()
    {
        AgentBridge::instance().setToolCallCallback(
            [](const QString& tool, const QJsonObject& params) {
                return QJsonObject{{"tool", tool}, {"plugin", params["plugin"].toString()}};
            });

        auto response = process({
            {"version", "1.0"},
            {"type", "tool_call"},
            {"id", "tool-1"},
            {"payload", QJsonObject{{"tool", "add_module"},
                                     {"params", QJsonObject{{"plugin", "GrabImage"}}}}},
        });
        QVERIFY(!response.error);
        QCOMPARE(response.payload["tool"].toString(), QString("add_module"));
        QCOMPARE(response.payload["plugin"].toString(), QString("GrabImage"));
    }

    void testToolCallCanUseControllerPermissions()
    {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentBridge::instance().setToolCallCallback(
            [](const QString& tool, const QJsonObject& params) {
                return AgentController::instance().handleToolCall(tool, params);
            });

        auto response = process({
            {"version", "1.0"},
            {"type", "tool_call"},
            {"id", "tool-2"},
            {"payload", QJsonObject{{"tool", "add_module"},
                                     {"params", QJsonObject{{"plugin", "GrabImage"}}}}},
        });
        QVERIFY(!response.error);
        QCOMPARE(response.payload["status"].toString(), QString("pending_confirmation"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
    }

private:
    AgentBridge::ProtocolResponse process(const QJsonObject& request) const
    {
        return AgentBridge::instance().processMessage("test-client", request);
    }
};

QTEST_MAIN(TestAgentBridge)
#include "test_agentbridge.moc"
