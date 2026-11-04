#include "FlowGraphAnalyzer.h"

#include <QQueue>
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <utility>

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

bool isOpenerType(ControlFlowType type) {
    return type == ControlFlowType::Conditional || type == ControlFlowType::Loop || type == ControlFlowType::While ||
           type == ControlFlowType::Parallel;
}

bool isBranchPort(ControlFlowType type, const QString& portId) {
    switch (type) {
    case ControlFlowType::Conditional:
        return portId == QLatin1String("true") || portId == QLatin1String("false");
    case ControlFlowType::Loop:
    case ControlFlowType::While:
        return portId == QLatin1String("body") || portId == QLatin1String("done");
    case ControlFlowType::Parallel:
        return portId == QLatin1String("branch");
    default:
        return false;
    }
}

// If true<false；Loop/While body<done；Parallel 使用控制边声明序号。
int staticBranchOrder(ControlFlowType type, const QString& portId) {
    if (type == ControlFlowType::Conditional)
        return portId == QLatin1String("true") ? 0 : 1;
    if (type == ControlFlowType::Loop || type == ControlFlowType::While)
        return portId == QLatin1String("body") ? 0 : 1;
    return -1;
}

QString paddedIndex(int value) {
    return QString::number(value).rightJustified(10, QLatin1Char('0'));
}

// 控制区域路径：父 opener 区域 + 当前 opener 的 displayIndex + 分支顺序。
// 顶层 opener 的父路径为空时仍保留前导 '/'，保证主链空键排在最前，
// 同时嵌套区域以父路径为前缀。
QString childRegionKey(const QString& parentRegion, int openerDisplayIndex, int branchOrder) {
    return parentRegion + QLatin1Char('/') + paddedIndex(openerDisplayIndex) + QLatin1Char(':') +
           paddedIndex(branchOrder);
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
    return rankIndex.value(id, -1);
}

int FlowOrderPlan::laneOf(const QString& id) const {
    return laneIndex.value(id, -1);
}

bool FlowGraphAnalyzer::isControlPort(const QList<PortSpec>& ports, bool source, const QString& portId) {
    if (source && portId == QLatin1String("next"))
        return true;
    if (!source && portId == QLatin1String("control"))
        return true;
    const PortSpec* port = findPort(ports, portId);
    return port && port->control;
}

QList<ModuleConnection> FlowGraphAnalyzer::normalizeConnections(const FlowGraphCatalog& catalog,
                                                                const QList<ModuleConnection>& connections,
                                                                QString* error) {
    setError(error, QString());
    QList<ModuleConnection> normalized;
    normalized.reserve(connections.size());

    QHash<QString, const FlowGraphModule*> moduleIndex;
    moduleIndex.reserve(catalog.modules.size());
    for (const FlowGraphModule& module : catalog.modules) {
        if (!moduleIndex.contains(module.instanceId))
            moduleIndex.insert(module.instanceId, &module);
    }

    for (ModuleConnection connection : connections) {
        const FlowGraphModule* source = moduleIndex.value(connection.fromModuleId, nullptr);
        if (!source) {
            setError(error,
                     QStringLiteral("Connection references missing source module: %1").arg(connection.fromModuleId));
            return {};
        }
        const FlowGraphModule* target = moduleIndex.value(connection.toModuleId, nullptr);
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

        // 隐式控制端口：next（所有节点的隐式控制输出）和 control（隐式控制输入）
        const bool fromIsControl = isControlPort(source->outputPorts, true, connection.fromPort);
        const bool toIsControl = isControlPort(target->inputPorts, false, connection.toPort);

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

    QQueue<QString> ready;
    for (const QString& id : nodeIds) {
        if (dataIndegree.value(id) == 0)
            ready.enqueue(id);
    }
    QSet<QString> emitted;
    while (!ready.isEmpty()) {
        const QString current = ready.dequeue();
        plan.dataOrder.append(current);
        emitted.insert(current);
        for (const QString& downstream : dataAdjacency.value(current)) {
            if (--dataIndegree[downstream] == 0)
                ready.enqueue(downstream);
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

    const auto reachable = [&combinedAdjacency](const QStringList& starts, const QString& excludedLoopTarget) {
        QQueue<QString> pending;
        for (const QString& start : starts)
            pending.enqueue(start);
        QSet<QString> reached;
        while (!pending.isEmpty()) {
            const QString current = pending.dequeue();
            if (reached.contains(current))
                continue;
            reached.insert(current);
            for (const CombinedEdge& edge : combinedAdjacency.value(current)) {
                if (edge.control && edge.target == excludedLoopTarget)
                    continue;
                pending.enqueue(edge.target);
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
            ready.enqueue(id);
    }
    int visitedCount = 0;
    while (!ready.isEmpty()) {
        const QString current = ready.dequeue();
        ++visitedCount;
        for (const QString& downstream : reducedAdjacency.value(current)) {
            if (--combinedIndegree[downstream] == 0)
                ready.enqueue(downstream);
        }
    }
    if (visitedCount != nodeIds.size()) {
        plan.valid = false;
        plan.error = QStringLiteral("Control or combined graph contains an unstructured cycle");
        return plan;
    }

    const auto reachableIn = [](const QMap<QString, QStringList>& graph, const QStringList& starts) {
        QQueue<QString> pending;
        for (const QString& start : starts)
            pending.enqueue(start);
        QSet<QString> reached;
        while (!pending.isEmpty()) {
            const QString current = pending.dequeue();
            if (reached.contains(current))
                continue;
            reached.insert(current);
            for (const QString& next : graph.value(current))
                pending.enqueue(next);
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
            ready.enqueue(id);
    }
    while (!ready.isEmpty()) {
        const QString current = ready.dequeue();
        for (const QString& downstream : reducedAdjacency.value(current)) {
            rank[downstream] = std::max(rank.value(downstream), rank.value(current) + 1);
            if (--rankIndegree[downstream] == 0)
                ready.enqueue(downstream);
        }
    }

    // ---------- 整图无任何连接：按 Project 稳定顺序形成展示用初始化链 ----------
    // 仅用于全无连接工程（§7.3 的无连接顺序列表）；混合图中的孤立节点保持默认 rank 0，
    // 避免 rank 在 displayOrder 上非单调。
    if (normalizedConnections.isEmpty()) {
        for (int i = 0; i < nodeIds.size(); ++i)
            rank[nodeIds.at(i)] = i;
    }

    QMap<QString, int> dataOrderIndex;
    for (int i = 0; i < plan.dataOrder.size(); ++i)
        dataOrderIndex.insert(plan.dataOrder.at(i), i);

    // ---------- 分支标签：只有 opener 的分支端口边参与 ----------
    // 普通顺序控制边（next -> control）不设置 branchLabel/controlParent。
    QHash<QString, QString> controlParent;
    QHash<QString, QString> branchLabel;
    QHash<QString, int> branchOrder;
    QHash<QString, int> parallelBranchCounter;
    for (const ModuleConnection& connection : plan.controlEdges) {
        const ControlFlowType sourceType = flowTypes.value(connection.fromModuleId, ControlFlowType::Sequential);
        if (!isOpenerType(sourceType) || !isBranchPort(sourceType, connection.fromPort))
            continue;
        int order = staticBranchOrder(sourceType, connection.fromPort);
        if (order < 0) // Parallel：按控制边声明顺序
            order = parallelBranchCounter[connection.fromModuleId]++;
        if (controlParent.contains(connection.toModuleId))
            continue;
        controlParent.insert(connection.toModuleId, connection.fromModuleId);
        branchLabel.insert(connection.toModuleId, connection.fromPort);
        branchOrder.insert(connection.toModuleId, order);
    }

    QHash<QString, int> displayIndexById;
    QHash<QString, QString> regionCache;
    // 控制区域路径：分支目标取“父 opener 区域 + 自身分支键”；
    // 普通节点继承前驱中字典序最小的非空区域，使嵌套 opener 的子分支仍位于父区域之下。
    std::function<QString(const QString&)> regionOf = [&](const QString& id) -> QString {
        const auto cached = regionCache.constFind(id);
        if (cached != regionCache.constEnd())
            return cached.value();
        QString region;
        if (branchLabel.contains(id)) {
            const QString opener = controlParent.value(id);
            region = childRegionKey(regionOf(opener), displayIndexById.value(opener, 0), branchOrder.value(id));
        } else {
            for (const QString& predecessor : reducedReverse.value(id)) {
                const QString predecessorRegion = regionOf(predecessor);
                if (!predecessorRegion.isEmpty() && (region.isEmpty() || predecessorRegion < region))
                    region = predecessorRegion;
            }
        }
        regionCache.insert(id, region);
        return region;
    };

    // ---------- displayOrder：有控制边时按前向图做稳定 Kahn ----------
    // ready 优先级 = 控制分支分组键（主链/普通节点为空）> dataOrderIndex。
    // rank 只用于布局，不参与展示优先级。
    if (plan.controlEdges.isEmpty()) {
        plan.displayOrder = plan.dataOrder;
        for (int i = 0; i < plan.displayOrder.size(); ++i)
            displayIndexById.insert(plan.displayOrder.at(i), i);
    } else {
        QMap<QString, int> displayIndegree;
        for (const QString& id : nodeIds)
            displayIndegree.insert(id, 0);
        for (const ModuleConnection& connection : normalizedConnections) {
            if (plan.loopBackEdges.contains(connectionKey(connection)))
                continue;
            ++displayIndegree[connection.toModuleId];
        }

        std::map<std::pair<QString, int>, QString> displayReady;
        const auto enqueueDisplay = [&](const QString& id) {
            const QString groupKey = branchLabel.contains(id) ? regionOf(id) : QString();
            displayReady.emplace(std::make_pair(groupKey, dataOrderIndex.value(id, std::numeric_limits<int>::max())),
                                 id);
        };
        for (const QString& id : nodeIds) {
            if (displayIndegree.value(id) == 0)
                enqueueDisplay(id);
        }

        while (!displayReady.empty()) {
            auto best = displayReady.begin();
            const QString current = best->second;
            displayReady.erase(best);
            plan.displayOrder.append(current);
            displayIndexById.insert(current, plan.displayOrder.size() - 1);
            for (const QString& downstream : reducedAdjacency.value(current)) {
                if (--displayIndegree[downstream] == 0)
                    enqueueDisplay(downstream);
            }
        }
    }

    // ---------- 统一 lane：displayOrder 之后按 rank 分组 ----------
    // 主链（空 branchLabel）先按 dataOrderIndex 取 lane 0，其余分支按
    // (控制区域路径, 分支顺序, dataOrderIndex) 排序后居中分配 0, -1, +1...
    QHash<QString, int> lane;
    QMap<int, QStringList> byRank;
    for (const QString& id : plan.displayOrder)
        byRank[rank.value(id)].append(id);
    for (auto it = byRank.begin(); it != byRank.end(); ++it) {
        QStringList mainMembers;
        QStringList branchMembers;
        for (const QString& id : it.value()) {
            if (branchLabel.value(id).isEmpty())
                mainMembers.append(id);
            else
                branchMembers.append(id);
        }
        const auto byDataOrder = [&dataOrderIndex](const QString& a, const QString& b) {
            return dataOrderIndex.value(a, std::numeric_limits<int>::max()) <
                   dataOrderIndex.value(b, std::numeric_limits<int>::max());
        };
        std::stable_sort(mainMembers.begin(), mainMembers.end(), byDataOrder);
        std::stable_sort(branchMembers.begin(), branchMembers.end(), [&](const QString& a, const QString& b) {
            const QString regionA = regionOf(a);
            const QString regionB = regionOf(b);
            if (regionA != regionB)
                return regionA < regionB;
            const int orderA = branchOrder.value(a);
            const int orderB = branchOrder.value(b);
            if (orderA != orderB)
                return orderA < orderB;
            return dataOrderIndex.value(a, std::numeric_limits<int>::max()) <
                   dataOrderIndex.value(b, std::numeric_limits<int>::max());
        });
        QStringList members = mainMembers;
        members.append(branchMembers);
        for (int i = 0; i < members.size(); ++i)
            lane.insert(members.at(i), laneForIndex(i));
    }

    plan.nodes.reserve(plan.displayOrder.size());
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
        plan.rankIndex.insert(id, node.rank);
        plan.laneIndex.insert(id, node.lane);
    }

    plan.valid = true;
    return plan;
}

} // namespace DeepLux
