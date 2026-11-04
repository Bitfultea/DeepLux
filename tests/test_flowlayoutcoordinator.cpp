#include <QSignalSpy>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QtTest/QtTest>
#include <core/engine/FlowGraphAnalyzer.h>
#include <core/model/Project.h>
#include <ui/controllers/FlowLayoutCoordinator.h>
#include <ui/controllers/ProcessTreeController.h>
#include <ui/widgets/FlowCanvas.h>

using namespace DeepLux;

namespace {

ModuleInstance makeInstance(const QString& id, int x = 0, int y = 0) {
    ModuleInstance instance;
    instance.id = id;
    instance.moduleId = QStringLiteral("module.%1").arg(id);
    instance.name = id;
    instance.posX = x;
    instance.posY = y;
    return instance;
}

ModuleConnection makeConnection(const QString& from, const QString& to) {
    ModuleConnection connection;
    connection.fromModuleId = from;
    connection.toModuleId = to;
    connection.fromPort = QStringLiteral("image");
    connection.toPort = QStringLiteral("image");
    connection.edgeType = QStringLiteral("data");
    return connection;
}

FlowGraphCatalog sequentialCatalog(const Project& project) {
    FlowGraphCatalog catalog;
    for (const ModuleInstance& instance : project.modules()) {
        FlowGraphModule module;
        module.instanceId = instance.id;
        module.flowType = ControlFlowType::Sequential;
        catalog.modules.append(module);
    }
    return catalog;
}

void addTreeItem(QTreeWidget* tree, const QString& id) {
    QTreeWidgetItem* item = new QTreeWidgetItem();
    item->setData(0, Qt::UserRole, QStringLiteral("flow_item"));
    item->setData(0, Qt::UserRole + 1, id);
    item->setText(0, id);
    tree->addTopLevelItem(item);
}

QStringList treeOrder(const QTreeWidget& tree) {
    QStringList ids;
    for (int i = 0; i < tree.topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree.topLevelItem(i);
        ids.append(item ? item->data(0, Qt::UserRole + 1).toString() : QString());
    }
    return ids;
}

} // namespace

class TestFlowLayoutCoordinator : public QObject {
    Q_OBJECT

private slots:
    void testCoalescesStructureSignalsIntoOneLayout();
    void testLayoutMatchesDisplayOrder();
    void testUserDragDoesNotRelayoutButNewConnectionDoes();
    void testSuppressionCancelsPendingLayout();
    void testProjectSwitchPreservesInitializedPositions();
    void testMissingCatalogModuleKeepsPositions();
    void testLayoutAppliesDisplayOrderToProcessTree();
    void testProjectSwitchSortsTreeWithoutCanvasLayout();
};

void TestFlowLayoutCoordinator::testCoalescesStructureSignalsIntoOneLayout() {
    Project project;
    FlowCanvas canvas;
    FlowLayoutCoordinator coordinator(nullptr, &canvas, [](const Project& p) { return sequentialCatalog(p); });
    coordinator.setProject(&project);

    // 3 个 moduleAdded + 2 个 connectionAdded，全部发生在同一轮事件循环内。
    project.addModule(makeInstance(QStringLiteral("a")));
    project.addModule(makeInstance(QStringLiteral("b")));
    project.addModule(makeInstance(QStringLiteral("c")));
    canvas.loadFromProject(&project);
    project.addConnection(makeConnection(QStringLiteral("a"), QStringLiteral("b")));
    project.addConnection(makeConnection(QStringLiteral("b"), QStringLiteral("c")));

    QTRY_COMPARE(coordinator.layoutRunCount(), 1);
    QTest::qWait(60);
    QCOMPARE(coordinator.layoutRunCount(), 1);

    QVERIFY(coordinator.currentPlan().valid);
    QCOMPARE(coordinator.currentPlan().displayOrder.size(), 3);
}

void TestFlowLayoutCoordinator::testLayoutMatchesDisplayOrder() {
    Project project;
    project.addModule(makeInstance(QStringLiteral("a")));
    project.addModule(makeInstance(QStringLiteral("b")));
    project.addModule(makeInstance(QStringLiteral("c")));
    project.addConnection(makeConnection(QStringLiteral("a"), QStringLiteral("b")));
    project.addConnection(makeConnection(QStringLiteral("b"), QStringLiteral("c")));

    FlowCanvas canvas;
    canvas.loadFromProject(&project);
    FlowLayoutCoordinator coordinator(&project, &canvas, [](const Project& p) { return sequentialCatalog(p); });

    QTRY_COMPARE(coordinator.layoutRunCount(), 1);

    const FlowOrderPlan& plan = coordinator.currentPlan();
    QVERIFY(plan.valid);
    QCOMPARE(plan.displayOrder.size(), 3);

    qreal previousY = -1.0;
    qreal previousBottom = -1.0e9;
    for (const QString& id : plan.displayOrder) {
        FlowNodeItem* item = canvas.nodeItem(id);
        QVERIFY(item != nullptr);
        QVERIFY2(item->pos().y() >= previousY, "displayOrder must be laid out top-to-bottom");
        QVERIFY2(item->sceneBoundingRect().top() > previousBottom, "rank rows must not overlap");
        previousY = item->pos().y();
        previousBottom = item->sceneBoundingRect().bottom();
    }
    QCOMPARE(plan.displayOrder.first(), QStringLiteral("a"));
    QCOMPARE(plan.displayOrder.last(), QStringLiteral("c"));
}

void TestFlowLayoutCoordinator::testUserDragDoesNotRelayoutButNewConnectionDoes() {
    Project project;
    project.addModule(makeInstance(QStringLiteral("a")));
    project.addModule(makeInstance(QStringLiteral("b")));
    project.addConnection(makeConnection(QStringLiteral("a"), QStringLiteral("b")));

    FlowCanvas canvas;
    canvas.loadFromProject(&project);
    FlowLayoutCoordinator coordinator(&project, &canvas, [](const Project& p) { return sequentialCatalog(p); });

    QTRY_COMPARE(coordinator.layoutRunCount(), 1);
    const int countAfterLayout = coordinator.layoutRunCount();

    // 用户拖动只写回坐标，不触发重新布局。
    FlowNodeItem* node = canvas.nodeItem(QStringLiteral("a"));
    QVERIFY(node != nullptr);
    node->setPos(QPointF(777, 888));
    project.setModulePosition(QStringLiteral("a"), 777, 888);

    QTest::qWait(80);
    QCOMPARE(coordinator.layoutRunCount(), countAfterLayout);
    QCOMPARE(canvas.nodeItem(QStringLiteral("a"))->pos(), QPointF(777, 888));

    // 新结构（新增模块 + 连接）触发一次 Full；使用无环的 a→c。
    project.addModule(makeInstance(QStringLiteral("c")));
    canvas.loadFromProject(&project);
    project.addConnection(makeConnection(QStringLiteral("a"), QStringLiteral("c")));
    QTRY_COMPARE(coordinator.layoutRunCount(), countAfterLayout + 1);
    QVERIFY(canvas.nodeItem(QStringLiteral("a"))->pos() != QPointF(777, 888));
}

void TestFlowLayoutCoordinator::testSuppressionCancelsPendingLayout() {
    Project project;
    project.addModule(makeInstance(QStringLiteral("a")));
    project.addModule(makeInstance(QStringLiteral("b")));
    project.addConnection(makeConnection(QStringLiteral("a"), QStringLiteral("b")));

    FlowCanvas canvas;
    canvas.loadFromProject(&project);
    FlowLayoutCoordinator coordinator(&project, &canvas, [](const Project& p) { return sequentialCatalog(p); });

    QTRY_COMPARE(coordinator.layoutRunCount(), 1);
    const int baseline = coordinator.layoutRunCount();

    // 制造一个待执行的布局，然后抑制。
    project.addConnection(makeConnection(QStringLiteral("b"), QStringLiteral("a")));
    coordinator.suppressAutoLayout(true);
    QVERIFY(coordinator.autoLayoutSuppressed());

    QSignalSpy appliedSpy(&coordinator, &FlowLayoutCoordinator::layoutApplied);
    QVERIFY(appliedSpy.isValid());
    QVERIFY(!appliedSpy.wait(120));
    QCOMPARE(coordinator.layoutRunCount(), baseline);

    // Undo 恢复的坐标不得被覆盖。
    project.setModulePosition(QStringLiteral("a"), 33, 44);
    project.setModulePosition(QStringLiteral("b"), 55, 66);
    canvas.nodeItem(QStringLiteral("a"))->setPos(QPointF(33, 44));
    canvas.nodeItem(QStringLiteral("b"))->setPos(QPointF(55, 66));
    QTest::qWait(80);
    QCOMPARE(canvas.nodeItem(QStringLiteral("a"))->pos(), QPointF(33, 44));
    QCOMPARE(canvas.nodeItem(QStringLiteral("b"))->pos(), QPointF(55, 66));

    // 解除抑制不会自动补跑。
    coordinator.suppressAutoLayout(false);
    QTest::qWait(80);
    QCOMPARE(coordinator.layoutRunCount(), baseline);
}

void TestFlowLayoutCoordinator::testProjectSwitchPreservesInitializedPositions() {
    Project first;
    first.addModule(makeInstance(QStringLiteral("a"), 40, 60));
    first.addModule(makeInstance(QStringLiteral("b"), 300, 60));
    first.setLayoutInitialized(true);

    Project second;
    second.addModule(makeInstance(QStringLiteral("x"), 12, 34));
    second.addModule(makeInstance(QStringLiteral("y"), 250, 34));
    second.setLayoutInitialized(true);

    FlowCanvas canvas;
    FlowLayoutCoordinator coordinator(nullptr, &canvas, [](const Project& p) { return sequentialCatalog(p); });

    canvas.loadFromProject(&first);
    coordinator.setProject(&first);
    QTest::qWait(80);
    QCOMPARE(coordinator.layoutRunCount(), 0);
    QCOMPARE(canvas.nodeItem(QStringLiteral("a"))->pos(), QPointF(40, 60));
    QCOMPARE(canvas.nodeItem(QStringLiteral("b"))->pos(), QPointF(300, 60));

    // 切走前存在待执行布局：切换必须取消它。
    first.addModule(makeInstance(QStringLiteral("pending")));
    canvas.loadFromProject(&second);
    coordinator.setProject(&second);
    QTest::qWait(100);
    QCOMPARE(coordinator.layoutRunCount(), 0);
    QCOMPARE(canvas.nodeItem(QStringLiteral("x"))->pos(), QPointF(12, 34));
    QCOMPARE(canvas.nodeItem(QStringLiteral("y"))->pos(), QPointF(250, 34));

    // 未初始化/坐标无效的工程才触发 Full。
    Project uninitialized;
    uninitialized.addModule(makeInstance(QStringLiteral("u1")));
    uninitialized.addModule(makeInstance(QStringLiteral("u2")));
    QVERIFY(!uninitialized.layoutInitialized());

    canvas.loadFromProject(&uninitialized);
    coordinator.setProject(&uninitialized);
    QTRY_COMPARE(coordinator.layoutRunCount(), 1);
    QVERIFY(uninitialized.layoutInitialized());
    QVERIFY(canvas.nodeItem(QStringLiteral("u1"))->pos() != canvas.nodeItem(QStringLiteral("u2"))->pos());
}

void TestFlowLayoutCoordinator::testMissingCatalogModuleKeepsPositions() {
    Project project;
    project.addModule(makeInstance(QStringLiteral("a"), 100, 100));
    project.addModule(makeInstance(QStringLiteral("b"), 400, 100));
    project.setLayoutInitialized(true);

    FlowCanvas canvas;
    canvas.loadFromProject(&project);

    // provider 只返回 a，缺少 b：布局必须放弃并保持原坐标。
    auto provider = [](const Project& p) {
        FlowGraphCatalog catalog;
        for (const ModuleInstance& instance : p.modules()) {
            if (instance.id == QStringLiteral("b"))
                continue;
            FlowGraphModule module;
            module.instanceId = instance.id;
            module.flowType = ControlFlowType::Sequential;
            catalog.modules.append(module);
        }
        return catalog;
    };

    FlowLayoutCoordinator coordinator(&project, &canvas, provider);
    QTest::qWait(50);
    QCOMPARE(coordinator.layoutRunCount(), 0);

    QSignalSpy appliedSpy(&coordinator, &FlowLayoutCoordinator::layoutApplied);
    project.addConnection(makeConnection(QStringLiteral("a"), QStringLiteral("b")));
    QVERIFY(!appliedSpy.wait(120));
    QCOMPARE(coordinator.layoutRunCount(), 0);

    QCOMPARE(canvas.nodeItem(QStringLiteral("a"))->pos(), QPointF(100, 100));
    QCOMPARE(canvas.nodeItem(QStringLiteral("b"))->pos(), QPointF(400, 100));
    QCOMPARE(project.moduleById(QStringLiteral("a"))->posX, 100);
    QCOMPARE(project.moduleById(QStringLiteral("b"))->posX, 400);
}

void TestFlowLayoutCoordinator::testLayoutAppliesDisplayOrderToProcessTree() {
    Project project;
    project.addModule(makeInstance(QStringLiteral("a")));
    project.addModule(makeInstance(QStringLiteral("b")));
    project.addModule(makeInstance(QStringLiteral("c")));
    project.addConnection(makeConnection(QStringLiteral("a"), QStringLiteral("b")));
    project.addConnection(makeConnection(QStringLiteral("b"), QStringLiteral("c")));

    FlowCanvas canvas;
    canvas.loadFromProject(&project);

    QTreeWidget tree;
    tree.setColumnCount(1);
    // 故意用逆序插入，证明布局后流程树被 displayOrder 重排，而不是保留插入顺序。
    addTreeItem(&tree, QStringLiteral("c"));
    addTreeItem(&tree, QStringLiteral("b"));
    addTreeItem(&tree, QStringLiteral("a"));
    ProcessTreeController treeController(&tree);

    FlowLayoutCoordinator coordinator(&project, &canvas, [](const Project& p) { return sequentialCatalog(p); });
    coordinator.setProcessTreeController(&treeController);
    QTRY_COMPARE(coordinator.layoutRunCount(), 1);

    QVERIFY(coordinator.currentPlan().valid);
    QCOMPARE(coordinator.processTreeController(), &treeController);
    QCOMPARE(treeOrder(tree), coordinator.currentPlan().displayOrder);
    QCOMPARE(coordinator.currentPlan().displayOrder, (QStringList{"a", "b", "c"}));
    for (int i = 0; i < tree.topLevelItemCount(); ++i) {
        QCOMPARE(tree.topLevelItem(i)->data(0, Qt::UserRole + 6).toInt(), i);
    }
}

void TestFlowLayoutCoordinator::testProjectSwitchSortsTreeWithoutCanvasLayout() {
    Project first;
    first.addModule(makeInstance(QStringLiteral("a"), 40, 60));
    first.addModule(makeInstance(QStringLiteral("b"), 300, 60));
    first.setLayoutInitialized(true);

    Project second;
    second.addModule(makeInstance(QStringLiteral("x"), 12, 34));
    second.addModule(makeInstance(QStringLiteral("y"), 250, 34));
    second.setLayoutInitialized(true);

    FlowCanvas canvas;
    QTreeWidget tree;
    tree.setColumnCount(1);
    ProcessTreeController treeController(&tree);
    FlowLayoutCoordinator coordinator(nullptr, &canvas, [](const Project& p) { return sequentialCatalog(p); });
    coordinator.setProcessTreeController(&treeController);

    addTreeItem(&tree, QStringLiteral("b"));
    addTreeItem(&tree, QStringLiteral("a"));
    canvas.loadFromProject(&first);
    coordinator.setProject(&first);
    QTest::qWait(80);
    QCOMPARE(coordinator.layoutRunCount(), 0);
    QCOMPARE(canvas.nodeItem(QStringLiteral("a"))->pos(), QPointF(40, 60));
    QCOMPARE(treeOrder(tree), (QStringList{"a", "b"}));

    // 切换到另一个已初始化且坐标有效的工程：画布坐标原样保留，只排流程树。
    tree.clear();
    treeController.clearInstanceItemMap();
    addTreeItem(&tree, QStringLiteral("y"));
    addTreeItem(&tree, QStringLiteral("x"));
    canvas.loadFromProject(&second);
    coordinator.setProject(&second);
    QTest::qWait(80);
    QCOMPARE(coordinator.layoutRunCount(), 0);
    QCOMPARE(canvas.nodeItem(QStringLiteral("x"))->pos(), QPointF(12, 34));
    QCOMPARE(canvas.nodeItem(QStringLiteral("y"))->pos(), QPointF(250, 34));
    QCOMPARE(treeOrder(tree), (QStringList{"x", "y"}));
    QVERIFY(coordinator.currentPlan().valid);
    QCOMPARE(coordinator.currentPlan().displayOrder, (QStringList{"x", "y"}));
}

QTEST_MAIN(TestFlowLayoutCoordinator)
#include "test_flowlayoutcoordinator.moc"
