#include <QtTest/QtTest>
#include "core/agent/AgentObserver.h"
#include "core/agent/GuiEvent.h"

using namespace DeepLux;

class TestAgentObserver : public QObject
{
    Q_OBJECT

private slots:
    void testRecentEventsEmpty()
    {
        AgentObserver observer;
        QList<GuiEvent> events = observer.recentEvents(10);
        QCOMPARE(events.size(), 0);
    }

    void testEventSerialization()
    {
        GuiEvent e(GuiEventType::ProjectCreated, "ProjectManager",
                   QJsonObject{{"name", "Test"}});
        QCOMPARE(e.typeString(), QString("project_created"));
        QVERIFY(e.timestamp.isValid());

        QJsonObject json = e.toJson();
        QCOMPARE(json["type"].toString(), QString("project_created"));
        QCOMPARE(json["source"].toString(), QString("ProjectManager"));
    }

    void testEventTypeStrings()
    {
        QCOMPARE(GuiEvent(GuiEventType::RunStarted, "").typeString(), QString("run_started"));
        QCOMPARE(GuiEvent(GuiEventType::ModuleAdded, "").typeString(), QString("module_added"));
        QCOMPARE(GuiEvent(GuiEventType::PropertyChanged, "").typeString(), QString("property_changed"));
        QCOMPARE(GuiEvent(GuiEventType::Unknown, "").typeString(), QString("unknown"));
    }
};

QTEST_MAIN(TestAgentObserver)
#include "test_agentobserver.moc"
