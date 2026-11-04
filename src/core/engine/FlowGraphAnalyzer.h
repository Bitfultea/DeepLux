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

    // B3.2：由当前工程与拖动后的展示顺序构造候选图（只读，不修改 Project）。
    // 仅支持设计 §7.3 的两种互斥情形：
    //   A. 工程无任何连接：只重排模块顺序，不创建连接；
    //   B. 已连接的简单线性数据链：按新顺序用同一端口模式重连。
    // 其他结构（多端口模式、分支、汇合、控制边、环、孤立节点）返回 false。
    static bool buildReorderedCandidate(const Project& project, const QStringList& orderedIds,
                                        FlowGraphCandidate* candidate, QString* error);

    // B3.2：只读校验候选图。依次检查模块存在/完整排列、端口存在与类型兼容、
    // 边类型归一化、重复边、前向图无环、控制结构可解析、线性链端口模式无歧义。
    // 成功时把分析得到的执行计划写入 plan；失败时不修改任何外部状态。
    static bool validateCandidate(const FlowGraphCatalog& catalog, const FlowGraphCandidate& candidate,
                                  FlowOrderPlan* plan, QString* error);
};

} // namespace DeepLux
