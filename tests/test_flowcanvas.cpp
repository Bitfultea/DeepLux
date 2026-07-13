#include <QImage>
#include <QPainter>
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
    void testConnectionHasPaintableBoundsAfterAdd();
    void testNodeBoundsIncludePorts();
    void testImplicitSequentialRelationIsPainted();
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

void TestFlowCanvas::testConnectionHasPaintableBoundsAfterAdd() {
    FlowCanvas canvas;

    const QString firstId = canvas.addNode("module.a", "Module A", QPointF(0, 0));
    const QString secondId = canvas.addNode("module.b", "Module B", QPointF(260, 0));

    canvas.addConnection(firstId, 0, secondId, 0);

    QCOMPARE(canvas.m_connections.size(), 1);
    const QRectF bounds = canvas.m_connections.first()->boundingRect();
    QVERIFY2(bounds.width() > 0.0, "Connection should have horizontal paint bounds immediately after add");
    QVERIFY2(bounds.height() > 0.0, "Connection should include pen width so horizontal links can repaint cleanly");
}

void TestFlowCanvas::testNodeBoundsIncludePorts() {
    FlowCanvas canvas;
    const QString nodeId = canvas.addNode("module.a", "Module A", QPointF(0, 0));

    FlowNodeItem* node = canvas.nodeItem(nodeId);
    QVERIFY(node != nullptr);
    const QRectF bounds = node->boundingRect();

    QVERIFY2(bounds.left() <= -5.0, "Node bounds should include the input port painted outside the body");
    QVERIFY2(bounds.right() >= 155.0, "Node bounds should include the output port painted outside the body");
}

void TestFlowCanvas::testImplicitSequentialRelationIsPainted() {
    FlowCanvas canvas;
    canvas.resize(420, 160);
    canvas.centerOn(120, 40);

    canvas.addNode("module.a", "Module A", QPointF(0, 0));
    canvas.addNode("module.b", "Module B", QPointF(260, 0));
    QCOMPARE(canvas.m_connections.size(), 0);

    QImage image(canvas.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    canvas.render(&painter);
    painter.end();

    const QPoint mid = canvas.mapFromScene(QPointF(205, 40));
    bool foundLinePixel = false;
    for (int y = mid.y() - 2; y <= mid.y() + 2 && !foundLinePixel; ++y) {
        for (int x = mid.x() - 2; x <= mid.x() + 2; ++x) {
            if (!image.rect().contains(x, y)) {
                continue;
            }
            const QColor c = image.pixelColor(x, y);
            if (c.red() > 120 && c.green() > 100 && c.blue() < 120) {
                foundLinePixel = true;
                break;
            }
        }
    }
    QVERIFY2(foundLinePixel,
             "FlowCanvas should paint an implicit sequence relation when no explicit connection exists");
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
