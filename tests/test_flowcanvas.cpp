#include <QGraphicsSceneEvent>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QToolTip>
#include <QtTest/QtTest>
#include <core/engine/FlowGraphAnalyzer.h>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>
#include <ui/controllers/FlowLayoutCoordinator.h>
#include <ui/widgets/FlowCanvas.h>

using namespace DeepLux;

class TestableFlowNodeItem : public FlowNodeItem {
public:
    using FlowNodeItem::FlowNodeItem;
    using FlowNodeItem::mouseMoveEvent;
    using FlowNodeItem::mousePressEvent;
    using FlowNodeItem::mouseReleaseEvent;
};

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
    void testPortTooltipUsesCompleteDataTypeName();
    void testConnectionDragMovesAndClearsCompatibleHighlight();

    // B2: 执行计划布局与坐标写回
    void testExecutionLayoutByRankAndLane();
    void testExecutionLayoutSizesRowsByNodeHeight();
    void testNormalizeLayoutPreservesX();
    void testNodeDragWritesBackPosition();
    void testLayoutInitializedRestoresSavedPositions();
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
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "module.a";
    a.name = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "module.b";
    b.name = "B";
    project.addModule(a);
    project.addModule(b);

    ModuleConnection c1;
    c1.fromModuleId = "A";
    c1.toModuleId = "B";
    c1.fromPort = "image";
    c1.toPort = "image";
    c1.edgeType = "data";
    project.addConnection(c1);

    ModuleConnection c2;
    c2.fromModuleId = "A";
    c2.toModuleId = "B";
    c2.fromPort = "val";
    c2.toPort = "val";
    c2.edgeType = "data";
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
    PortSpec in;
    in.id = "image";
    in.displayName = "image";
    in.type = DataType::Image2D;
    inputs.append(in);
    QList<PortSpec> outputs;
    PortSpec out;
    out.id = "image";
    out.displayName = "image";
    out.type = DataType::Image2D;
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
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "module.a";
    a.name = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "module.b";
    b.name = "B";
    project.addModule(a);
    project.addModule(b);

    ModuleConnection c;
    c.fromModuleId = "A";
    c.toModuleId = "B";
    c.fromPort = "image";
    c.toPort = "image";
    c.edgeType = "data";

    project.addConnection(c);
    project.addConnection(c); // 重复
    project.addConnection(c); // 再次重复

    QCOMPARE(project.connections().size(), 1);
}

void TestFlowCanvas::testProjectRemoveConnectionWithPortsPrecise() {
    // P1-fix: removeConnectionWithPorts 只删除匹配的四元组
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "module.a";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "module.b";
    project.addModule(a);
    project.addModule(b);

    ModuleConnection c1;
    c1.fromModuleId = "A";
    c1.toModuleId = "B";
    c1.fromPort = "image";
    c1.toPort = "image";
    c1.edgeType = "data";
    ModuleConnection c2;
    c2.fromModuleId = "A";
    c2.toModuleId = "B";
    c2.fromPort = "val";
    c2.toPort = "val";
    c2.edgeType = "data";
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

void TestFlowCanvas::testPortTooltipUsesCompleteDataTypeName() {
    FlowCanvas canvas;
    canvas.setSceneRect(0, 0, 400, 240);
    canvas.resize(400, 240);

    const QString nodeId = canvas.addNode("module.a", "A", QPointF(80, 80));
    FlowNodeItem* node = canvas.nodeItem(nodeId);
    QVERIFY(node != nullptr);

    PortSpec input;
    input.id = "point";
    input.displayName = QStringLiteral("三维点");
    input.type = DataType::Point3D;
    input.required = true;
    node->setPortSpecs({input}, {});

    canvas.show();
    QCoreApplication::processEvents();
    QTest::mouseMove(canvas.viewport(), canvas.mapFromScene(node->scenePos() + node->inputPortPos(0)));
    QTRY_COMPARE(QToolTip::text(), QStringLiteral("三维点 (Point3D) [必需]"));

    QTest::mouseMove(canvas.viewport(), canvas.mapFromScene(QPointF(10, 10)));
    QTRY_VERIFY(QToolTip::text().isEmpty());
}

void TestFlowCanvas::testConnectionDragMovesAndClearsCompatibleHighlight() {
    FlowCanvas canvas;
    auto* source = new TestableFlowNodeItem("source", "Source", "module.source");
    auto* first = new FlowNodeItem("first", "First", "module.first");
    auto* second = new FlowNodeItem("second", "Second", "module.second");
    canvas.scene()->addItem(source);
    canvas.scene()->addItem(first);
    canvas.scene()->addItem(second);
    source->setPos(80, 60);
    first->setPos(80, 240);
    second->setPos(380, 240);

    PortSpec output;
    output.id = "point";
    output.displayName = "point";
    output.type = DataType::Point3D;
    PortSpec input = output;
    source->setPortSpecs({}, {output});
    first->setPortSpecs({input}, {});
    second->setPortSpecs({input}, {});

    QGraphicsSceneMouseEvent press(QEvent::GraphicsSceneMousePress);
    press.setScenePos(source->scenePos() + source->outputPortPos(0));
    press.setButton(Qt::LeftButton);
    press.setButtons(Qt::LeftButton);
    source->mousePressEvent(&press);

    QGraphicsSceneMouseEvent move(QEvent::GraphicsSceneMouseMove);
    move.setButtons(Qt::LeftButton);
    move.setScenePos(first->scenePos() + first->inputPortPos(0));
    source->mouseMoveEvent(&move);
    QVERIFY(first->isCompatibleHighlighted());

    move.setScenePos(second->scenePos() + second->inputPortPos(0));
    source->mouseMoveEvent(&move);
    QVERIFY(!first->isCompatibleHighlighted());
    QVERIFY(second->isCompatibleHighlighted());

    QGraphicsSceneMouseEvent release(QEvent::GraphicsSceneMouseRelease);
    release.setScenePos(second->scenePos() + second->inputPortPos(0));
    release.setButton(Qt::LeftButton);
    source->mouseReleaseEvent(&release);
    QVERIFY(!second->isCompatibleHighlighted());
}

// ---------------------------------------------------------------------------
// B2: 执行计划布局与坐标写回
// ---------------------------------------------------------------------------

namespace {

void appendPlanNode(FlowOrderPlan& plan, const QString& id, int rank, int lane) {
    FlowOrderNode node;
    node.instanceId = id;
    node.rank = rank;
    node.lane = lane;
    node.displayIndex = plan.nodes.size();
    plan.nodes.append(node);
    plan.displayOrder.append(id);
    plan.rankIndex.insert(id, rank);
    plan.laneIndex.insert(id, lane);
}

QList<PortSpec> makeOutputPorts(int count) {
    QList<PortSpec> outputs;
    for (int i = 0; i < count; ++i) {
        PortSpec port;
        port.id = QStringLiteral("out_%1").arg(i);
        port.displayName = port.id;
        port.type = DataType::Any;
        outputs.append(port);
    }
    return outputs;
}

FlowGraphCatalog catalogForProject(const Project& project) {
    FlowGraphCatalog catalog;
    for (const ModuleInstance& instance : project.modules()) {
        FlowGraphModule module;
        module.instanceId = instance.id;
        module.flowType = ControlFlowType::Sequential;
        catalog.modules.append(module);
    }
    return catalog;
}

} // namespace

void TestFlowCanvas::testExecutionLayoutByRankAndLane() {
    FlowCanvas canvas;
    const QString a = canvas.addNode("module.a", "A", QPointF(500, 500));
    const QString b = canvas.addNode("module.b", "B", QPointF(500, 500));
    const QString c = canvas.addNode("module.c", "C", QPointF(500, 500));
    const QString d = canvas.addNode("module.d", "D", QPointF(500, 500));
    const QString e = canvas.addNode("module.e", "E", QPointF(500, 500));

    FlowOrderPlan plan;
    plan.valid = true;
    appendPlanNode(plan, a, 0, 0);
    appendPlanNode(plan, b, 1, 0);
    appendPlanNode(plan, c, 2, 0);
    appendPlanNode(plan, d, 1, 1);
    appendPlanNode(plan, e, 1, -1);

    canvas.applyExecutionLayout(plan, FlowCanvas::LayoutMode::Full);

    const qreal ya = canvas.nodeItem(a)->pos().y();
    const qreal yb = canvas.nodeItem(b)->pos().y();
    const qreal yc = canvas.nodeItem(c)->pos().y();
    QVERIFY(ya < yb);
    QVERIFY(yb < yc);

    // 同一 rank 同一 Y；不同 lane 的 X 不重叠
    QCOMPARE(canvas.nodeItem(d)->pos().y(), yb);
    QCOMPARE(canvas.nodeItem(e)->pos().y(), yb);
    QVERIFY(qAbs(canvas.nodeItem(d)->pos().x() - canvas.nodeItem(b)->pos().x()) >= 220.0);
    QVERIFY(qAbs(canvas.nodeItem(e)->pos().x() - canvas.nodeItem(b)->pos().x()) >= 220.0);

    const QStringList ids = {a, b, c, d, e};
    for (int i = 0; i < ids.size(); ++i) {
        for (int j = i + 1; j < ids.size(); ++j) {
            FlowNodeItem* first = canvas.nodeItem(ids.at(i));
            FlowNodeItem* second = canvas.nodeItem(ids.at(j));
            QVERIFY2(!first->sceneBoundingRect().intersects(second->sceneBoundingRect()),
                     qPrintable(QStringLiteral("nodes %1 and %2 overlap").arg(ids.at(i), ids.at(j))));
        }
    }
}

void TestFlowCanvas::testExecutionLayoutSizesRowsByNodeHeight() {
    FlowCanvas canvas;
    const QString tall = canvas.addNode("module.tall", "Tall", QPointF(0, 0));
    const QString shortNode = canvas.addNode("module.short", "Short", QPointF(0, 0));
    const QString tallPeer = canvas.addNode("module.tall_peer", "Tall Peer", QPointF(0, 0));

    // 6~11 个输出端口的真实插件会超过旧的固定 160px 行距。
    canvas.nodeItem(tall)->setPortSpecs({}, makeOutputPorts(11));
    canvas.nodeItem(tallPeer)->setPortSpecs({}, makeOutputPorts(11));
    QVERIFY(canvas.nodeItem(tall)->boundingRect().height() > 160.0);

    FlowOrderPlan plan;
    plan.valid = true;
    appendPlanNode(plan, tall, 0, 0);
    appendPlanNode(plan, shortNode, 1, 0);
    appendPlanNode(plan, tallPeer, 1, 1);

    auto verifyNoOverlap = [&canvas]() {
        const QStringList ids = canvas.nodeIds();
        for (int i = 0; i < ids.size(); ++i) {
            for (int j = i + 1; j < ids.size(); ++j) {
                FlowNodeItem* first = canvas.nodeItem(ids.at(i));
                FlowNodeItem* second = canvas.nodeItem(ids.at(j));
                QVERIFY2(!first->sceneBoundingRect().intersects(second->sceneBoundingRect()),
                         qPrintable(QStringLiteral("nodes %1 and %2 overlap").arg(ids.at(i), ids.at(j))));
            }
        }
    };

    for (FlowCanvas::LayoutMode mode : {FlowCanvas::LayoutMode::Full, FlowCanvas::LayoutMode::Normalize}) {
        canvas.applyExecutionLayout(plan, mode);
        FlowNodeItem* top = canvas.nodeItem(tall);
        FlowNodeItem* below = canvas.nodeItem(shortNode);
        FlowNodeItem* peer = canvas.nodeItem(tallPeer);

        QCOMPARE(below->pos().y(), peer->pos().y());
        QVERIFY(top->sceneBoundingRect().bottom() < below->sceneBoundingRect().top());
        verifyNoOverlap();
    }
}

void TestFlowCanvas::testNormalizeLayoutPreservesX() {
    FlowCanvas canvas;
    const QString a = canvas.addNode("module.a", "A", QPointF(37, 10));
    const QString b = canvas.addNode("module.b", "B", QPointF(142, 20));
    const QString c = canvas.addNode("module.c", "C", QPointF(-55, 30));

    FlowOrderPlan plan;
    plan.valid = true;
    appendPlanNode(plan, a, 0, 0);
    appendPlanNode(plan, b, 1, 0);
    appendPlanNode(plan, c, 2, 0);

    canvas.applyExecutionLayout(plan, FlowCanvas::LayoutMode::Normalize);

    QCOMPARE(canvas.nodeItem(a)->pos().x(), 37.0);
    QCOMPARE(canvas.nodeItem(b)->pos().x(), 142.0);
    QCOMPARE(canvas.nodeItem(c)->pos().x(), -55.0);
    QVERIFY(canvas.nodeItem(a)->pos().y() < canvas.nodeItem(b)->pos().y());
    QVERIFY(canvas.nodeItem(b)->pos().y() < canvas.nodeItem(c)->pos().y());
}

void TestFlowCanvas::testNodeDragWritesBackPosition() {
    FlowCanvas canvas;
    canvas.resize(400, 300);
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project != nullptr);

    ModuleInstance instance;
    instance.id = QStringLiteral("drag_node");
    instance.moduleId = QStringLiteral("module.drag");
    project->addModule(instance);

    auto* node = new TestableFlowNodeItem("drag_node", "Drag", "module.drag");
    canvas.scene()->addItem(node);
    node->setPos(20, 30);

    QSignalSpy projectSpy(project, &Project::modulePositionChanged);
    QSignalSpy canvasSpy(&canvas, &FlowCanvas::nodePositionChanged);
    QVERIFY(projectSpy.isValid());
    QVERIFY(canvasSpy.isValid());

    const QPointF downPos = node->pos();
    QGraphicsSceneMouseEvent press(QEvent::GraphicsSceneMousePress);
    press.setScenePos(downPos);
    press.setButton(Qt::LeftButton);
    press.setButtons(Qt::LeftButton);
    press.setButtonDownScenePos(Qt::LeftButton, downPos);
    node->mousePressEvent(&press);

    QGraphicsSceneMouseEvent move(QEvent::GraphicsSceneMouseMove);
    move.setScenePos(downPos + QPointF(30, 40));
    move.setLastScenePos(downPos);
    move.setButtonDownScenePos(Qt::LeftButton, downPos);
    move.setButtons(Qt::LeftButton);
    node->mouseMoveEvent(&move);
    node->setPos(QPointF(50, 70));

    QGraphicsSceneMouseEvent release(QEvent::GraphicsSceneMouseRelease);
    release.setScenePos(QPointF(50, 70));
    release.setButton(Qt::LeftButton);
    release.setButtons(Qt::LeftButton);
    node->mouseReleaseEvent(&release);

    QCOMPARE(canvasSpy.count(), 1);
    QCOMPARE(projectSpy.count(), 1);
    QCOMPARE(project->moduleById(QStringLiteral("drag_node"))->posX, 50);
    QCOMPARE(project->moduleById(QStringLiteral("drag_node"))->posY, 70);

    const QList<QVariant> args = projectSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("drag_node"));
    QCOMPARE(args.at(1).toInt(), 50);
    QCOMPARE(args.at(2).toInt(), 70);
}

void TestFlowCanvas::testLayoutInitializedRestoresSavedPositions() {
    // 已初始化布局：加载后保留保存坐标，不自动重排
    Project initialized;
    ModuleInstance first;
    first.id = "init_a";
    first.moduleId = "module.a";
    first.name = "A";
    first.posX = 40;
    first.posY = 60;
    initialized.addModule(first);
    ModuleInstance second;
    second.id = "init_b";
    second.moduleId = "module.b";
    second.name = "B";
    second.posX = 260;
    second.posY = 60;
    initialized.addModule(second);
    initialized.setLayoutInitialized(true);

    FlowCanvas canvas;
    canvas.loadFromProject(&initialized);

    FlowLayoutCoordinator coordinator(&initialized, &canvas,
                                      [](const Project& project) { return catalogForProject(project); });
    coordinator.setProject(&initialized);
    QTest::qWait(80);
    QCOMPARE(coordinator.layoutRunCount(), 0);
    QCOMPARE(canvas.nodeItem("init_a")->pos(), QPointF(40, 60));
    QCOMPARE(canvas.nodeItem("init_b")->pos(), QPointF(260, 60));

    // 未初始化（全部 0,0）：触发 Full 并标记 layoutInitialized
    Project uninitialized;
    ModuleInstance ua;
    ua.id = "u_a";
    ua.moduleId = "module.a";
    uninitialized.addModule(ua);
    ModuleInstance ub;
    ub.id = "u_b";
    ub.moduleId = "module.b";
    uninitialized.addModule(ub);

    FlowCanvas canvas2;
    canvas2.loadFromProject(&uninitialized);
    FlowLayoutCoordinator coordinator2(&uninitialized, &canvas2,
                                       [](const Project& project) { return catalogForProject(project); });
    coordinator2.setProject(&uninitialized);

    QTRY_COMPARE(coordinator2.layoutRunCount(), 1);
    QVERIFY(uninitialized.layoutInitialized());
    QVERIFY(canvas2.nodeItem("u_a")->pos() != canvas2.nodeItem("u_b")->pos());
}

QTEST_MAIN(TestFlowCanvas)
#include "test_flowcanvas.moc"
