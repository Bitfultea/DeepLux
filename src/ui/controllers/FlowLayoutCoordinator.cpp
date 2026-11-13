#include "FlowLayoutCoordinator.h"

#include "../widgets/FlowCanvas.h"
#include "ProcessTreeController.h"

#include <QDebug>
#include <QSet>

namespace DeepLux {

namespace {
constexpr int kLayoutDebounceMs = 30;
}

FlowLayoutCoordinator::FlowLayoutCoordinator(Project* project, FlowCanvas* canvas, CatalogProvider catalogProvider,
                                             QObject* parent)
    : QObject(parent), m_project(project), m_canvas(canvas), m_catalogProvider(std::move(catalogProvider)) {
    m_layoutTimer.setSingleShot(true);
    m_layoutTimer.setInterval(kLayoutDebounceMs);
    connect(&m_layoutTimer, &QTimer::timeout, this, &FlowLayoutCoordinator::runLayout);

    if (!m_catalogProvider) {
        qWarning() << "FlowLayoutCoordinator: catalog provider must not be null";
    }

    if (m_project) {
        connectProjectSignals();
        // 初始工程同样遵循“未初始化/坐标无效才布局”的规则。
        if (!m_project->layoutInitialized() || !positionsValid(*m_project)) {
            scheduleLayout();
        }
    }
}

FlowLayoutCoordinator::~FlowLayoutCoordinator() = default;

void FlowLayoutCoordinator::connectProjectSignals() {
    if (!m_project)
        return;

    Project* project = m_project.data();
    connect(project, &Project::moduleAdded, this, &FlowLayoutCoordinator::scheduleLayout);
    connect(project, &Project::moduleRemoved, this, &FlowLayoutCoordinator::scheduleLayout);
    connect(project, &Project::connectionAdded, this, &FlowLayoutCoordinator::scheduleLayout);
    connect(project, &Project::connectionRemoved, this, &FlowLayoutCoordinator::scheduleLayout);
    connect(project, &Project::connectionRemovedWithPorts, this, &FlowLayoutCoordinator::scheduleLayout);
    connect(project, &Project::modulesReordered, this, &FlowLayoutCoordinator::scheduleLayout);
    connect(project, &Project::structureChanged, this, &FlowLayoutCoordinator::scheduleLayout);
    // modulePositionChanged 不参与自动布局：用户拖动不应触发重新排版。
}

void FlowLayoutCoordinator::setProject(Project* project) {
    if (m_project == project && project)
        return;

    if (m_project) {
        disconnect(m_project.data(), nullptr, this, nullptr);
    }

    // 任何在途结果立即失效
    ++m_generation;
    m_layoutTimer.stop();
    m_layoutPending = false;
    m_currentPlan = FlowOrderPlan();
    m_project = project;

    if (!m_project)
        return;

    connectProjectSignals();

    if (!m_project->layoutInitialized() || !positionsValid(*m_project)) {
        scheduleLayout();
    } else {
        // K5：已初始化且坐标有效时画布坐标原样保留（不执行 Full/Normalize），
        // 但流程树仍按 displayOrder 排序，保证画布 rank/Y 与流程树顺序一致。
        const FlowOrderPlan plan = planForProject(*m_project);
        if (plan.valid) {
            m_currentPlan = plan;
            applyTreeOrder(plan);
        }
    }
}

void FlowLayoutCoordinator::setProcessTreeController(ProcessTreeController* controller) {
    m_processTreeController = controller;
}

ProcessTreeController* FlowLayoutCoordinator::processTreeController() const {
    return m_processTreeController.data();
}

void FlowLayoutCoordinator::applyTreeOrder(const FlowOrderPlan& plan) {
    if (!plan.valid || !m_processTreeController)
        return;
    m_processTreeController->applyExecutionOrder(plan.displayOrder);
}

FlowGraphCatalog FlowLayoutCoordinator::catalogForProject(const Project& project) const {
    if (!m_catalogProvider) {
        return FlowGraphCatalog();
    }
    return m_catalogProvider(project);
}

FlowOrderPlan FlowLayoutCoordinator::planForProject(const Project& project) const {
    FlowOrderPlan plan;
    buildPlan(project, &plan);
    return plan;
}

bool FlowLayoutCoordinator::buildPlan(const Project& project, FlowOrderPlan* plan) const {
    if (!plan)
        return false;
    *plan = FlowOrderPlan();
    if (!m_catalogProvider) {
        qWarning() << "FlowLayoutCoordinator: no catalog provider available";
        return false;
    }

    const FlowGraphCatalog catalog = m_catalogProvider(project);
    // provider 必须覆盖工程中的所有模块；缺失时保持原布局并记录错误。
    for (const ModuleInstance& module : project.modules()) {
        if (!catalog.find(module.id)) {
            qWarning() << "FlowLayoutCoordinator: catalog is missing module" << module.id
                       << "- keeping existing positions";
            return false;
        }
    }

    QString normalizeError;
    const QList<ModuleConnection> normalized =
        FlowGraphAnalyzer::normalizeConnections(catalog, project.connections(), &normalizeError);
    if (!normalizeError.isEmpty()) {
        qWarning() << "FlowLayoutCoordinator: invalid connections:" << normalizeError;
        return false;
    }

    QStringList moduleOrder;
    moduleOrder.reserve(project.modules().size());
    for (const ModuleInstance& module : project.modules())
        moduleOrder.append(module.id);

    const FlowOrderPlan computed = FlowGraphAnalyzer::analyze(catalog, normalized, moduleOrder);
    if (!computed.valid) {
        qWarning() << "FlowLayoutCoordinator: execution plan is invalid:" << computed.error;
        return false;
    }
    *plan = computed;
    return true;
}

void FlowLayoutCoordinator::suppressAutoLayout(bool suppress) {
    if (m_suppressed == suppress)
        return;
    m_suppressed = suppress;
    if (m_suppressed) {
        ++m_generation;
        m_layoutTimer.stop();
        m_layoutPending = false;
    }
}

void FlowLayoutCoordinator::requestLayout() {
    scheduleLayout();
}

void FlowLayoutCoordinator::scheduleLayout() {
    if (m_suppressed || !m_project)
        return;
    // 同一事件循环轮次内的多个结构信号合并为一次布局。
    m_layoutPending = true;
    m_layoutTimer.stop();
    m_layoutTimer.start(kLayoutDebounceMs);
}

bool FlowLayoutCoordinator::positionsValid(const Project& project) {
    const QList<ModuleInstance> modules = project.modules();
    if (modules.size() <= 1)
        return true;

    // 至少两个互不相同的坐标；典型的未初始化工程全部为 (0,0)。
    QSet<QString> distinct;
    for (const ModuleInstance& module : modules) {
        distinct.insert(QStringLiteral("%1,%2").arg(module.posX).arg(module.posY));
        if (distinct.size() >= 2)
            return true;
    }
    return false;
}

void FlowLayoutCoordinator::runLayout() {
    m_layoutPending = false;

    if (!m_project || !m_canvas || m_suppressed)
        return;
    if (!m_catalogProvider) {
        qWarning() << "FlowLayoutCoordinator: no catalog provider available";
        return;
    }

    Project* project = m_project.data();
    FlowCanvas* canvas = m_canvas.data();
    if (!project || !canvas)
        return;

    const quint64 generation = m_generation;

    FlowOrderPlan plan;
    if (!buildPlan(*project, &plan))
        return;

    // 分析期间可能发生工程切换或抑制，丢弃过期结果。
    if (generation != m_generation || m_suppressed || m_project.data() != project)
        return;

    m_currentPlan = plan;
    applyTreeOrder(plan);
    canvas->applyExecutionLayout(plan, FlowCanvas::LayoutMode::Full);

    // 显式写回：即使画布未绑定该工程，也保证计划坐标持久化。
    for (const FlowOrderNode& node : plan.nodes) {
        if (FlowNodeItem* item = canvas->nodeItem(node.instanceId)) {
            project->setModulePosition(node.instanceId, qRound(item->pos().x()), qRound(item->pos().y()));
        }
    }

    project->setLayoutInitialized(true);

    ++m_layoutRunCount;
    emit layoutApplied();
}

} // namespace DeepLux
