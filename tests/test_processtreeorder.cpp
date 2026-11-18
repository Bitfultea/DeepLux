#include <QScrollBar>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QtTest/QtTest>
#include <ui/controllers/ProcessTreeController.h>

using namespace DeepLux;

namespace {

QTreeWidgetItem* makeItem(QTreeWidget* tree, const QString& id) {
    QTreeWidgetItem* item = new QTreeWidgetItem(tree);
    item->setData(0, Qt::UserRole, QStringLiteral("flow_item"));
    item->setData(0, Qt::UserRole + 1, id);
    item->setText(0, id);
    return item;
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

class TestProcessTreeOrder : public QObject {
    Q_OBJECT

private slots:
    void testReorderMatchesDisplayOrderAndWritesDisplayIndex();
    void testReorderPreservesCurrentSelectionAndScroll();
    void testUnknownIdsAreAppendedAndMissingNodesAreKept();
    void testEmptyPlanKeepsAllItems();
    void testDuplicateAndUnknownPlanIdsDoNotDuplicateNodes();
};

void TestProcessTreeOrder::testReorderMatchesDisplayOrderAndWritesDisplayIndex() {
    QTreeWidget tree;
    tree.setColumnCount(1);
    makeItem(&tree, QStringLiteral("A"));
    makeItem(&tree, QStringLiteral("B"));
    makeItem(&tree, QStringLiteral("C"));
    ProcessTreeController controller(&tree);

    controller.applyExecutionOrder({"B", "A", "C"});

    QCOMPARE(treeOrder(tree), (QStringList{"B", "A", "C"}));
    for (int i = 0; i < tree.topLevelItemCount(); ++i) {
        QCOMPARE(tree.topLevelItem(i)->data(0, Qt::UserRole + 6).toInt(), i);
    }
    controller.rebuildInstanceItemMap();
    QCOMPARE(controller.instanceItem(QStringLiteral("B")), tree.topLevelItem(0));
    QCOMPARE(controller.instanceItem(QStringLiteral("A")), tree.topLevelItem(1));
    QCOMPARE(controller.instanceItem(QStringLiteral("C")), tree.topLevelItem(2));
}

void TestProcessTreeOrder::testReorderPreservesCurrentSelectionAndScroll() {
    QTreeWidget tree;
    tree.setColumnCount(1);
    QTreeWidgetItem* a = makeItem(&tree, QStringLiteral("A"));
    QTreeWidgetItem* b = makeItem(&tree, QStringLiteral("B"));
    QTreeWidgetItem* c = makeItem(&tree, QStringLiteral("C"));
    // 给树足够内容以便滚动条有有效范围；offscreen 下范围可能为 0，仍需断言不崩溃。
    for (int i = 0; i < 40; ++i) {
        makeItem(&tree, QStringLiteral("extra_%1").arg(i));
    }
    tree.setCurrentItem(b);
    b->setSelected(true);
    c->setExpanded(true);
    const int scroll = tree.verticalScrollBar() ? tree.verticalScrollBar()->value() : 0;
    ProcessTreeController controller(&tree);

    QSignalSpy currentSpy(&tree, &QTreeWidget::currentItemChanged);
    QVERIFY(currentSpy.isValid());

    controller.applyExecutionOrder({"C", "B", "A"});

    QCOMPARE(treeOrder(tree).mid(0, 3), (QStringList{"C", "B", "A"}));
    QCOMPARE(tree.currentItem(), b);
    QVERIFY(b->isSelected());
    QVERIFY(c->isExpanded());
    if (tree.verticalScrollBar()) {
        QCOMPARE(tree.verticalScrollBar()->value(), scroll);
    }
    QCOMPARE(currentSpy.count(), 0); // 重排不得产生瞬态选中变化通知
    Q_UNUSED(a)
}

void TestProcessTreeOrder::testUnknownIdsAreAppendedAndMissingNodesAreKept() {
    QTreeWidget tree;
    tree.setColumnCount(1);
    makeItem(&tree, QStringLiteral("A"));
    makeItem(&tree, QStringLiteral("B"));
    makeItem(&tree, QStringLiteral("C"));
    ProcessTreeController controller(&tree);

    // 计划只列出 C、A；B 不在计划中，防御性追加末尾；计划中的 X 不产生节点。
    controller.applyExecutionOrder({"C", "X", "A"});

    QCOMPARE(treeOrder(tree), (QStringList{"C", "A", "B"}));
    QCOMPARE(tree.topLevelItemCount(), 3);
    QCOMPARE(tree.topLevelItem(0)->data(0, Qt::UserRole + 6).toInt(), 0);
    QCOMPARE(tree.topLevelItem(1)->data(0, Qt::UserRole + 6).toInt(), 1);
    QCOMPARE(tree.topLevelItem(2)->data(0, Qt::UserRole + 6).toInt(), 2);
}

void TestProcessTreeOrder::testEmptyPlanKeepsAllItems() {
    QTreeWidget tree;
    tree.setColumnCount(1);
    makeItem(&tree, QStringLiteral("A"));
    makeItem(&tree, QStringLiteral("B"));
    ProcessTreeController controller(&tree);

    controller.applyExecutionOrder({});

    QCOMPARE(treeOrder(tree), (QStringList{"A", "B"}));
    QCOMPARE(tree.topLevelItemCount(), 2);
}

void TestProcessTreeOrder::testDuplicateAndUnknownPlanIdsDoNotDuplicateNodes() {
    QTreeWidget tree;
    tree.setColumnCount(1);
    makeItem(&tree, QStringLiteral("A"));
    makeItem(&tree, QStringLiteral("B"));
    ProcessTreeController controller(&tree);

    controller.applyExecutionOrder({"B", "B", "ghost", "A"});

    QCOMPARE(treeOrder(tree), (QStringList{"B", "A"}));
    QCOMPARE(tree.topLevelItemCount(), 2);
}

QTEST_MAIN(TestProcessTreeOrder)
#include "test_processtreeorder.moc"
