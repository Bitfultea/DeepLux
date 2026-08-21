#include <QtTest/QtTest>
#include "core/agent/AgentObserver.h"
#include "core/agent/GuiEvent.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"
#include "ui/widgets/FlowCanvas.h"

using namespace DeepLux;

class TestAgentObserver : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        ProjectManager::instance().closeProject();
    }

    void cleanup()
    {
        ProjectManager::instance().closeProject();
    }

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

    void testCanvasRemovalProducesOneExactProjectEvent()
    {
        AgentObserver observer;
        QVERIFY(observer.initialize());

        FlowCanvas canvas;
        observer.setFlowCanvas(&canvas);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        const QString source = canvas.addNode("module.source", "Source", QPointF(0, 0));
        const QString target = canvas.addNode("module.target", "Target", QPointF(0, 120));
        canvas.addConnection(source, "point", target, "point");
        canvas.removeConnection(source, "point", target, "point");

        int removalCount = 0;
        GuiEvent removal;
        for (const GuiEvent& event : observer.allEvents()) {
            if (event.type == GuiEventType::ConnectionRemoved) {
                ++removalCount;
                removal = event;
            }
        }

        QCOMPARE(removalCount, 1);
        QCOMPARE(removal.source, QString("Project"));
        QCOMPARE(removal.details.value("from").toString(), source);
        QCOMPARE(removal.details.value("fromPort").toString(), QString("point"));
        QCOMPARE(removal.details.value("to").toString(), target);
        QCOMPARE(removal.details.value("toPort").toString(), QString("point"));
        observer.shutdown();
    }
};

QTEST_MAIN(TestAgentObserver)
#include "test_agentobserver.moc"
