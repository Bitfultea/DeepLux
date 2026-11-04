#pragma once

#include "core/deeplux/ControlFlowType.h"
#include "core/deeplux/DataContract.h"
#include "core/model/Project.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QStringList>

namespace DeepLux {

// 共享描述符：instanceId 是 Project 模块实例 ID，与 ModuleConnection.fromModuleId/toModuleId 同命名空间；
// 不是运行期显示名。端口直接复用 ABI 的 PortSpec，不另造一套类型。
struct FlowGraphModule {
    QString instanceId;
    ControlFlowType flowType = ControlFlowType::Sequential;
    QList<PortSpec> inputPorts;
    QList<PortSpec> outputPorts;
};

struct FlowGraphCatalog {
    QList<FlowGraphModule> modules;
    const FlowGraphModule* find(const QString& instanceId) const;
};

struct FlowOrderNode {
    QString instanceId;
    int rank = 0;
    int lane = 0;
    int displayIndex = 0;
    QString branchLabel;
    QString controlParent;
};

struct FlowOrderPlan {
    bool valid = false;
    QString error;
    QStringList dataOrder;
    QStringList displayOrder;
    QList<FlowOrderNode> nodes;
    QList<ModuleConnection> controlEdges;
    QSet<QString> loopBackEdges;
    QSet<QString> reentrantModules;
    QMap<QString, QSet<QString>> loopRegions;
    QHash<QString, int> rankIndex;
    QHash<QString, int> laneIndex;
    int rankOf(const QString& id) const;
    int laneOf(const QString& id) const;
};

class FlowGraphAnalyzer {
public:
    // 统一的控制端口判定：源端支持隐式 next，目标端支持隐式 control，
    // 其余端口以 PortSpec.control 为准。normalizeConnections 与 RunEngine 共用，
    // 避免 next/control/PortSpec.control 规则分散实现。
    static bool isControlPort(const QList<PortSpec>& ports, bool source, const QString& portId);
    static QList<ModuleConnection> normalizeConnections(const FlowGraphCatalog& catalog,
                                                        const QList<ModuleConnection>& connections, QString* error);
    static FlowOrderPlan analyze(const FlowGraphCatalog& catalog, const QList<ModuleConnection>& normalizedConnections,
                                 const QStringList& moduleOrder);
};

} // namespace DeepLux
