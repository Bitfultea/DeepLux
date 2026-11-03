#include "FlowGraphAnalyzer.h"

#include <algorithm>
#include <limits>

namespace DeepLux {

namespace {

QString connectionKey(const ModuleConnection& connection) {
    return connection.fromModuleId + QLatin1Char('\x1f') + connection.toModuleId + QLatin1Char('\x1f') +
           connection.fromPort + QLatin1Char('\x1f') + connection.toPort;
}

const PortSpec* findPort(const QList<PortSpec>& ports, const QString& id) {
    for (const PortSpec& port : ports) {
        if (port.id == id)
            return &port;
    }
    return nullptr;
}

// 同一 rank 内主链使用 lane 0，其余分支稳定地取 0, -1, +1, -2, +2 ...
int laneForIndex(int index) {
    if (index == 0)
        return 0;
    const int magnitude = (index + 1) / 2;
    return (index % 2 == 1) ? -magnitude : magnitude;
}

void setError(QString* error, const QString& message) {
    if (error)
        *error = message;
}

} // namespace

const FlowGraphModule* FlowGraphCatalog::find(const QString& instanceId) const {
    for (const FlowGraphModule& module : modules) {
        if (module.instanceId == instanceId)
            return &module;
    }
    return nullptr;
}

int FlowOrderPlan::rankOf(const QString& id) const {
    for (const FlowOrderNode& node : nodes) {
        if (node.instanceId == id)
            return node.rank;
    }
    return -1;
}

int FlowOrderPlan::laneOf(const QString& id) const {
    for (const FlowOrderNode& node : nodes) {
        if (node.instanceId == id)
            return node.lane;
    }
    return -1;
}

QList<ModuleConnection> FlowGraphAnalyzer::normalizeConnections(const FlowGraphCatalog& catalog,
                                                                const QList<ModuleConnection>& connections,
                                                                QString* error) {
    setError(error, QString());
    QList<ModuleConnection> normalized;
    normalized.reserve(connections.size());

    for (ModuleConnection connection : connections) {
        const FlowGraphModule* source = catalog.find(connection.fromModuleId);
        if (!source) {
            setError(error,
                     QStringLiteral("Connection references missing source module: %1").arg(connection.fromModuleId));
            return {};
        }
        const FlowGraphModule* target = catalog.find(connection.toModuleId);
        if (!target) {
            setError(error,
                     QStringLiteral("Connection references missing target module: %1").arg(connection.toModuleId));
            return {};
        }
        if (connection.edgeType == QLatin1String("pending")) {
            setError(error, QStringLiteral("Connection %1 -> %2 requires manual port mapping")
                                .arg(connection.fromModuleId, connection.toModuleId));
            return {};
        }
        if (connection.fromPort.isEmpty())
            connection.fromPort = QStringLiteral("image");
        if (connection.toPort.isEmpty())
            connection.toPort = QStringLiteral("image");

        const PortSpec* output = findPort(source->outputPorts, connection.fromPort);
        const PortSpec* input = findPort(target->inputPorts, connection.toPort);
        // 隐式控制端口：next（所有节点的隐式控制输出）和 control（隐式控制输入）
        const bool fromIsControl = connection.fromPort == QLatin1String("next") || (output && output->control);
        const bool toIsControl = connection.toPort == QLatin1String("control") || (input && input->control);

        if (connection.edgeType.isEmpty())
            connection.edgeType = (fromIsControl || toIsControl) ? QStringLiteral("control") : QStringLiteral("data");

        if (connection.edgeType != QLatin1String("data") && connection.edgeType != QLatin1String("control")) {
            setError(error, QStringLiteral("Unknown edge type '%1' on connection %2.%3 -> %4.%5")
                                .arg(connection.edgeType, connection.fromModuleId, connection.fromPort,
                                     connection.toModuleId, connection.toPort));
            return {};
        }
        normalized.append(connection);
    }
    return normalized;
}

FlowOrderPlan FlowGraphAnalyzer::analyze(const FlowGraphCatalog& catalog,
                                         const QList<ModuleConnection>& normalizedConnections,
                                         const QStringList& moduleOrder) {
    FlowOrderPlan plan;

    QStringList nodeIds;
    QSet<QString> knownNodes;
    for (const QString& id : moduleOrder) {
        if (!id.isEmpty() && !knownNodes.contains(id)) {
            nodeIds.append(id);
            knownNodes.insert(id);
        }
    }
    for (const FlowGraphModule& module : catalog.modules) {
        if (!knownNodes.contains(module.instanceId)) {
            nodeIds.append(module.instanceId);
            knownNodes.insert(module.instanceId);
        }
    }

    QMap<QString, ControlFlowType> flowTypes;
    for (const FlowGraphModule& module : catalog.modules)
        flowTypes.insert(module.instanceId, module.flowType);

    // ---------- dataOrder：保持旧引擎的 FIFO 拓扑排序（仅数据边参与） ----------
    QMap<QString, QStringList> dataAdjacency;
    QMap<QString, int> dataIndegree;
    for (const QString& id : nodeIds)
        dataIndegree.insert(id, 0);
    for (const ModuleConnection& connection : normalizedConnections) {
        if (connection.edgeType != QLatin1String("data"))
            continue;
        dataAdjacency[connection.fromModuleId].append(connection.toModuleId);
        ++dataIndegree[connection.toModuleId];
    }

    QStringList ready;
    for (const QString& id : nodeIds) {
        if (dataIndegree.value(id) == 0)
            ready.append(id);
    }
    QSet<QString> emitted;
    while (!ready.isEmpty()) {
        const QString current = ready.takeFirst();
        plan.dataOrder.append(current);
        emitted.insert(current);
        for (const QString& downstream : dataAdjacency.value(current)) {
            if (--dataIndegree[downstream] == 0)
                ready.append(downstream);
        }
    }
    if (plan.dataOrder.size() != nodeIds.size()) {
        // 防御性补齐：排序结果始终覆盖全部节点，但计划标记为无效。
        for (const QString& id : nodeIds) {
            if (!emitted.contains(id))
                plan.dataOrder.append(id);
        }
        plan.valid = false;
        plan.error = QStringLiteral("Data-flow graph contains a cycle");
        return plan;
    }

    for (const ModuleConnection& connection : normalizedConnections) {
        if (connection.edgeType == QLatin1String("control"))
            plan.controlEdges.append(connection);
    }

    // ---------- 回边检测：与旧引擎可达性规则一致 ----------
    struct CombinedEdge {
        QString target;
        bool control = false;
    };
    QMap<QString, QList<CombinedEdge>> combinedAdjacency;
    for (const ModuleConnection& connection : normalizedConnections) {
        combinedAdjacency[connection.fromModuleId].append(
            {connection.toModuleId, connection.edgeType == QLatin1String("control")});
    }

    const auto reachable = [&combinedAdjacency](QStringList pending, const QString& excludedLoopTarget) {
        QSet<QString> reached;
        while (!pending.isEmpty()) {
            const QString current = pending.takeFirst();
            if (reached.contains(current))
                continue;
            reached.insert(current);
            for (const CombinedEdge& edge : combinedAdjacency.value(current)) {
                if (edge.control && edge.target == excludedLoopTarget)
                    continue;
                pending.append(edge.target);
            }
        }
        return reached;
    };

    for (const ModuleConnection& candidate : plan.controlEdges) {
        const ControlFlowType targetType = flowTypes.value(candidate.toModuleId, ControlFlowType::Sequential);
        if (candidate.fromModuleId == candidate.toModuleId ||
            (targetType != ControlFlowType::Loop && targetType != ControlFlowType::While)) {
            continue;
        }
        QStringList bodyStarts;
        QStringList doneStarts;
        for (const ModuleConnection& edge : plan.controlEdges) {
            if (edge.fromModuleId != candidate.toModuleId)
                continue;
            if (edge.fromPort == QLatin1String("body"))
                bodyStarts.append(edge.toModuleId);
            else if (edge.fromPort == QLatin1String("done"))
                doneStarts.append(edge.toModuleId);
        }
        if (reachable(bodyStarts, candidate.toModuleId).contains(candidate.fromModuleId) &&
            !reachable(doneStarts, candidate.toModuleId).contains(candidate.fromModuleId)) {
            plan.loopBackEdges.insert(connectionKey(candidate));
        }
    }

    QMap<QString, QStringList> reducedAdjacency;
    QMap<QString, QStringList> reducedReverse;
    QMap<QString, int> combinedIndegree;
    for (const QString& id : nodeIds)
        combinedIndegree.insert(id, 0);
    for (const ModuleConnection& connection : normalizedConnections) {
        if (plan.loopBackEdges.contains(connectionKey(connection)))
            continue;
        reducedAdjacency[connection.fromModuleId].append(connection.toModuleId);
        reducedReverse[connection.toModuleId].append(connection.fromModuleId);
        ++combinedIndegree[connection.toModuleId];
    }

    ready.clear();
    for (const QString& id : nodeIds) {
        if (combinedIndegree.value(id) == 0)
            ready.append(id);
    }
    int visitedCount = 0;
    while (!ready.isEmpty()) {
        const QString current = ready.takeFirst();
        ++visitedCount;
        for (const QString& downstream : reducedAdjacency.value(current)) {
            if (--combinedIndegree[downstream] == 0)
                ready.append(downstream);
        }
    }
    if (visitedCount != nodeIds.size()) {
        plan.valid = false;
        plan.error = QStringLiteral("Control or combined graph contains an unstructured cycle");
        return plan;
    }

    const auto reachableIn = [](const QMap<QString, QStringList>& graph, QStringList pending) {
        QSet<QString> reached;
        while (!pending.isEmpty()) {
            const QString current = pending.takeFirst();
            if (reached.contains(current))
                continue;
            reached.insert(current);
            pending.append(graph.value(current));
        }
        return reached;
    };
    for (const ModuleConnection& backEdge : plan.controlEdges) {
        if (!plan.loopBackEdges.contains(connectionKey(backEdge)))
            continue;
        QStringList bodyStarts;
        for (const ModuleConnection& edge : plan.controlEdges) {
            if (edge.fromModuleId == backEdge.toModuleId && edge.fromPort == QLatin1String("body"))
                bodyStarts.append(edge.toModuleId);
        }
        const QSet<QString> bodyReach = reachableIn(reducedAdjacency, bodyStarts);
        const QSet<QString> reachesBackSource = reachableIn(reducedReverse, {backEdge.fromModuleId});
        plan.reentrantModules.insert(backEdge.toModuleId);
        for (const QString& moduleId : bodyReach) {
            if (reachesBackSource.contains(moduleId)) {
                plan.reentrantModules.insert(moduleId);
                plan.loopRegions[backEdge.toModuleId].insert(moduleId);
            }
        }
    }

    // ---------- 前向依赖 rank（数据边 + 控制前向边，回边已剔除） ----------
    QMap<QString, int> rank;
    QMap<QString, int> rankIndegree;
    for (const QString& id : nodeIds) {
        rank.insert(id, 0);
        rankIndegree.insert(id, 0);
    }
    for (const ModuleConnection& connection : normalizedConnections) {
        if (plan.loopBackEdges.contains(connectionKey(connection)))
            continue;
        ++rankIndegree[connection.toModuleId];
    }
    ready.clear();
    for (const QString& id : nodeIds) {
        if (rankIndegree.value(id) == 0)
            ready.append(id);
    }
    while (!ready.isEmpty()) {
        const QString current = ready.takeFirst();
        for (const QString& downstream : reducedAdjacency.value(current)) {
            rank[downstream] = std::max(rank.value(downstream), rank.value(current) + 1);
            if (--rankIndegree[downstream] == 0)
                ready.append(downstream);
        }
    }

    // ---------- 无连接节点：按 Project 稳定顺序形成展示用初始化链 ----------
    QSet<QString> incidentNodes;
    for (const ModuleConnection& connection : normalizedConnections) {
        incidentNodes.insert(connection.fromModuleId);
        incidentNodes.insert(connection.toModuleId);
    }
    QStringList isolatedIds;
    for (const QString& id : nodeIds) {
        if (!incidentNodes.contains(id))
            isolatedIds.append(id);
    }
    if (!isolatedIds.isEmpty()) {
        if (normalizedConnections.isEmpty()) {
            // 整图无任何连接：rank=0..N-1，仅用于展示。
            for (int i = 0; i < nodeIds.size(); ++i)
                rank[nodeIds.at(i)] = i;
        } else {
            // 混合图：无连接节点接在已连接子图最大 rank 之后，保持 Project 稳定顺序。
            int maxConnectedRank = -1;
            for (const QString& id : nodeIds) {
                if (incidentNodes.contains(id))
                    maxConnectedRank = std::max(maxConnectedRank, rank.value(id));
            }
            int nextRank = maxConnectedRank + 1;
            for (const QString& id : isolatedIds)
                rank[id] = nextRank++;
        }
    }

    QMap<QString, int> dataOrderIndex;
    for (int i = 0; i < plan.dataOrder.size(); ++i)
        dataOrderIndex.insert(plan.dataOrder.at(i), i);

    QMap<QString, QString> controlParent;
    QMap<QString, QString> branchLabel;
    for (const ModuleConnection& connection : plan.controlEdges) {
        if (!controlParent.contains(connection.toModuleId)) {
            controlParent.insert(connection.toModuleId, connection.fromModuleId);
            branchLabel.insert(connection.toModuleId, connection.fromPort);
        }
    }

    // 统一 lane：同 rank 分组，主链 lane=0，其余分支稳定交替。
    QMap<int, QStringList> byRank;
    for (const QString& id : nodeIds)
        byRank[rank.value(id)].append(id);
    QMap<QString, int> lane;
    for (auto it = byRank.begin(); it != byRank.end(); ++it) {
        QStringList members = it.value();
        std::stable_sort(members.begin(), members.end(), [&dataOrderIndex](const QString& a, const QString& b) {
            return dataOrderIndex.value(a, std::numeric_limits<int>::max()) <
                   dataOrderIndex.value(b, std::numeric_limits<int>::max());
        });
        for (int i = 0; i < members.size(); ++i)
            lane.insert(members.at(i), laneForIndex(i));
    }

    // ---------- displayOrder：有控制边时按前向图做稳定 Kahn ----------
    if (plan.controlEdges.isEmpty()) {
        plan.displayOrder = plan.dataOrder;
    } else {
        QMap<QString, int> displayIndegree;
        for (const QString& id : nodeIds)
            displayIndegree.insert(id, 0);
        for (const ModuleConnection& connection : normalizedConnections) {
            if (plan.loopBackEdges.contains(connectionKey(connection)))
                continue;
            ++displayIndegree[connection.toModuleId];
        }
        QStringList displayReady;
        for (const QString& id : nodeIds) {
            if (displayIndegree.value(id) == 0)
                displayReady.append(id);
        }

        const auto groupingKey = [&controlParent, &branchLabel](const QString& id) {
            const QString parent = controlParent.value(id);
            if (parent.isEmpty())
                return QString();
            return parent + QLatin1Char('\x1f') + branchLabel.value(id);
        };
        const auto higherPriority = [&](const QString& lhs, const QString& rhs) {
            const int lhsRank = rank.value(lhs);
            const int rhsRank = rank.value(rhs);
            if (lhsRank != rhsRank)
                return lhsRank < rhsRank;
            const QString lhsGroup = groupingKey(lhs);
            const QString rhsGroup = groupingKey(rhs);
            if (lhsGroup != rhsGroup)
                return lhsGroup < rhsGroup;
            return dataOrderIndex.value(lhs, std::numeric_limits<int>::max()) <
                   dataOrderIndex.value(rhs, std::numeric_limits<int>::max());
        };

        while (!displayReady.isEmpty()) {
            int best = 0;
            for (int i = 1; i < displayReady.size(); ++i) {
                if (higherPriority(displayReady.at(i), displayReady.at(best)))
                    best = i;
            }
            const QString current = displayReady.takeAt(best);
            plan.displayOrder.append(current);
            for (const QString& downstream : reducedAdjacency.value(current)) {
                if (--displayIndegree[downstream] == 0)
                    displayReady.append(downstream);
            }
        }
    }

    for (int i = 0; i < plan.displayOrder.size(); ++i) {
        const QString& id = plan.displayOrder.at(i);
        FlowOrderNode node;
        node.instanceId = id;
        node.rank = rank.value(id);
        node.lane = lane.value(id);
        node.displayIndex = i;
        node.branchLabel = branchLabel.value(id);
        node.controlParent = controlParent.value(id);
        plan.nodes.append(node);
    }

    plan.valid = true;
    return plan;
}

} // namespace DeepLux
