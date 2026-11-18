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
const FlowOrderNode* nodeOf(const FlowOrderPlan& plan, const QString& id) {
    for (const FlowOrderNode& node : plan.nodes) {
        if (node.instanceId == id)
            return &node;
    }
    return nullptr;
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
    void testMixedGraphIsolatedModulesKeepDefaultRank() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B"), mod("C"), mod("D")};
        const QList<ModuleConnection> edges{edge("A", "B")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B", "C", "D"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        // 已连接节点的 rank 不受影响
        QCOMPARE(plan.rankOf("A"), 0);
        QCOMPARE(plan.rankOf("B"), 1);
        // 混合图中的孤立节点保持默认 rank 0（初始化链仅用于全无连接工程）
        QCOMPARE(plan.rankOf("C"), 0);
        QCOMPARE(plan.rankOf("D"), 0);
        // dataOrder 与 displayOrder 均保持 FIFO 语义
        QCOMPARE(plan.dataOrder, (QStringList{"A", "C", "D", "B"}));
        QCOMPARE(plan.displayOrder, plan.dataOrder);
        // rank 沿 displayOrder 单调不减
        for (int i = 1; i < plan.displayOrder.size(); ++i) {
            QVERIFY(plan.rankOf(plan.displayOrder.at(i - 1)) <= plan.rankOf(plan.displayOrder.at(i)));
        }
    }

    // B1 follow-up: displayOrder 控制分支优先级
    void testConditionalTrueBeforeFalseAndParents() {
        const QList<FlowGraphModule> modules{controlModule("if", ControlFlowType::Conditional, {"true", "false"}),
                                             mod("then"), mod("otherwise")};
        const QList<ModuleConnection> edges{edge("if", "then", "true", "control", "control"),
                                            edge("if", "otherwise", "false", "control", "control")};
        const FlowOrderPlan plan = planFor(modules, edges, {"if", "then", "otherwise"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QVERIFY(plan.displayOrder.indexOf("then") < plan.displayOrder.indexOf("otherwise"));
        const FlowOrderNode* thenNode = nodeOf(plan, "then");
        const FlowOrderNode* elseNode = nodeOf(plan, "otherwise");
        QVERIFY(thenNode && elseNode);
        QCOMPARE(thenNode->branchLabel, QStringLiteral("true"));
        QCOMPARE(thenNode->controlParent, QStringLiteral("if"));
        QCOMPARE(elseNode->branchLabel, QStringLiteral("false"));
        QCOMPARE(elseNode->controlParent, QStringLiteral("if"));
    }
    void testLoopBodyChainContinuity() {
        const QList<FlowGraphModule> modules{controlModule("loop", ControlFlowType::Loop, {"body", "done"}),
                                             mod("body"), mod("body2"), mod("after")};
        const QList<ModuleConnection> edges{edge("loop", "body", "body", "control", "control"),
                                            edge("body", "body2", "next", "control", "control"),
                                            edge("loop", "after", "done", "control", "control")};
        const FlowOrderPlan plan = planFor(modules, edges, {"loop", "body", "body2", "after"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QCOMPARE(plan.displayOrder, (QStringList{"loop", "body", "body2", "after"}));
        QCOMPARE(nodeOf(plan, "body")->branchLabel, QStringLiteral("body"));
        QCOMPARE(nodeOf(plan, "body")->controlParent, QStringLiteral("loop"));
        QCOMPARE(nodeOf(plan, "body2")->branchLabel, QString());
        QCOMPARE(nodeOf(plan, "body2")->controlParent, QString());
        QCOMPARE(nodeOf(plan, "after")->branchLabel, QStringLiteral("done"));
        QCOMPARE(nodeOf(plan, "after")->controlParent, QStringLiteral("loop"));
    }
    void testParallelBranchDeclarationOrder() {
        const QList<FlowGraphModule> modules{controlModule("par", ControlFlowType::Parallel, {"branch"}), mod("p1"),
                                             mod("p2")};
        const FlowOrderPlan first = planFor(
            modules,
            {edge("par", "p1", "branch", "control", "control"), edge("par", "p2", "branch", "control", "control")},
            {"par", "p1", "p2"});
        QVERIFY2(first.valid, qPrintable(first.error));
        QVERIFY(first.displayOrder.indexOf("p1") < first.displayOrder.indexOf("p2"));
        QCOMPARE(nodeOf(first, "p1")->branchLabel, QStringLiteral("branch"));
        QCOMPARE(nodeOf(first, "p2")->controlParent, QStringLiteral("par"));

        const FlowOrderPlan second = planFor(
            modules,
            {edge("par", "p2", "branch", "control", "control"), edge("par", "p1", "branch", "control", "control")},
            {"par", "p1", "p2"});
        QVERIFY2(second.valid, qPrintable(second.error));
        QVERIFY(second.displayOrder.indexOf("p2") < second.displayOrder.indexOf("p1"));
    }
    void testNestedBranchOrderingAndPath() {
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
        QCOMPARE(plan.displayOrder,
                 (QStringList{"outer", "outerTrue", "inner", "innerTrue", "innerFalse", "outerFalse"}));
        QCOMPARE(nodeOf(plan, "innerTrue")->branchLabel, QStringLiteral("true"));
        QCOMPARE(nodeOf(plan, "innerTrue")->controlParent, QStringLiteral("inner"));
        QCOMPARE(nodeOf(plan, "innerFalse")->branchLabel, QStringLiteral("false"));
    }
    void testOrdinaryNextEdgeHasNoBranchLabel() {
        const QList<FlowGraphModule> modules{mod("A"), mod("B")};
        const QList<ModuleConnection> edges{edge("A", "B", "next", "control", "control")};
        const FlowOrderPlan plan = planFor(modules, edges, {"A", "B"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QCOMPARE(nodeOf(plan, "B")->branchLabel, QString());
        QCOMPARE(nodeOf(plan, "B")->controlParent, QString());
        QCOMPARE(plan.displayOrder, (QStringList{"A", "B"}));
    }
    void testLaneAllocationMainChainAndUnique() {
        const QList<FlowGraphModule> modules{controlModule("if", ControlFlowType::Conditional, {"true", "false"}),
                                             mod("then"), mod("otherwise"), mod("root"), mod("main")};
        const QList<ModuleConnection> edges{edge("if", "then", "true", "control", "control"),
                                            edge("if", "otherwise", "false", "control", "control"),
                                            edge("root", "main")};
        const FlowOrderPlan plan = planFor(modules, edges, {"if", "then", "otherwise", "root", "main"});
        QVERIFY2(plan.valid, qPrintable(plan.error));
        QCOMPARE(nodeOf(plan, "main")->lane, 0);
        QVERIFY(nodeOf(plan, "then")->lane != nodeOf(plan, "otherwise")->lane);

        QMap<int, QSet<int>> lanesByRank;
        for (const FlowOrderNode& node : plan.nodes) {
            QVERIFY2(!lanesByRank[node.rank].contains(node.lane), "duplicate lane within rank");
            lanesByRank[node.rank].insert(node.lane);
        }
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

    // ===== B3.2：候选构造与只读校验 =====
    void testBuildReorderedCandidateNoConnectionsAllowsAnyPermutation();
    void testBuildReorderedCandidateReconnectsSimpleChain();
    void testBuildReorderedCandidateRejectsMultiPortPattern();
    void testBuildReorderedCandidateRejectsBranchAndMerge();
    void testValidateCandidateAcceptsUnconnectedAndReconnectedChains();
    void testValidateCandidateRejectsInvalidCandidateWithoutTouchingProject();
    void testValidateCandidateRejectsBranchMergeAndMixedPortPatterns();
    void testValidateCandidateDetectsMissingModulesAndDuplicateEdges();
};

namespace {

void fillProject(Project& project, const QStringList& ids, const QList<ModuleConnection>& connections) {
    for (const QString& id : ids) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = QStringLiteral("module.") + id;
        instance.name = id;
        project.addModule(instance);
    }
    project.setConnections(connections);
}

FlowGraphModule portedModule(const QString& id, const QStringList& inputs, const QStringList& outputs) {
    FlowGraphModule module = mod(id);
    for (const QString& portId : inputs) {
        PortSpec port;
        port.id = portId;
        port.displayName = portId;
        port.type = DataType::Image2D;
        module.inputPorts.append(port);
    }
    for (const QString& portId : outputs) {
        PortSpec port;
        port.id = portId;
        port.displayName = portId;
        port.type = DataType::Image2D;
        module.outputPorts.append(port);
    }
    return module;
}

bool candidateValid(const QList<FlowGraphModule>& modules, const FlowGraphCandidate& candidate, FlowOrderPlan* plan,
                    QString* error) {
    return FlowGraphAnalyzer::validateCandidate(catalog(modules), candidate, plan, error);
}

} // namespace

void TestFlowGraphAnalyzer::testBuildReorderedCandidateNoConnectionsAllowsAnyPermutation() {
    Project project;
    fillProject(project, {"A", "B", "C"}, {});
    QString error;
    FlowGraphCandidate candidate;
    QVERIFY2(FlowGraphAnalyzer::buildReorderedCandidate(project, {"C", "A", "B"}, &candidate, &error),
             qPrintable(error));
    QCOMPARE(error, QString());
    QCOMPARE(candidate.orderedIds, (QStringList{"C", "A", "B"}));
    QVERIFY(candidate.connections.isEmpty());

    FlowOrderPlan plan;
    QVERIFY2(candidateValid({mod("A"), mod("B"), mod("C")}, candidate, &plan, &error), qPrintable(error));
    QVERIFY(plan.valid);
    QCOMPARE(plan.dataOrder, (QStringList{"C", "A", "B"}));
    QCOMPARE(plan.displayOrder, (QStringList{"C", "A", "B"}));
}

void TestFlowGraphAnalyzer::testBuildReorderedCandidateReconnectsSimpleChain() {
    Project project;
    fillProject(project, {"A", "B", "C"}, {edge("A", "B"), edge("B", "C")});
    QString error;
    FlowGraphCandidate candidate;
    QVERIFY2(FlowGraphAnalyzer::buildReorderedCandidate(project, {"C", "A", "B"}, &candidate, &error),
             qPrintable(error));
    QCOMPARE(candidate.orderedIds, (QStringList{"C", "A", "B"}));
    QCOMPARE(candidate.connections.size(), 2);
    QCOMPARE(candidate.connections.at(0).fromModuleId, QStringLiteral("C"));
    QCOMPARE(candidate.connections.at(0).toModuleId, QStringLiteral("A"));
    QCOMPARE(candidate.connections.at(1).fromModuleId, QStringLiteral("A"));
    QCOMPARE(candidate.connections.at(1).toModuleId, QStringLiteral("B"));
    for (const ModuleConnection& connection : candidate.connections) {
        QCOMPARE(connection.fromPort, QStringLiteral("image"));
        QCOMPARE(connection.toPort, QStringLiteral("image"));
        QCOMPARE(connection.edgeType, QStringLiteral("data"));
    }

    QList<FlowGraphModule> modules{portedModule("A", {"image"}, {"image"}), portedModule("B", {"image"}, {"image"}),
                                   portedModule("C", {"image"}, {"image"})};
    FlowOrderPlan plan;
    QVERIFY2(candidateValid(modules, candidate, &plan, &error), qPrintable(error));
    QVERIFY(plan.valid);
    QCOMPARE(plan.dataOrder, (QStringList{"C", "A", "B"}));
    QCOMPARE(plan.displayOrder, (QStringList{"C", "A", "B"}));
    // 原工程未被修改（校验与构造只读）。
    Project reference;
    fillProject(reference, {"A", "B", "C"}, {edge("A", "B"), edge("B", "C")});
    QCOMPARE(project.structureFingerprint(), reference.structureFingerprint());
}

void TestFlowGraphAnalyzer::testBuildReorderedCandidateRejectsMultiPortPattern() {
    Project project;
    fillProject(project, {"A", "B", "C"}, {edge("A", "B", "image", "image"), edge("B", "C", "result", "points")});
    QString error;
    FlowGraphCandidate candidate;
    QVERIFY(!FlowGraphAnalyzer::buildReorderedCandidate(project, {"C", "A", "B"}, &candidate, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(candidate.orderedIds.isEmpty());
}

void TestFlowGraphAnalyzer::testBuildReorderedCandidateRejectsBranchAndMerge() {
    Project branch;
    fillProject(branch, {"A", "B", "C"}, {edge("A", "B"), edge("A", "C")});
    QString error;
    FlowGraphCandidate candidate;
    QVERIFY(!FlowGraphAnalyzer::buildReorderedCandidate(branch, {"C", "A", "B"}, &candidate, &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    Project diamond;
    fillProject(diamond, {"A", "B", "C", "D"}, {edge("A", "B"), edge("A", "C"), edge("B", "D"), edge("C", "D")});
    QVERIFY(!FlowGraphAnalyzer::buildReorderedCandidate(diamond, {"D", "C", "B", "A"}, &candidate, &error));
    QVERIFY(!error.isEmpty());
}

void TestFlowGraphAnalyzer::testValidateCandidateAcceptsUnconnectedAndReconnectedChains() {
    // 无连接顺序列表：任意重排合法。
    QString error;
    FlowGraphCandidate unconnected;
    unconnected.orderedIds = QStringList{"B", "C", "A"};
    FlowOrderPlan plan;
    QVERIFY2(candidateValid({mod("A"), mod("B"), mod("C")}, unconnected, &plan, &error), qPrintable(error));
    QCOMPARE(plan.displayOrder, (QStringList{"B", "C", "A"}));

    // 已连接简单链：同端口模式重连合法。
    FlowGraphCandidate reconnected;
    reconnected.orderedIds = QStringList{"B", "A", "C"};
    reconnected.connections = {edge("B", "A"), edge("A", "C")};
    QList<FlowGraphModule> modules{portedModule("A", {"image"}, {"image"}), portedModule("B", {"image"}, {"image"}),
                                   portedModule("C", {"image"}, {"image"})};
    QVERIFY2(candidateValid(modules, reconnected, &plan, &error), qPrintable(error));
    QCOMPARE(plan.dataOrder, (QStringList{"B", "A", "C"}));
}

void TestFlowGraphAnalyzer::testValidateCandidateRejectsInvalidCandidateWithoutTouchingProject() {
    Project project;
    fillProject(project, {"A", "B", "C"}, {edge("A", "B"), edge("B", "C")});
    const QString fingerprintBefore = project.structureFingerprint();
    const QJsonObject jsonBefore = project.toJson();

    QList<FlowGraphModule> modules{portedModule("A", {"image"}, {"image"}), portedModule("B", {"image"}, {"image"}),
                                   portedModule("C", {"image"}, {"image"})};
    QString error;
    FlowOrderPlan plan;

    // 缺模块（非完整排列）
    FlowGraphCandidate incomplete;
    incomplete.orderedIds = QStringList{"A", "B"};
    QVERIFY(!candidateValid(modules, incomplete, &plan, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!plan.valid);

    // 重复 id
    error.clear();
    FlowGraphCandidate duplicated;
    duplicated.orderedIds = QStringList{"A", "A", "B"};
    QVERIFY(!candidateValid(modules, duplicated, &plan, &error));
    QVERIFY(!error.isEmpty());

    // 未知模块
    error.clear();
    FlowGraphCandidate unknown;
    unknown.orderedIds = QStringList{"A", "B", "ghost"};
    QVERIFY(!candidateValid(modules, unknown, &plan, &error));
    QVERIFY(!error.isEmpty());

    // 端口不存在
    error.clear();
    FlowGraphCandidate missingPort;
    missingPort.orderedIds = QStringList{"A", "B", "C"};
    missingPort.connections = {edge("A", "B", "image", "unknown"), edge("B", "C")};
    QVERIFY(!candidateValid(modules, missingPort, &plan, &error));
    QVERIFY(!error.isEmpty());

    QCOMPARE(project.structureFingerprint(), fingerprintBefore);
    QCOMPARE(project.toJson(), jsonBefore);
}

void TestFlowGraphAnalyzer::testValidateCandidateRejectsBranchMergeAndMixedPortPatterns() {
    QList<FlowGraphModule> modules{portedModule("A", {"image"}, {"image", "result"}),
                                   portedModule("B", {"image", "points"}, {"image"}),
                                   portedModule("C", {"image"}, {"image"}), portedModule("D", {"image"}, {"image"})};
    QString error;
    FlowOrderPlan plan;

    // 分支/汇合
    FlowGraphCandidate diamond;
    diamond.orderedIds = QStringList{"A", "B", "C", "D"};
    diamond.connections = {edge("A", "B"), edge("A", "C"), edge("B", "D"), edge("C", "D")};
    QVERIFY(!candidateValid(modules, diamond, &plan, &error));
    QVERIFY(!error.isEmpty());

    // 多端口模式（同一链上出现两种端口组合）
    error.clear();
    QList<FlowGraphModule> three{portedModule("A", {"image"}, {"image", "result"}),
                                 portedModule("B", {"image", "points"}, {"image", "result"}),
                                 portedModule("C", {"image", "points"}, {"image"})};
    FlowGraphCandidate mixed;
    mixed.orderedIds = QStringList{"A", "B", "C"};
    mixed.connections = {edge("A", "B", "image", "image"), edge("B", "C", "result", "points")};
    QVERIFY(!candidateValid(three, mixed, &plan, &error));
    QVERIFY(!error.isEmpty());
}

void TestFlowGraphAnalyzer::testValidateCandidateDetectsMissingModulesAndDuplicateEdges() {
    QList<FlowGraphModule> modules{portedModule("A", {"image"}, {"image"}), portedModule("B", {"image"}, {"image"}),
                                   portedModule("C", {"image"}, {"image"})};
    QString error;
    FlowOrderPlan plan;

    FlowGraphCandidate duplicateEdges;
    duplicateEdges.orderedIds = QStringList{"A", "B", "C"};
    duplicateEdges.connections = {edge("A", "B"), edge("A", "B"), edge("B", "C")};
    QVERIFY(!candidateValid(modules, duplicateEdges, &plan, &error));
    QVERIFY(!error.isEmpty());

    // 控制边混入重排候选：线性链端口模式有歧义，拒绝。
    error.clear();
    QList<FlowGraphModule> controlModules{controlModule("A", ControlFlowType::Conditional, {"true", "false"}), mod("B"),
                                          mod("C")};
    FlowGraphCandidate withControl;
    withControl.orderedIds = QStringList{"A", "B", "C"};
    withControl.connections = {edge("A", "B", "true", "control", "control")};
    QVERIFY(!candidateValid(controlModules, withControl, &plan, &error));
    QVERIFY(!error.isEmpty());

    // 前向图成环
    error.clear();
    FlowGraphCandidate cycle;
    cycle.orderedIds = QStringList{"A", "B", "C"};
    cycle.connections = {edge("A", "B"), edge("B", "C"), edge("C", "A")};
    QVERIFY(!candidateValid(modules, cycle, &plan, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!plan.valid);
}

QTEST_MAIN(TestFlowGraphAnalyzer)
#include "test_flowgraphanalyzer.moc"
