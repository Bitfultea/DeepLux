#include <QtTest/QtTest>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>
#include <ui/widgets/FlowCanvas.h>

using namespace DeepLux;

class TestFlowCanvas : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void testAddNodeAndConnectionSyncsToCurrentProject();
    void testLoadFromProjectRebuildsStableNodesAndConnections();
};

void TestFlowCanvas::init() {
    ProjectManager::instance().newProject();
}

void TestFlowCanvas::cleanup() {
    ProjectManager::instance().closeProject();
}

void TestFlowCanvas::testAddNodeAndConnectionSyncsToCurrentProject() {
    FlowCanvas canvas;
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project != nullptr);

    const QString firstId = canvas.addNode("module.a", "Module A", QPointF(12, 34));
    const QString secondId = canvas.addNode("module.b", "Module B", QPointF(56, 78));

    QCOMPARE(project->modules().size(), 2);
    QVERIFY(project->findModule(firstId) != nullptr);
    QVERIFY(project->findModule(secondId) != nullptr);
    QCOMPARE(project->findModule(firstId)->moduleId, QString("module.a"));
    QCOMPARE(project->findModule(firstId)->posX, 12);
    QCOMPARE(project->findModule(firstId)->posY, 34);

    canvas.addConnection(firstId, 0, secondId, 0);
    QCOMPARE(project->connections().size(), 1);
    QCOMPARE(project->connections().first().fromModuleId, firstId);
    QCOMPARE(project->connections().first().toModuleId, secondId);

    canvas.removeConnection(firstId, secondId);
    QCOMPARE(project->connections().size(), 0);
}

void TestFlowCanvas::testLoadFromProjectRebuildsStableNodesAndConnections() {
    Project project;

    ModuleInstance first;
    first.id = "inst_a";
    first.moduleId = "module.a";
    first.name = "Module A";
    first.posX = 10;
    first.posY = 20;
    project.addModule(first);

    ModuleInstance second;
    second.id = "inst_b";
    second.moduleId = "module.b";
    second.name = "Module B";
    second.posX = 30;
    second.posY = 40;
    project.addModule(second);

    ModuleConnection conn;
    conn.fromModuleId = first.id;
    conn.toModuleId = second.id;
    project.addConnection(conn);

    FlowCanvas canvas;
    canvas.loadFromProject(&project);

    QCOMPARE(canvas.nodeIds().size(), 2);
    QVERIFY(canvas.nodeIds().contains("inst_a"));
    QVERIFY(canvas.nodeIds().contains("inst_b"));
    QCOMPARE(canvas.nodeItem("inst_a")->pos(), QPointF(10, 20));
    QCOMPARE(canvas.m_connections.size(), 1);
}

QTEST_MAIN(TestFlowCanvas)
#include "test_flowcanvas.moc"
