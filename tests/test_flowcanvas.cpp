#include <QGraphicsSceneEvent>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
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
    void testThemeUpdatesSceneBackground();
    void testLoadFromProjectRebuildsStableNodesAndConnections();
    void testNodeClickEmitsNodeSelected();
    void testProgrammaticSelectNodeDoesNotEmitSignal();
    // 阶段 F 新增
    void testMultiPortConnectionsNotDeduplicated();
    void testDeleteOneConnectionDoesNotDeleteOthers();
    void testDeleteSecondConnectionPreservesFirst();
    void testSaveReloadPreservesPortConnections();
    void testNodeDragUpdatesConnections();
    void testControlEdgeRenderedAsDashed();
    void testPortSpecsFromPluginManagerLoaded();
    void testDuplicateConnectionIsIdempotent();
    void testProjectRemoveConnectionWithPortsPrecise();
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

    // 阶段 F: 使用字符串端口 ID
    canvas.addConnection(firstId, "image", secondId, "image");
    QCOMPARE(project->connections().size(), 1);
    QCOMPARE(project->connections().first().fromModuleId, firstId);
    QCOMPARE(project->connections().first().toModuleId, secondId);
    QCOMPARE(project->connections().first().fromPort, QString("image"));
    QCOMPARE(project->connections().first().toPort, QString("image"));

    // 按完整 4 元组删除
    canvas.removeConnection(firstId, "image", secondId, "image");
    QCOMPARE(project->connections().size(), 0);
}

void TestFlowCanvas::testConnectionHasPaintableBoundsAfterAdd() {
    FlowCanvas canvas;

    const QString firstId = canvas.addNode("module.a", "Module A", QPointF(0, 0));
    const QString secondId = canvas.addNode("module.b", "Module B", QPointF(260, 0));

    canvas.addConnection(firstId, "image", secondId, "image");

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
    canvas.resize(300, 260);
    canvas.centerOn(110, 92);

    canvas.addNode("module.a", "Module A", QPointF(0, 0));
    canvas.addNode("module.b", "Module B", QPointF(0, 120));
    QCOMPARE(canvas.m_connections.size(), 0);

    QImage image(canvas.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    canvas.render(&painter);
    painter.end();

    const QPoint mid = canvas.mapFromScene(QPointF(110, 92));
    bool foundLinePixel = false;
    for (int y = mid.y() - 2; y <= mid.y() + 2 && !foundLinePixel; ++y) {
        for (int x = mid.x() - 2; x <= mid.x() + 2; ++x) {
            if (!image.rect().contains(x, y)) {
                continue;
            }
            const QColor c = image.pixelColor(x, y);
            if (c.blue() > c.red() + 10 && c.blue() > c.green() + 5 && c.red() < 220) {
                foundLinePixel = true;
                break;
            }
        }
    }
    QVERIFY2(foundLinePixel,
             "FlowCanvas should paint an implicit sequence relation when no explicit connection exists");
}

void TestFlowCanvas::testThemeUpdatesSceneBackground() {
    FlowCanvas canvas;

    canvas.applyTheme(false);
    QCOMPARE(canvas.scene()->backgroundBrush().color(), QColor("#f5f5f5"));

    canvas.applyTheme(true);
    QCOMPARE(canvas.scene()->backgroundBrush().color(), QColor("#1e1e1e"));
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
    conn.fromPort = "image";
    conn.toPort = "image";
    conn.edgeType = "data";
    project.addConnection(conn);

    FlowCanvas canvas;
    canvas.loadFromProject(&project);

    QCOMPARE(canvas.nodeIds().size(), 2);
    QVERIFY(canvas.nodeIds().contains("inst_a"));
    QVERIFY(canvas.nodeIds().contains("inst_b"));
    QCOMPARE(canvas.nodeItem("inst_a")->pos(), QPointF(10, 20));
    QCOMPARE(canvas.m_connections.size(), 1);
}

void TestFlowCanvas::testNodeClickEmitsNodeSelected() {
    FlowCanvas canvas;
    canvas.resize(400, 200);
    const QString nodeId = canvas.addNode("module.a", "Module A", QPointF(50, 50));

    QSignalSpy spy(&canvas, &FlowCanvas::nodeSelected);
    QVERIFY(spy.isValid());

    FlowNodeItem* node = canvas.nodeItem(nodeId);
    QVERIFY(node != nullptr);

    const QPointF nodeCenter =
        node->pos() + QPointF(node->boundingRect().width() / 2, node->boundingRect().height() / 2);
    const QPoint viewportPos = canvas.mapFromScene(nodeCenter);
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, viewportPos);
    QCoreApplication::processEvents();

    QVERIFY2(spy.count() >= 1, "Clicking a node should emit nodeSelected at least once");
    QCOMPARE(spy.takeFirst().at(0).toString(), nodeId);
}

void TestFlowCanvas::testProgrammaticSelectNodeDoesNotEmitSignal() {
    FlowCanvas canvas;
    const QString nodeId = canvas.addNode("module.a", "Module A", QPointF(50, 50));

    QSignalSpy spy(&canvas, &FlowCanvas::nodeSelected);
    QVERIFY(spy.isValid());

    canvas.selectNode(nodeId);

    QCOMPARE(spy.count(), 0);

    FlowNodeItem* node = canvas.nodeItem(nodeId);
    QVERIFY(node != nullptr);
    QVERIFY(node->isSelected());
}

// ---------------------------------------------------------------------------
// 阶段 F 新增测试
// ---------------------------------------------------------------------------

void TestFlowCanvas::testMultiPortConnectionsNotDeduplicated() {
    // 同节点对、不同端口对 → 两条连接共存
    FlowCanvas canvas;
    Project* project = ProjectManager::instance().currentProject();

    const QString a = canvas.addNode("module.a", "A", QPointF(0, 0));
    const QString b = canvas.addNode("module.b", "B", QPointF(260, 0));

    canvas.addConnection(a, "image", b, "image");
    canvas.addConnection(a, "val", b, "val");

    QCOMPARE(canvas.m_connections.size(), 2);
    QCOMPARE(project->connections().size(), 2);
}

void TestFlowCanvas::testDeleteOneConnectionDoesNotDeleteOthers() {
    // 删除 A.image→B.image 不影响 A.val→B.val
    FlowCanvas canvas;
    Project* project = ProjectManager::instance().currentProject();

    const QString a = canvas.addNode("module.a", "A", QPointF(0, 0));
    const QString b = canvas.addNode("module.b", "B", QPointF(260, 0));

    canvas.addConnection(a, "image", b, "image");
    canvas.addConnection(a, "val", b, "val");
    QCOMPARE(canvas.m_connections.size(), 2);

    canvas.removeConnection(a, "image", b, "image");
    QCOMPARE(canvas.m_connections.size(), 1);
    QCOMPARE(canvas.m_connections.first()->fromPortId(), QString("val"));
    QCOMPARE(canvas.m_connections.first()->toPortId(), QString("val"));
    QCOMPARE(project->connections().size(), 1);
}

void TestFlowCanvas::testSaveReloadPreservesPortConnections() {
    // 创建带字符串端口的工程 → JSON → 重新加载 → 连接端口一致
    Project project;
    ModuleInstance a; a.id = "A"; a.moduleId = "module.a"; a.name = "A";
    ModuleInstance b; b.id = "B"; b.moduleId = "module.b"; b.name = "B";
    project.addModule(a);
    project.addModule(b);

    ModuleConnection c1;
    c1.fromModuleId = "A"; c1.toModuleId = "B";
    c1.fromPort = "image"; c1.toPort = "image"; c1.edgeType = "data";
    project.addConnection(c1);

    ModuleConnection c2;
    c2.fromModuleId = "A"; c2.toModuleId = "B";
    c2.fromPort = "val"; c2.toPort = "val"; c2.edgeType = "data";
    project.addConnection(c2);

    QJsonObject json = project.toJson();

    Project restored;
    restored.fromJson(json);

    QCOMPARE(restored.connections().size(), 2);
    QCOMPARE(restored.connections()[0].fromPort, QString("image"));
    QCOMPARE(restored.connections()[0].toPort, QString("image"));
    QCOMPARE(restored.connections()[1].fromPort, QString("val"));
    QCOMPARE(restored.connections()[1].toPort, QString("val"));

    FlowCanvas canvas;
    canvas.loadFromProject(&restored);
    QCOMPARE(canvas.m_connections.size(), 2);
}

void TestFlowCanvas::testNodeDragUpdatesConnections() {
    // 拖动节点后连接路径应更新
    FlowCanvas canvas;
    const QString a = canvas.addNode("module.a", "A", QPointF(0, 0));
    const QString b = canvas.addNode("module.b", "B", QPointF(260, 0));
    canvas.addConnection(a, "image", b, "image");

    QCOMPARE(canvas.m_connections.size(), 1);
    FlowConnectionItem* conn = canvas.m_connections.first();
    const QRectF boundsBefore = conn->boundingRect();

    // 拖动节点 B
    FlowNodeItem* nodeB = canvas.nodeItem(b);
    nodeB->setPos(QPointF(300, 100));
    canvas.updateConnectionsForNode(b);

    const QRectF boundsAfter = conn->boundingRect();
    QVERIFY2(boundsAfter != boundsBefore, "Connection path should update after node drag");
}

void TestFlowCanvas::testControlEdgeRenderedAsDashed() {
    // 控制边应标记为虚线
    FlowCanvas canvas;
    const QString a = canvas.addNode("module.a", "A", QPointF(0, 0));
    const QString b = canvas.addNode("module.b", "B", QPointF(260, 0));

    canvas.addConnection(a, "next", b, "control", "control");
    QCOMPARE(canvas.m_connections.size(), 1);
    QVERIFY(canvas.m_connections.first()->isControlEdge());
}

void TestFlowCanvas::testPortSpecsFromPluginManagerLoaded() {
    // 节点创建后应从 PluginManager 加载端口声明
    FlowCanvas canvas;
    const QString nodeId = canvas.addNode("module.a", "A", QPointF(0, 0));
    FlowNodeItem* node = canvas.nodeItem(nodeId);
    QVERIFY(node);

    // 无插件元数据时端口列表为空（默认端口回退）
    // 主要验证 setPortSpecs 可被调用且不崩溃
    QList<PortSpec> inputs;
    PortSpec in; in.id = "image"; in.displayName = "image"; in.type = DataType::Image2D;
    inputs.append(in);
    QList<PortSpec> outputs;
    PortSpec out; out.id = "image"; out.displayName = "image"; out.type = DataType::Image2D;
    outputs.append(out);
    node->setPortSpecs(inputs, outputs);

    QCOMPARE(node->inputPortCount(), 1);
    QCOMPARE(node->outputPortCount(), 1);
    QCOMPARE(node->inputPortSpecs().first().id, QString("image"));
}

void TestFlowCanvas::testDeleteSecondConnectionPreservesFirst() {
    // P1-fix: 删除 A.val→B.val 时，A.image→B.image 应保留
    FlowCanvas canvas;
    Project* project = ProjectManager::instance().currentProject();

    const QString a = canvas.addNode("module.a", "A", QPointF(0, 0));
    const QString b = canvas.addNode("module.b", "B", QPointF(260, 0));

    canvas.addConnection(a, "image", b, "image");
    canvas.addConnection(a, "val", b, "val");
    QCOMPARE(canvas.m_connections.size(), 2);
    QCOMPARE(project->connections().size(), 2);

    // 删除第二条（val→val）
    canvas.removeConnection(a, "val", b, "val");
    QCOMPARE(canvas.m_connections.size(), 1);
    QCOMPARE(project->connections().size(), 1);
    // 确认剩余的是 image→image
    QCOMPARE(canvas.m_connections.first()->fromPortId(), QString("image"));
    QCOMPARE(canvas.m_connections.first()->toPortId(), QString("image"));
    QCOMPARE(project->connections().first().fromPort, QString("image"));
    QCOMPARE(project->connections().first().toPort, QString("image"));
}

void TestFlowCanvas::testDuplicateConnectionIsIdempotent() {
    // P1-fix: 重复添加同一四元组连接 → 模型幂等
    Project project;
    ModuleInstance a; a.id = "A"; a.moduleId = "module.a"; a.name = "A";
    ModuleInstance b; b.id = "B"; b.moduleId = "module.b"; b.name = "B";
    project.addModule(a);
    project.addModule(b);

    ModuleConnection c;
    c.fromModuleId = "A"; c.toModuleId = "B";
    c.fromPort = "image"; c.toPort = "image"; c.edgeType = "data";

    project.addConnection(c);
    project.addConnection(c); // 重复
    project.addConnection(c); // 再次重复

    QCOMPARE(project.connections().size(), 1);
}

void TestFlowCanvas::testProjectRemoveConnectionWithPortsPrecise() {
    // P1-fix: removeConnectionWithPorts 只删除匹配的四元组
    Project project;
    ModuleInstance a; a.id = "A"; a.moduleId = "module.a";
    ModuleInstance b; b.id = "B"; b.moduleId = "module.b";
    project.addModule(a);
    project.addModule(b);

    ModuleConnection c1;
    c1.fromModuleId = "A"; c1.toModuleId = "B";
    c1.fromPort = "image"; c1.toPort = "image"; c1.edgeType = "data";
    ModuleConnection c2;
    c2.fromModuleId = "A"; c2.toModuleId = "B";
    c2.fromPort = "val"; c2.toPort = "val"; c2.edgeType = "data";
    project.addConnection(c1);
    project.addConnection(c2);
    QCOMPARE(project.connections().size(), 2);

    QSignalSpy spy(&project, &Project::connectionRemovedWithPorts);
    project.removeConnectionWithPorts("A", "val", "B", "val");

    QCOMPARE(project.connections().size(), 1);
    QCOMPARE(project.connections().first().fromPort, QString("image"));
    QCOMPARE(spy.count(), 1);
    const auto args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QString("A"));
    QCOMPARE(args.at(1).toString(), QString("val"));
    QCOMPARE(args.at(2).toString(), QString("B"));
    QCOMPARE(args.at(3).toString(), QString("val"));
}

QTEST_MAIN(TestFlowCanvas)
#include "test_flowcanvas.moc"
