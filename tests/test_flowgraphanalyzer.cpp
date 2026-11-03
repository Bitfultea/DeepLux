#define DEEPLUX_BUILD_TESTS 1
#include "core/engine/FlowGraphAnalyzer.h"
#include "core/model/Project.h"

#include <QtTest/QtTest>

using namespace DeepLux;

namespace {
FlowGraphModule mod(const QString& id, ControlFlowType type = ControlFlowType::Sequential) {
    FlowGraphModule m;
    m.instanceId = id;
    m.flowType = type;
    return m;
}
ModuleConnection edge(const QString& from, const QString& to, const QString& fromPort = QStringLiteral("image"),
                      const QString& toPort = QStringLiteral("image"), const QString& type = QStringLiteral("data")) {
    ModuleConnection c;
    c.fromModuleId = from;
    c.toModuleId = to;
    c.fromPort = fromPort;
    c.toPort = toPort;
    c.edgeType = type;
    return c;
}
FlowGraphCatalog catalog(const QList<FlowGraphModule>& modules) {
    return FlowGraphCatalog{modules};
}
FlowOrderPlan planFor(const QList<FlowGraphModule>& modules, const QList<ModuleConnection>& edges,
                      const QStringList& order) {
    const FlowGraphCatalog c = catalog(modules);
    QString error;
    const QList<ModuleConnection> normalized = FlowGraphAnalyzer::normalizeConnections(c, edges, &error);
    return FlowGraphAnalyzer::analyze(c, normalized, order);
}
FlowGraphModule controlModule(const QString& id, ControlFlowType type, const QStringList& controlOutputs) {
    FlowGraphModule m = mod(id, type);
    for (const QString& outputId : controlOutputs) {
        PortSpec port;
        port.id = outputId;
        port.displayName = outputId;
        port.type = DataType::Boolean;
        port.control = true;
        m.outputPorts.append(port);
    }
    return m;
}
QString edgeKey(const ModuleConnection& connection) {
    return connection.fromModuleId + QLatin1Char('\x1f') + connection.toModuleId + QLatin1Char('\x1f') +
           connection.fromPort + QLatin1Char('\x1f') + connection.toPort;
}
} // namespace

class TestFlowGraphAnalyzer : public QObject {
    Q_OBJECT
private slots:
    void testDataOrderKeepsFifoCompatibility() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C")};
        const QList<ModuleConnection> edges{edge("A", "B")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B", "C"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QCOMPARE(plan.dataOrder, (QStringList{"A", "C", "B"}));
    }
    void testDiamondDisplayOrderRespectsDependencies() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C"), mod("D")};
        const QList<ModuleConnection> edges{edge("A", "B"), edge("A", "C"), edge("B", "D"), edge("C", "D")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B", "C", "D"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        const int ia = plan.displayOrder.indexOf("A");
        const int ib = plan.displayOrder.indexOf("B");
        const int ic = plan.displayOrder.indexOf("C");
        const int id = plan.displayOrder.indexOf("D");
        QVERIFY(ia < ib && ia < ic && ib < id && ic < id);
        QCOMPARE(plan.displayOrder.last(), QStringLiteral("D"));
    }
    void testPureControlChainHasDistinctRanks() {
        QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C")};
        const QList<ModuleConnection> edges{edge("A", "B", "next", "control", "control"),
                                            edge("B", "C", "next", "control", "control")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B", "C"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QCOMPARE(plan.rankOf("A"), 0);
        QCOMPARE(plan.rankOf("B"), 1);
        QCOMPARE(plan.rankOf("C"), 2);
        QCOMPARE(plan.laneOf("A"), plan.laneOf("B"));
        QCOMPARE(plan.laneOf("B"), plan.laneOf("C"));
    }
    void testPureDataDisplayEqualsDataOrder() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C")};
        const QList<ModuleConnection> edges{edge("A", "B")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B", "C"});
        QVERIFY(plan.valid);
        QCOMPARE(plan.displayOrder, plan.dataOrder);
    }

    // B1.2
    void testFanOutSameRankDifferentLane() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C")};
        const QList<ModuleConnection> edges{edge("A", "B"), edge("A", "C")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B", "C"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QCOMPARE(plan.rankOf("B"), 1);
        QCOMPARE(plan.rankOf("C"), 1);
        QVERIFY(plan.laneOf("B") != plan.laneOf("C"));
    }
    void testConditionalBranchesSameRankDifferentLaneAndLabels() {
        const QList<FlowGraphModule> modules{controlModule("if", ControlFlowType::Conditional, {"true", "false"}),
                                             mod("then"), mod("otherwise")};
        const QList<ModuleConnection> edges{edge("if", "then", "true", "control", "control"),
                                            edge("if", "otherwise", "false", "control", "control")};
        const FlowOrderPlan plan = planFor(modules, edges, {"if", "then", "otherwise"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QCOMPARE(plan.rankOf("then"), plan.rankOf("otherwise"));
        QVERIFY(plan.laneOf("then") != plan.laneOf("otherwise"));
        QString thenLabel;
        QString elseLabel;
        for (const FlowOrderNode& node : plan.nodes) {
            if (node.instanceId == QLatin1String("then"))
                thenLabel = node.branchLabel;
            if (node.instanceId == QLatin1String("otherwise"))
                elseLabel = node.branchLabel;
        }
        QCOMPARE(thenLabel, QStringLiteral("true"));
        QCOMPARE(elseLabel, QStringLiteral("false"));
    }
    void testNestedControlRanksIncrease() {
        const QList<FlowGraphModule> modules{controlModule("outer", ControlFlowType::Conditional, {"true", "false"}),
                                             mod("outerTrue"),
                                             controlModule("inner", ControlFlowType::Conditional, {"true", "false"}),
                                             mod("innerTrue"),
                                             mod("innerFalse"),
                                             mod("outerFalse")};
        const QList<ModuleConnection> edges{edge("outer", "outerTrue", "true", "control", "control"),
                                            edge("outer", "outerFalse", "false", "control", "control"),
                                            edge("outerTrue", "inner", "next", "control", "control"),
                                            edge("inner", "innerTrue", "true", "control", "control"),
                                            edge("inner", "innerFalse", "false", "control", "control")};
        const FlowOrderPlan plan =
            planFor(modules, edges, {"outer", "outerTrue", "inner", "innerTrue", "innerFalse", "outerFalse"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QVERIFY(plan.rankOf("outerTrue") > plan.rankOf("outer"));
        QVERIFY(plan.rankOf("inner") > plan.rankOf("outerTrue"));
        QVERIFY(plan.rankOf("innerTrue") > plan.rankOf("inner"));
        QVERIFY(plan.rankOf("innerFalse") > plan.rankOf("inner"));
    }
    void testLoopBackEdgeMetadata() {
        const QList<FlowGraphModule> modules{controlModule("loop", ControlFlowType::Loop, {"body", "done"}),
                                             mod("body"), mod("after")};
        const QList<ModuleConnection> edges{edge("loop", "body", "body", "control", "control"),
                                            edge("body", "loop", "next", "control", "control"),
                                            edge("loop", "after", "done", "control", "control")};
        const FlowOrderPlan plan = planFor(modules, edges, {"loop", "body", "after"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QCOMPARE(plan.loopBackEdges.size(), 1);
        QVERIFY(plan.loopBackEdges.contains(edgeKey(edges.at(1))));
        QVERIFY(plan.reentrantModules.contains("loop"));
        QVERIFY(plan.reentrantModules.contains("body"));
        QVERIFY(plan.loopRegions.value("loop").contains("body"));
        QCOMPARE(plan.rankOf("loop"), 0);
        QCOMPARE(plan.rankOf("body"), 1);
    }
    void testUnstructuredControlCycleRejected() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B")};
        const QList<ModuleConnection> edges{edge("A", "B", "next", "control", "control"),
                                            edge("B", "A", "next", "control", "control")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B"});
        QVERIFY(!plan.valid);
        QVERIFY(!plan.error.isEmpty());
    }
    void testDataCycleRejected() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B")};
        const QList<ModuleConnection> edges{edge("A", "B"), edge("B", "A")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B"});
        QVERIFY(!plan.valid);
        QVERIFY(!plan.error.isEmpty());
    }

    // B1.3
    void testDisplayOrderIsTopologicalWithControlEdges() {
        const QList<FlowGraphModule> modules{controlModule("loop", ControlFlowType::Loop, {"body", "done"}),
                                             mod("body"), mod("after")};
        const QList<ModuleConnection> edges{edge("loop", "body", "body", "control", "control"),
                                            edge("body", "loop", "next", "control", "control"),
                                            edge("loop", "after", "done", "control", "control")};
        const FlowOrderPlan plan = planFor(modules, edges, {"loop", "body", "after"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        const int loop = plan.displayOrder.indexOf("loop");
        const int body = plan.displayOrder.indexOf("body");
        const int after = plan.displayOrder.indexOf("after");
        QVERIFY(loop >= 0 && body >= 0 && after >= 0);
        QVERIFY(loop < body);
        QVERIFY(loop < after);
    }
    void testConnectionDeclarationOrderChangesDataOrder() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C")};
        const FlowOrderPlan first = planFor(modules, {edge("A", "B"), edge("A", "C")}, {"A", "B", "C"});
        const FlowOrderPlan second = planFor(modules, {edge("A", "C"), edge("A", "B")}, {"A", "B", "C"});
        QVERIFY(first.valid);
        QVERIFY(second.valid);
        QCOMPARE(first.dataOrder, (QStringList{"A", "B", "C"}));
        QCOMPARE(second.dataOrder, (QStringList{"A", "C", "B"}));
    }
    void testEmptyConnectionsDataOrderEqualsModuleOrder() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C")};
        const FlowOrderPlan plan = planFor(modules, {}, {"A", "B", "C"});
        QVERIFY(plan.valid);
        QCOMPARE(plan.dataOrder, (QStringList{"A", "B", "C"}));
        QCOMPARE(plan.displayOrder, plan.dataOrder);
    }
    void testUnconnectedModulesFormInitializationChain() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C")};
        const FlowOrderPlan plan = planFor(modules, {}, {"A", "B", "C"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QCOMPARE(plan.rankOf("A"), 0);
        QCOMPARE(plan.rankOf("B"), 1);
        QCOMPARE(plan.rankOf("C"), 2);
        QCOMPARE(plan.displayOrder, (QStringList{"A", "B", "C"}));
        QCOMPARE(plan.laneOf("A"), 0);
        QCOMPARE(plan.laneOf("B"), 0);
        QCOMPARE(plan.laneOf("C"), 0);
    }
    void testMixedGraphAppendsUnconnectedModulesAfterConnectedRanks() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C"), mod("D")};
        const QList<ModuleConnection> edges{edge("A", "B")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B", "C", "D"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        // 已连接节点的 rank 不受影响
        QCOMPARE(plan.rankOf("A"), 0);
        QCOMPARE(plan.rankOf("B"), 1);
        // 无连接节点按 Project 稳定顺序接在最大已连接 rank 之后
        QCOMPARE(plan.rankOf("C"), 2);
        QCOMPARE(plan.rankOf("D"), 3);
        // dataOrder 的 FIFO 语义保持不变
        QCOMPARE(plan.dataOrder, (QStringList{"A", "C", "D", "B"}));
        // displayOrder 仍是包含全部节点的合法前向拓扑序
        QCOMPARE(plan.displayOrder.size(), 4);
        QVERIFY(plan.displayOrder.indexOf("A") < plan.displayOrder.indexOf("B"));
        QSet<QString> seen;
        for (const QString& id : plan.displayOrder)
            seen.insert(id);
        QCOMPARE(seen.size(), 4);
        QVERIFY(seen.contains("C"));
        QVERIFY(seen.contains("D"));
    }

    // normalizeConnections 规则
    void testNormalizeInfersControlFromImplicitAndDeclaredPorts() {
        QList<FlowGraphModule> modules{controlModule("if", ControlFlowType::Conditional, {"true"}), mod("target")};
        const FlowGraphCatalog c = catalog(modules);
        QString error;
        const QList<ModuleConnection> normalized = FlowGraphAnalyzer::normalizeConnections(
            c, {edge("if", "target", "true", "control", QString()), edge("if", "target", "next", "data", QString())},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(normalized.size(), 2);
        QCOMPARE(normalized.at(0).edgeType, QStringLiteral("control"));
        QCOMPARE(normalized.at(1).edgeType, QStringLiteral("control"));
    }
    void testNormalizePreservesExplicitEdgeType() {
        QList<FlowGraphModule> modules{controlModule("if", ControlFlowType::Conditional, {"true"}), mod("target")};
        const FlowGraphCatalog c = catalog(modules);
        QString error;
        const QList<ModuleConnection> normalized =
            FlowGraphAnalyzer::normalizeConnections(c, {edge("if", "target", "true", "control", "data")}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(normalized.size(), 1);
        QCOMPARE(normalized.first().edgeType, QStringLiteral("data"));
    }
    void testNormalizeRejectsUnknownEdgeType() {
        const FlowGraphCatalog c = catalog({mod("A"), mod("B")});
        QString error;
        const QList<ModuleConnection> normalized =
            FlowGraphAnalyzer::normalizeConnections(c, {edge("A", "B", "image", "image", "weird")}, &error);
        QVERIFY(normalized.isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void testNormalizeDefaultsPortsAndType() {
        const FlowGraphCatalog c = catalog({mod("A"), mod("B")});
        QString error;
        ModuleConnection connection;
        connection.fromModuleId = "A";
        connection.toModuleId = "B";
        const QList<ModuleConnection> normalized = FlowGraphAnalyzer::normalizeConnections(c, {connection}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(normalized.size(), 1);
        QCOMPARE(normalized.first().fromPort, QStringLiteral("image"));
        QCOMPARE(normalized.first().toPort, QStringLiteral("image"));
        QCOMPARE(normalized.first().edgeType, QStringLiteral("data"));
    }
};

QTEST_MAIN(TestFlowGraphAnalyzer)
#include "test_flowgraphanalyzer.moc"
