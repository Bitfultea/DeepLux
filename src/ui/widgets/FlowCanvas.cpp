#include "FlowCanvas.h"

#include "core/common/ModuleIconProvider.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsTextItem>
#include <QMimeData>
#include <QPainter>
#include <QPolygonF>
#include <QToolTip>
#include <cmath>

namespace DeepLux {
namespace {

QColor connectionColor(const QGraphicsScene* scene, int alpha = 255) {
    QColor color = scene && scene->backgroundBrush().color().lightness() <= 128 ? QColor("#94A3B8") : QColor("#64748B");
    color.setAlpha(alpha);
    return color;
}

QPainterPath connectionPath(const QPointF& start, const QPointF& end) {
    QPainterPath path(start);
    const qreal dx = end.x() - start.x();
    const qreal dy = end.y() - start.y();
    if (qAbs(dy) >= qAbs(dx)) {
        const qreal offset = qMax<qreal>(32.0, qAbs(dy) / 2.0);
        const qreal direction = dy >= 0.0 ? 1.0 : -1.0;
        path.cubicTo(start.x(), start.y() + direction * offset, end.x(), end.y() - direction * offset, end.x(),
                     end.y());
    } else {
        const qreal offset = qMax<qreal>(32.0, qAbs(dx) / 2.0);
        const qreal direction = dx >= 0.0 ? 1.0 : -1.0;
        path.cubicTo(start.x() + direction * offset, start.y(), end.x() - direction * offset, end.y(), end.x(),
                     end.y());
    }
    return path;
}

void drawConnectionArrow(QPainter* painter, const QPainterPath& path, const QColor& color) {
    if (!painter || path.isEmpty()) {
        return;
    }
    const QPointF tip = path.pointAtPercent(0.56);
    const QPointF tail = path.pointAtPercent(0.44);
    const qreal angle = std::atan2(tip.y() - tail.y(), tip.x() - tail.x());
    const qreal size = 8.0;
    QPolygonF arrow;
    arrow << tip << tip - QPointF(std::cos(angle - 0.55) * size, std::sin(angle - 0.55) * size)
          << tip - QPointF(std::cos(angle + 0.55) * size, std::sin(angle + 0.55) * size);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawPolygon(arrow);
}

} // namespace

// ========== FlowCanvas ==========

FlowCanvas::FlowCanvas(QWidget* parent) : QGraphicsView(parent), m_scene(new QGraphicsScene(this)) {
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::RubberBandDrag);
    setAcceptDrops(true);

    QPalette pal = viewport()->palette();
    bool isDark = pal.color(QPalette::Window).lightness() <= 128;
    const QColor background = isDark ? QColor("#1e1e1e") : QColor("#f5f5f5");
    setBackgroundBrush(background);
    m_scene->setBackgroundBrush(background);
}

FlowCanvas::~FlowCanvas() {
    clearConnections();
    clearNodes();
}

void FlowCanvas::applyTheme(bool isDark) {
    const QColor background = isDark ? QColor("#1e1e1e") : QColor("#f5f5f5");
    setBackgroundBrush(background);
    m_scene->setBackgroundBrush(background);
    viewport()->update();
}

bool FlowCanvas::portsCompatible(const PortSpec& fromPort, const PortSpec& toPort) {
    if (fromPort.control != toPort.control)
        return false;
    if (fromPort.type == DataType::Any || toPort.type == DataType::Any)
        return true;
    return fromPort.type == toPort.type;
}

QString FlowCanvas::connectionKey(const QString& fromId, const QString& fromPort,
                                    const QString& toId, const QString& toPort) {
    return fromId + QLatin1Char('\x1f') + fromPort + QLatin1Char('\x1f') +
           toId + QLatin1Char('\x1f') + toPort;
}

QString FlowCanvas::addNode(const QString& moduleId, const QString& name, const QPointF& pos,
                            const QString& instanceId) {
    QString nodeId = instanceId.isEmpty() ? QString("node_%1").arg(++m_nodeCounter) : instanceId;
    if (m_nodes.contains(nodeId)) {
        return nodeId;
    }

    FlowNodeItem* item = new FlowNodeItem(nodeId, name, moduleId);
    m_scene->addItem(item);
    item->setPos(pos);
    m_nodes[nodeId] = item;

    // 从 PluginManager 加载端口声明
    PluginInfo info = PluginManager::instance().pluginInfo(moduleId);
    if (!info.id.isEmpty()) {
        item->setPortSpecs(info.inputPorts, info.outputPorts);
    }

    if (!m_loadingProject) {
        Project* project = ProjectManager::instance().currentProject();
        if (project && !project->findModule(nodeId)) {
            ModuleInstance inst;
            inst.id = nodeId;
            inst.moduleId = moduleId;
            inst.name = name;
            inst.posX = static_cast<int>(pos.x());
            inst.posY = static_cast<int>(pos.y());
            project->addModule(inst);
        }
    }

    emit nodeAdded(nodeId);
    return nodeId;
}

void FlowCanvas::removeNode(const QString& nodeId) {
    if (!m_nodes.contains(nodeId)) {
        return;
    }

    FlowNodeItem* item = m_nodes.take(nodeId);

    removeConnectionsForNode(nodeId);

    m_scene->removeItem(item);
    delete item;

    if (!m_loadingProject) {
        Project* project = ProjectManager::instance().currentProject();
        if (project) {
            project->removeModule(nodeId);
        }
    }

    emit nodeRemoved(nodeId);
}

void FlowCanvas::removeConnectionsForNode(const QString& nodeId) {
    for (int i = m_connections.size() - 1; i >= 0; i--) {
        FlowConnectionItem* conn = m_connections[i];
        if (conn->fromNodeId() == nodeId || conn->toNodeId() == nodeId) {
            const QString fromId = conn->fromNodeId();
            const QString toId = conn->toNodeId();
            m_scene->removeItem(conn);
            delete conn;
            m_connections.removeAt(i);
            if (!m_loadingProject) {
                Project* project = ProjectManager::instance().currentProject();
                if (project) {
                    project->removeConnection(fromId, toId);
                }
                emit connectionRemoved(fromId, toId);
            }
        }
    }
}

void FlowCanvas::clearNodes() {
    clearConnections();

    for (auto* item : m_nodes) {
        m_scene->removeItem(item);
        delete item;
    }
    m_nodes.clear();
}

void FlowCanvas::addConnection(const QString& fromNodeId, const QString& fromPortId,
                               const QString& toNodeId, const QString& toPortId,
                               const QString& edgeType) {
    if (!m_nodes.contains(fromNodeId) || !m_nodes.contains(toNodeId)) {
        return;
    }

    // 按完整 4 元组去重（允许同节点对不同端口的多连接）
    const QString key = connectionKey(fromNodeId, fromPortId, toNodeId, toPortId);
    for (FlowConnectionItem* existing : m_connections) {
        if (existing && connectionKey(existing->fromNodeId(), existing->fromPortId(),
                                       existing->toNodeId(), existing->toPortId()) == key) {
            return;
        }
    }

    FlowNodeItem* fromItem = m_nodes[fromNodeId];
    FlowNodeItem* toItem = m_nodes[toNodeId];

    FlowConnectionItem* conn = new FlowConnectionItem(fromItem, fromPortId, toItem, toPortId, edgeType);
    m_scene->addItem(conn);
    m_connections.append(conn);

    if (!m_loadingProject && !m_syncingFromProject) {
        Project* project = ProjectManager::instance().currentProject();
        if (project) {
            ModuleConnection projectConn;
            projectConn.fromModuleId = fromNodeId;
            projectConn.toModuleId = toNodeId;
            projectConn.fromPort = fromPortId;
            projectConn.toPort = toPortId;
            projectConn.edgeType = edgeType;
            projectConn.fromOutput = 0;
            projectConn.toInput = 0;
            project->addConnection(projectConn);
        }
    }

    emit connectionCreated(fromNodeId, toNodeId);
}

void FlowCanvas::removeConnection(const QString& fromNodeId, const QString& fromPortId,
                                   const QString& toNodeId, const QString& toPortId) {
    const QString key = connectionKey(fromNodeId, fromPortId, toNodeId, toPortId);
    for (int i = m_connections.size() - 1; i >= 0; i--) {
        FlowConnectionItem* conn = m_connections[i];
        if (connectionKey(conn->fromNodeId(), conn->fromPortId(),
                         conn->toNodeId(), conn->toPortId()) == key) {
            m_scene->removeItem(conn);
            m_connections.removeAt(i);
            delete conn;
            if (!m_loadingProject && !m_syncingFromProject) {
                Project* project = ProjectManager::instance().currentProject();
                if (project) {
                    project->removeConnectionWithPorts(fromNodeId, fromPortId, toNodeId, toPortId);
                }
            }
            emit connectionRemoved(fromNodeId, toNodeId);
            return;
        }
    }
}

void FlowCanvas::loadFromProject(Project* project) {
    m_loadingProject = true;
    clearConnections();
    clearNodes();

    if (project) {
        for (const ModuleInstance& inst : project->modules()) {
            addNode(inst.moduleId, inst.name, QPointF(inst.posX, inst.posY), inst.id);
        }
        for (const ModuleConnection& conn : project->connections()) {
            // 优先使用字符串端口；回退到旧整数字段
            QString fromPort = conn.fromPort.isEmpty() ? QStringLiteral("image") : conn.fromPort;
            QString toPort = conn.toPort.isEmpty() ? QStringLiteral("image") : conn.toPort;
            QString edgeType = conn.edgeType.isEmpty() ? QStringLiteral("data") : conn.edgeType;
            addConnection(conn.fromModuleId, fromPort, conn.toModuleId, toPort, edgeType);
        }
    }

    m_loadingProject = false;
}

void FlowCanvas::clearConnections() {
    for (auto* conn : m_connections) {
        if (conn) {
            m_scene->removeItem(conn);
            delete conn;
        }
    }
    m_connections.clear();
}

QStringList FlowCanvas::nodeIds() const {
    return m_nodes.keys();
}

FlowNodeItem* FlowCanvas::nodeItem(const QString& nodeId) const {
    return m_nodes.value(nodeId, nullptr);
}

void FlowCanvas::selectNode(const QString& nodeId) {
    FlowNodeItem* item = m_nodes.value(nodeId, nullptr);
    if (!item) {
        return;
    }
    m_scene->clearSelection();
    item->setSelected(true);
}

void FlowCanvas::setNodeExecutionState(const QString& nodeId, const QString& status, const QString& timeText) {
    if (FlowNodeItem* item = m_nodes.value(nodeId, nullptr)) {
        item->setExecutionState(status, timeText);
    }
}

QList<FlowNodeItem*> FlowCanvas::getOrderedModules() const {
    if (m_nodes.isEmpty()) {
        return {};
    }

    QMap<QString, int> inDegree;
    QMap<QString, QStringList> adj;
    for (const QString& nodeId : m_nodes.keys()) {
        inDegree[nodeId] = 0;
    }

    for (FlowConnectionItem* conn : m_connections) {
        QString fromId = conn->fromNodeId();
        QString toId = conn->toNodeId();
        if (m_nodes.contains(fromId) && m_nodes.contains(toId)) {
            adj[fromId].append(toId);
            inDegree[toId]++;
        }
    }

    QList<FlowNodeItem*> ordered;
    QStringList queue;

    for (const QString& nodeId : inDegree.keys()) {
        if (inDegree[nodeId] == 0) {
            queue.append(nodeId);
        }
    }

    while (!queue.isEmpty()) {
        QString nodeId = queue.takeFirst();
        if (m_nodes.contains(nodeId)) {
            ordered.append(m_nodes[nodeId]);
        }
        for (const QString& nextId : adj.value(nodeId)) {
            inDegree[nextId]--;
            if (inDegree[nextId] == 0) {
                queue.append(nextId);
            }
        }
    }

    if (ordered.size() != m_nodes.size()) {
        QSet<QString> visited;
        for (FlowNodeItem* item : ordered) {
            visited.insert(item->nodeId());
        }
        for (FlowNodeItem* item : m_nodes) {
            if (!visited.contains(item->nodeId())) {
                ordered.append(item);
            }
        }
    }

    return ordered;
}

void FlowCanvas::updateConnectionsForNode(const QString& nodeId) {
    bool updated = false;
    for (FlowConnectionItem* conn : m_connections) {
        if (conn && (conn->fromNodeId() == nodeId || conn->toNodeId() == nodeId)) {
            conn->updatePath();
            updated = true;
        }
    }
    if (!updated && m_scene) {
        m_scene->update();
    }
}

void FlowCanvas::showDragLine(const QPointF& start, const QPointF& end) {
    if (!m_dragLine) {
        m_dragLine = new QGraphicsPathItem();
        m_dragLine->setZValue(5);
        m_scene->addItem(m_dragLine);
    }
    QPainterPath path = connectionPath(start, end);
    m_dragLine->setPath(path);
    QPen pen(QColor("#3B82F6"), 2, Qt::DashLine, Qt::RoundCap);
    m_dragLine->setPen(pen);
    m_dragLine->show();
}

void FlowCanvas::hideDragLine() {
    if (m_dragLine) {
        m_dragLine->hide();
    }
}

void FlowCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasText()) {
        event->acceptProposedAction();
    }
}

void FlowCanvas::dragMoveEvent(QDragMoveEvent* event) {
    event->accept();
}

void FlowCanvas::dropEvent(QDropEvent* event) {
    QString moduleData = event->mimeData()->text();
    QStringList parts = moduleData.split("|");
    if (parts.size() >= 2) {
        QPointF pos = mapToScene(event->pos());
        addNode(parts[0], parts[1], pos);
    }
    event->acceptProposedAction();
}

void FlowCanvas::drawBackground(QPainter* painter, const QRectF& rect) {
    QGraphicsView::drawBackground(painter, rect);
    if (!painter || !m_connections.isEmpty() || m_nodes.size() < 2) {
        return;
    }

    QList<FlowNodeItem*> ordered;
    QSet<QString> added;
    if (Project* project = ProjectManager::instance().currentProject()) {
        for (const ModuleInstance& inst : project->modules()) {
            FlowNodeItem* item = m_nodes.value(inst.id, nullptr);
            if (item) {
                ordered.append(item);
                added.insert(inst.id);
            }
        }
    }
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
        if (!added.contains(it.key())) {
            ordered.append(it.value());
        }
    }
    if (ordered.size() < 2) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QColor lineColor = connectionColor(m_scene, 170);
    painter->setPen(QPen(lineColor, 2, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    for (int i = 0; i < ordered.size() - 1; ++i) {
        FlowNodeItem* from = ordered[i];
        FlowNodeItem* to = ordered[i + 1];
        if (!from || !to) {
            continue;
        }
        const QPointF start = from->scenePos() + from->outputPortPos(0);
        const QPointF end = to->scenePos() + to->inputPortPos(0);
        const QPainterPath path = connectionPath(start, end);
        painter->drawPath(path);
        drawConnectionArrow(painter, path, lineColor);
    }

    const bool isDark = m_scene->backgroundBrush().color().lightness() <= 128;
    QColor legendColor = isDark ? QColor("#94A3B8") : QColor("#64748B");
    legendColor.setAlpha(120);
    painter->setPen(QPen(legendColor, 1, Qt::DashLine));
    QFont legendFont = painter->font();
    legendFont.setPointSize(8);
    painter->setFont(legendFont);
    const QString legendText = tr("虚线 = 执行顺序");
    const QRectF legendRect = mapToScene(10, 10).x() >= 0 ? QRectF(mapToScene(10, 10), QSizeF(140, 18))
                                                           : QRectF(10, 10, 140, 18);
    painter->drawText(legendRect, Qt::AlignLeft | Qt::AlignVCenter, legendText);

    painter->restore();
}

// ========== FlowNodeItem ==========

FlowNodeItem::FlowNodeItem(const QString& nodeId, const QString& name, const QString& moduleId, QGraphicsItem* parent)
    : QGraphicsItem(parent), m_nodeId(nodeId), m_moduleId(moduleId), m_name(name) {
    setFlag(QGraphicsItem::ItemIsMovable);
    setFlag(QGraphicsItem::ItemIsSelectable);
    setFlag(QGraphicsItem::ItemSendsGeometryChanges);
    setZValue(1);
    setAcceptHoverEvents(true);
}

void FlowNodeItem::setPortSpecs(const QList<PortSpec>& inputs, const QList<PortSpec>& outputs) {
    prepareGeometryChange(); // P2-fix: 几何变更前通知场景
    m_inputPortSpecs = inputs;
    m_outputPortSpecs = outputs;
    m_height = qMax<qreal>(64, 32 + qMax(inputs.size(), outputs.size()) * 18.0);
    update();
}

void FlowNodeItem::setName(const QString& name) {
    m_name = name;
    update();
}

void FlowNodeItem::setExecutionState(const QString& status, const QString& timeText) {
    m_status = status;
    m_timeText = timeText;
    update();
}

QRectF FlowNodeItem::boundingRect() const {
    // P1-fix: 覆盖端口标签（上下 ±14）和高亮外环（±3）
    return QRectF(-8, -16, m_width + 16, m_height + 32);
}

QPointF FlowNodeItem::inputPortPos(int index) const {
    const int count = qMax(1, m_inputPortSpecs.size());
    const qreal spacing = 20.0;
    const qreal x = m_width / 2 + (index - (count - 1) / 2.0) * spacing;
    return QPointF(x, 0);
}

QPointF FlowNodeItem::outputPortPos(int index) const {
    const int count = qMax(1, m_outputPortSpecs.size());
    const qreal spacing = 20.0;
    const qreal x = m_width / 2 + (index - (count - 1) / 2.0) * spacing;
    return QPointF(x, m_height);
}

QPointF FlowNodeItem::inputPortPos(const QString& portId) const {
    for (int i = 0; i < m_inputPortSpecs.size(); ++i) {
        if (m_inputPortSpecs[i].id == portId)
            return inputPortPos(i);
    }
    return inputPortPos(0);
}

QPointF FlowNodeItem::outputPortPos(const QString& portId) const {
    for (int i = 0; i < m_outputPortSpecs.size(); ++i) {
        if (m_outputPortSpecs[i].id == portId)
            return outputPortPos(i);
    }
    return outputPortPos(0);
}

QString FlowNodeItem::inputPortAt(const QPointF& pos) const {
    // 将 pos 转换为节点本地坐标
    const QPointF local = mapFromScene(pos);
    for (int i = 0; i < m_inputPortSpecs.size(); ++i) {
        const QPointF portPos = inputPortPos(i);
        const qreal dx = local.x() - portPos.x();
        const qreal dy = local.y() - portPos.y();
        if (dx * dx + dy * dy <= 36.0) // 6px radius
            return m_inputPortSpecs[i].id;
    }
    return QString();
}

QString FlowNodeItem::outputPortAt(const QPointF& pos) const {
    const QPointF local = mapFromScene(pos);
    for (int i = 0; i < m_outputPortSpecs.size(); ++i) {
        const QPointF portPos = outputPortPos(i);
        const qreal dx = local.x() - portPos.x();
        const qreal dy = local.y() - portPos.y();
        if (dx * dx + dy * dy <= 36.0)
            return m_outputPortSpecs[i].id;
    }
    return QString();
}

void FlowNodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option)
    Q_UNUSED(widget)

    bool isDark = scene() && scene()->backgroundBrush().color().lightness() <= 128;

    // 视觉状态优先级：Selected > Running > Disabled > Breakpoint > Normal
    QColor bgColor, borderColor, textColor, secondaryTextColor;
    if (isDark) {
        bgColor = QColor(45, 45, 48);
        borderColor = QColor("#3b4148");
        textColor = QColor("#ffffff");
        secondaryTextColor = QColor("#94A3B8");
    } else {
        bgColor = QColor(255, 255, 255);
        borderColor = QColor("#dce2e8");
        textColor = QColor("#212121");
        secondaryTextColor = QColor("#64748B");
    }

    // 禁用：灰化
    if (m_disabledVisual) {
        bgColor = isDark ? QColor(35, 35, 38) : QColor(240, 240, 240);
        textColor = textColor.darker(150);
        secondaryTextColor = secondaryTextColor.darker(150);
    }

    // 选中：蓝色高亮
    if (isSelected()) {
        bgColor = QColor(0, 120, 212);
        borderColor = isDark ? QColor("#06B6D4") : QColor("#0078d7");
        textColor = QColor("#ffffff");
        secondaryTextColor = QColor("#E0F2FE");
    }

    // 运行中：蓝色边框
    if (m_status == QStringLiteral("running")) {
        borderColor = QColor("#3B82F6");
    }

    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(bgColor);
    painter->setPen(QPen(borderColor, isSelected() ? 2.0 : 1.5));
    painter->drawRoundedRect(0, 0, m_width, m_height, 6, 6);

    // P2-fix: 兼容目标高亮（绿色外环）
    if (m_highlightCompatible) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor("#22C55E"), 2, Qt::DashLine));
        painter->drawRoundedRect(-3, -3, m_width + 6, m_height + 6, 8, 8);
    }

    // 断点标记（左上角红色小圆点）
    if (m_breakpointVisual) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor("#EF4444"));
        painter->drawEllipse(QPointF(6, 6), 4, 4);
    }

    // 标题
    painter->setPen(textColor);
    QFont font = painter->font();
    font.setBold(true);
    font.setPointSize(9);
    painter->setFont(font);
    ModuleIconProvider::instance().iconFor(m_moduleId, QString()).paint(painter, QRect(8, 6, 20, 20));
    const qreal statusWidth = m_status.isEmpty() ? 8.0 : 72.0;
    const QRectF titleRect(34, 4, m_width - 42 - statusWidth, 24);
    painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(m_name, Qt::ElideRight, qRound(titleRect.width())));

    font.setBold(false);
    font.setPointSize(8);
    painter->setFont(font);
    painter->setPen(secondaryTextColor);
    const QRectF subtitleRect(34, 29, m_width - 42, 20);
    const QString subtitle = m_nodeId != m_name ? m_nodeId : m_moduleId;
    painter->drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(subtitle, Qt::ElideRight, qRound(subtitleRect.width())));

    // 执行状态色点
    QColor statusColor;
    if (m_status == QStringLiteral("running"))
        statusColor = QColor("#3B82F6");
    else if (m_status == QStringLiteral("success"))
        statusColor = QColor("#22C55E");
    else if (m_status == QStringLiteral("failure"))
        statusColor = QColor("#EF4444");
    else if (m_status == QStringLiteral("dirty"))
        statusColor = QColor("#F59E0B");
    else if (m_status == QStringLiteral("paused"))
        statusColor = QColor("#D97706");
    else if (m_status == QStringLiteral("skipped"))
        statusColor = QColor("#9CA3AF");
    if (statusColor.isValid()) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(statusColor);
        painter->drawEllipse(QPointF(m_width - 62, 16), 4, 4);
        painter->setPen(textColor);
        const QString elapsed = painter->fontMetrics().elidedText(m_timeText, Qt::ElideRight, 48);
        painter->drawText(QRectF(m_width - 54, 5, 48, 22), Qt::AlignRight | Qt::AlignVCenter, elapsed);
    }

    // 端口绘制
    const QColor portColor = connectionColor(scene());
    const QColor controlPortColor = portColor.lighter(120);
    painter->setPen(QPen(portColor.darker(120), 1));

    // 输入端口
    for (int i = 0; i < m_inputPortSpecs.size(); i++) {
        const QPointF pos = inputPortPos(i);
        const PortSpec& spec = m_inputPortSpecs[i];
        const QColor& pc = spec.control ? controlPortColor : portColor;
        painter->setBrush(pc);
        painter->setPen(QPen(pc.darker(120), 1));
        painter->drawEllipse(pos, 5, 5);
        // 必需端口标记
        if (spec.required) {
            painter->setPen(QPen(pc.darker(150), 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(pos, 7, 7);
        }
    }
    // 如果没有声明端口，画一个默认端口
    if (m_inputPortSpecs.isEmpty()) {
        painter->setBrush(portColor);
        painter->setPen(QPen(portColor.darker(120), 1));
        painter->drawEllipse(QPointF(m_width / 2, 0), 5, 5);
    }

    // 输出端口
    for (int i = 0; i < m_outputPortSpecs.size(); i++) {
        const QPointF pos = outputPortPos(i);
        const PortSpec& spec = m_outputPortSpecs[i];
        const QColor& pc = spec.control ? controlPortColor : portColor;
        painter->setBrush(pc);
        painter->setPen(QPen(pc.darker(120), 1));
        painter->drawEllipse(pos, 5, 5);
    }
    if (m_outputPortSpecs.isEmpty()) {
        painter->setBrush(portColor);
        painter->setPen(QPen(portColor.darker(120), 1));
        painter->drawEllipse(QPointF(m_width / 2, m_height), 5, 5);
    }

    // P2-fix: 端口名称标签
    font.setPointSize(7);
    painter->setFont(font);
    painter->setPen(secondaryTextColor);
    for (int i = 0; i < m_inputPortSpecs.size(); ++i) {
        const QPointF pos = inputPortPos(i);
        const QString& pid = m_inputPortSpecs[i].id;
        const QString label = pid.length() > 6 ? pid.left(5) + QChar(0x2026) : pid;
        painter->drawText(QRectF(pos.x() - 30, -14, 60, 12), Qt::AlignCenter, label);
    }
    for (int i = 0; i < m_outputPortSpecs.size(); ++i) {
        const QPointF pos = outputPortPos(i);
        const QString& pid = m_outputPortSpecs[i].id;
        const QString label = pid.length() > 6 ? pid.left(5) + QChar(0x2026) : pid;
        painter->drawText(QRectF(pos.x() - 30, m_height + 2, 60, 12), Qt::AlignCenter, label);
    }
}

QVariant FlowNodeItem::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionHasChanged) {
        QGraphicsScene* currentScene = scene();
        FlowCanvas* canvas = currentScene ? qobject_cast<FlowCanvas*>(currentScene->parent()) : nullptr;
        if (canvas) {
            canvas->updateConnectionsForNode(m_nodeId);
        }
    }
    return QGraphicsItem::itemChange(change, value);
}

void FlowNodeItem::hoverMoveEvent(QGraphicsSceneHoverEvent* event) {
    const QString inPort = inputPortAt(event->scenePos());
    if (!inPort.isEmpty()) {
        for (const PortSpec& spec : m_inputPortSpecs) {
            if (spec.id == inPort) {
                QString tip = QStringLiteral("%1 (%2)").arg(spec.displayName, dataTypeName(spec.type));
                if (spec.required)
                    tip += QStringLiteral(" [必需]");
                if (spec.multiple)
                    tip += QStringLiteral(" [多输入]");
                if (spec.control)
                    tip += QStringLiteral(" [控制]");
                QToolTip::showText(event->screenPos(), tip);
                return;
            }
        }
    }
    const QString outPort = outputPortAt(event->scenePos());
    if (!outPort.isEmpty()) {
        for (const PortSpec& spec : m_outputPortSpecs) {
            if (spec.id == outPort) {
                QString tip = QStringLiteral("%1 (%2)").arg(spec.displayName, dataTypeName(spec.type));
                if (spec.control)
                    tip += QStringLiteral(" [控制]");
                QToolTip::showText(event->screenPos(), tip);
                return;
            }
        }
    }
    QToolTip::hideText();
}

void FlowNodeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    QToolTip::hideText();
    QGraphicsItem::hoverLeaveEvent(event);
}

void FlowNodeItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    // 检查是否点击在输出端口上
    if (event->button() == Qt::LeftButton) {
        const QString portId = outputPortAt(event->scenePos());
        if (!portId.isEmpty()) {
            m_draggingConnection = true;
            m_dragFromPortId = portId;
            m_dragCurrentPos = event->scenePos();
            event->accept();
            return;
        }
    }
    QGraphicsItem::mousePressEvent(event);
    scene()->clearSelection();
    setSelected(true);

    QGraphicsScene* currentScene = scene();
    FlowCanvas* canvas = currentScene ? qobject_cast<FlowCanvas*>(currentScene->parent()) : nullptr;
    if (canvas) {
        emit canvas->nodeSelected(m_nodeId);
    }
}

void FlowNodeItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (m_draggingConnection) {
        m_dragCurrentPos = event->scenePos();
        // P1-fix: 使用场景级临时连线（不被节点边界裁剪）
        QGraphicsScene* currentScene = scene();
        FlowCanvas* canvas = currentScene ? qobject_cast<FlowCanvas*>(currentScene->parent()) : nullptr;
        if (canvas) {
            const QPointF start = scenePos() + outputPortPos(m_dragFromPortId);
            canvas->showDragLine(start, m_dragCurrentPos);
        }
        // P2-fix: 先清除旧高亮，再设置新高亮
        if (m_highlightedTarget)
            m_highlightedTarget->setHighlightCompatible(false);
        m_highlightedTarget = nullptr;
        if (currentScene) {
            for (QGraphicsItem* item : currentScene->items(event->scenePos())) {
                FlowNodeItem* targetNode = dynamic_cast<FlowNodeItem*>(item);
                if (targetNode && targetNode != this) {
                    QString targetPort = targetNode->inputPortAt(event->scenePos());
                    bool compatible = false;
                    if (!targetPort.isEmpty()) {
                        for (const PortSpec& outSpec : m_outputPortSpecs) {
                            if (outSpec.id == m_dragFromPortId) {
                                for (const PortSpec& inSpec : targetNode->m_inputPortSpecs) {
                                    if (inSpec.id == targetPort) {
                                        compatible = FlowCanvas::portsCompatible(outSpec, inSpec);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    targetNode->setHighlightCompatible(compatible);
                    m_highlightedTarget = targetNode;
                    break;
                }
            }
        }
        event->accept();
        return;
    }
    QGraphicsItem::mouseMoveEvent(event);
}

void FlowNodeItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (m_draggingConnection) {
        m_draggingConnection = false;
        QGraphicsScene* currentScene = scene();
        FlowCanvas* canvas = currentScene ? qobject_cast<FlowCanvas*>(currentScene->parent()) : nullptr;
        // P1-fix: 隐藏场景级临时连线
        if (canvas)
            canvas->hideDragLine();
        // P2-fix: 清除高亮
        if (m_highlightedTarget)
            m_highlightedTarget->setHighlightCompatible(false);
        m_highlightedTarget = nullptr;
        if (currentScene) {
            for (QGraphicsItem* item : currentScene->items(event->scenePos())) {
                FlowNodeItem* targetNode = dynamic_cast<FlowNodeItem*>(item);
                if (targetNode && targetNode != this) {
                    const QString targetPort = targetNode->inputPortAt(event->scenePos());
                    if (!targetPort.isEmpty()) {
                        bool compatible = false;
                        for (const PortSpec& outSpec : m_outputPortSpecs) {
                            if (outSpec.id == m_dragFromPortId) {
                                for (const PortSpec& inSpec : targetNode->m_inputPortSpecs) {
                                    if (inSpec.id == targetPort) {
                                        compatible = FlowCanvas::portsCompatible(outSpec, inSpec);
                                        break;
                                    }
                                }
                            }
                        }
                        if (compatible && canvas) {
                            emit canvas->connectionRequest(nodeId(), m_dragFromPortId,
                                                            targetNode->nodeId(), targetPort);
                        }
                    }
                    break;
                }
            }
        }
        m_dragFromPortId.clear();
        update();
        event->accept();
        return;
    }
    QGraphicsItem::mouseReleaseEvent(event);
}

// ========== FlowConnectionItem ==========

FlowConnectionItem::FlowConnectionItem(FlowNodeItem* fromNode, const QString& fromPortId,
                                        FlowNodeItem* toNode, const QString& toPortId,
                                        const QString& edgeType, QGraphicsItem* parent)
    : QGraphicsItem(parent), m_fromNode(fromNode), m_toNode(toNode),
      m_fromPortId(fromPortId), m_toPortId(toPortId), m_edgeType(edgeType) {
    setZValue(0);
    setAcceptHoverEvents(true);
    setToolTip(QStringLiteral("%1.%2 → %3.%4 (%5)")
                   .arg(fromNode ? fromNode->nodeId() : QString(), fromPortId,
                        toNode ? toNode->nodeId() : QString(), toPortId, edgeType));
    updatePath();
}

FlowConnectionItem::~FlowConnectionItem() = default;

QString FlowConnectionItem::fromNodeId() const {
    return m_fromNode ? m_fromNode->nodeId() : QString();
}

QString FlowConnectionItem::toNodeId() const {
    return m_toNode ? m_toNode->nodeId() : QString();
}

bool FlowConnectionItem::isValid() const {
    return m_fromNode && m_toNode && m_fromNode->scene() == scene() && m_toNode->scene() == scene();
}

QRectF FlowConnectionItem::boundingRect() const {
    if (m_path.isEmpty()) {
        return QRectF();
    }
    return m_path.boundingRect().adjusted(-10, -10, 10, 10);
}

void FlowConnectionItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option)
    Q_UNUSED(widget)

    const QColor lineColor = connectionColor(scene());
    // 数据边实线，控制边虚线
    const Qt::PenStyle penStyle = isControlEdge() ? Qt::DashLine : Qt::SolidLine;
    painter->setPen(QPen(lineColor, 2, penStyle, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(m_path);
    drawConnectionArrow(painter, m_path, lineColor);
}

void FlowConnectionItem::updatePath() {
    QPainterPath newPath;

    if (m_fromNode && m_toNode) {
        QGraphicsScene* nodeScene = m_fromNode->scene();
        const bool nodesShareScene = nodeScene && nodeScene == m_toNode->scene();
        const bool itemSceneMatches = !scene() || scene() == nodeScene;
        if (nodesShareScene && itemSceneMatches) {
            QPointF start = m_fromNode->scenePos() + m_fromNode->outputPortPos(m_fromPortId);
            QPointF end = m_toNode->scenePos() + m_toNode->inputPortPos(m_toPortId);
            newPath = connectionPath(start, end);
        }
    }

    prepareGeometryChange();
    m_path = newPath;
    update();
}

} // namespace DeepLux
