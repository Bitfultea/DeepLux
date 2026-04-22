#include "FlowCanvas.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QGraphicsSceneMouseEvent>
#include <QDebug>

namespace DeepLux {

// ========== FlowCanvas ==========

FlowCanvas::FlowCanvas(QWidget* parent)
    : QGraphicsView(parent)
    , m_scene(new QGraphicsScene(this))
{
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::RubberBandDrag);
    setAcceptDrops(true);
    setSceneRect(-5000, -5000, 10000, 10000);

    // 背景
    setBackgroundBrush(QColor(45, 45, 45));
}

FlowCanvas::~FlowCanvas()
{
    clearConnections();
    clearNodes();
}

void FlowCanvas::addNode(const QString& moduleId, const QString& name, const QPointF& pos)
{
    QString nodeId = QString("node_%1").arg(++m_nodeCounter);

    FlowNodeItem* item = new FlowNodeItem(nodeId, name, moduleId);
    item->setPos(pos);
    m_scene->addItem(item);
    m_nodes[nodeId] = item;

    emit nodeAdded(nodeId);
}

void FlowCanvas::removeNode(const QString& nodeId)
{
    if (!m_nodes.contains(nodeId)) {
        return;
    }

    FlowNodeItem* item = m_nodes.take(nodeId);

    // 清除与此节点相关的连接
    removeConnectionsForNode(nodeId);

    m_scene->removeItem(item);
    delete item;
    emit nodeRemoved(nodeId);
}

void FlowCanvas::removeConnectionsForNode(const QString& nodeId)
{
    for (int i = m_connections.size() - 1; i >= 0; i--) {
        FlowConnectionItem* conn = m_connections[i];
        if (conn->fromNodeId() == nodeId || conn->toNodeId() == nodeId) {
            m_scene->removeItem(conn);
            delete conn;
            m_connections.removeAt(i);
        }
    }
}

void FlowCanvas::clearNodes()
{
    // 先清除所有连接
    clearConnections();

    for (auto* item : m_nodes) {
        m_scene->removeItem(item);
        delete item;
    }
    m_nodes.clear();
}

void FlowCanvas::addConnection(const QString& fromNodeId, int fromPort,
                                const QString& toNodeId, int toPort)
{
    if (!m_nodes.contains(fromNodeId) || !m_nodes.contains(toNodeId)) {
        return;
    }

    FlowNodeItem* fromItem = m_nodes[fromNodeId];
    FlowNodeItem* toItem = m_nodes[toNodeId];

    FlowConnectionItem* conn = new FlowConnectionItem(fromItem, fromPort, toItem, toPort);
    m_scene->addItem(conn);
    m_connections.append(conn);

    emit connectionCreated(fromNodeId, toNodeId);
}

void FlowCanvas::removeConnection(const QString& fromNodeId, const QString& toNodeId)
{
    for (int i = m_connections.size() - 1; i >= 0; i--) {
        if (m_connections[i]->fromNodeId() == fromNodeId &&
            m_connections[i]->toNodeId() == toNodeId) {
            FlowConnectionItem* conn = m_connections[i];
            m_scene->removeItem(conn);
            m_connections.removeAt(i);
            delete conn;
            emit connectionRemoved(fromNodeId, toNodeId);
            return;
        }
    }
}

void FlowCanvas::clearConnections()
{
    for (auto* conn : m_connections) {
        if (conn) {
            m_scene->removeItem(conn);
            delete conn;
        }
    }
    m_connections.clear();
}

QStringList FlowCanvas::nodeIds() const
{
    return m_nodes.keys();
}

FlowNodeItem* FlowCanvas::nodeItem(const QString& nodeId) const
{
    return m_nodes.value(nodeId, nullptr);
}

QList<FlowNodeItem*> FlowCanvas::getOrderedModules() const
{
    if (m_nodes.isEmpty()) {
        return {};
    }

    // 构建入度表
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

    // Kahn算法拓扑排序
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

    // 如果有环或没有连接，按Y坐标排序（保证有连接的节点优先）
    if (ordered.size() != m_nodes.size()) {
        // 混合策略：先放拓扑排序结果，再追加剩余节点（按Y坐标）
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

void FlowCanvas::updateConnectionsForNode(const QString& nodeId)
{
    for (FlowConnectionItem* conn : m_connections) {
        if (conn && (conn->fromNodeId() == nodeId || conn->toNodeId() == nodeId)) {
            conn->updatePath();
        }
    }
}

void FlowCanvas::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasText()) {
        event->acceptProposedAction();
    }
}

void FlowCanvas::dragMoveEvent(QDragMoveEvent* event)
{
    event->accept();
}

void FlowCanvas::dropEvent(QDropEvent* event)
{
    QString moduleData = event->mimeData()->text();
    // 格式: moduleId|name
    QStringList parts = moduleData.split("|");
    if (parts.size() >= 2) {
        QPointF pos = mapToScene(event->pos());
        addNode(parts[0], parts[1], pos);
    }
    event->acceptProposedAction();
}

// ========== FlowNodeItem ==========

FlowNodeItem::FlowNodeItem(const QString& nodeId, const QString& name,
                           const QString& moduleId, QGraphicsItem* parent)
    : QGraphicsItem(parent)
    , m_nodeId(nodeId)
    , m_moduleId(moduleId)
    , m_name(name)
{
    setFlag(QGraphicsItem::ItemIsMovable);
    setFlag(QGraphicsItem::ItemIsSelectable);
    setFlag(QGraphicsItem::ItemSendsGeometryChanges);
    setZValue(1);
}

void FlowNodeItem::setName(const QString& name)
{
    m_name = name;
    update();
}

QRectF FlowNodeItem::boundingRect() const
{
    return QRectF(0, 0, m_width, m_height);
}

void FlowNodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
                         QWidget* widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    // 背景
    QColor bgColor = isSelected() ? QColor(0, 120, 212) : QColor(60, 60, 60);
    painter->setBrush(bgColor);
    painter->setPen(QPen(isSelected() ? Qt::white : QColor(100, 100, 100), 2));
    painter->drawRoundedRect(0, 0, m_width, m_height, 5, 5);

    // 标题
    painter->setPen(Qt::white);
    QFont font = painter->font();
    font.setBold(true);
    painter->setFont(font);
    painter->drawText(QRectF(5, 5, m_width - 10, 25), Qt::AlignLeft | Qt::AlignVCenter, m_name);

    // 分割线
    painter->setPen(QColor(80, 80, 80));
    painter->drawLine(5, 30, m_width - 5, 30);

    // 输入端口
    painter->setBrush(QColor(100, 200, 100));
    for (int i = 0; i < m_inputPorts; i++) {
        qreal y = 35 + i * 20;
        painter->drawEllipse(-5, y, 10, 10);
    }

    // 输出端口
    painter->setBrush(QColor(200, 100, 100));
    for (int i = 0; i < m_outputPorts; i++) {
        qreal y = 35 + i * 20;
        painter->drawEllipse(m_width - 5, y, 10, 10);
    }
}

QPointF FlowNodeItem::inputPortPos(int index) const
{
    qreal y = 40 + index * 20;
    return QPointF(0, y);
}

QPointF FlowNodeItem::outputPortPos(int index) const
{
    qreal y = 40 + index * 20;
    return QPointF(m_width, y);
}

QVariant FlowNodeItem::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemPositionHasChanged) {
        // 通过 FlowCanvas 更新相关连接
        FlowCanvas* canvas = qobject_cast<FlowCanvas*>(scene()->parent());
        if (canvas) {
            canvas->updateConnectionsForNode(m_nodeId);
        }
    }
    return QGraphicsItem::itemChange(change, value);
}

void FlowNodeItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    QGraphicsItem::mousePressEvent(event);
    scene()->clearSelection();
    setSelected(true);
}

// ========== FlowConnectionItem ==========

FlowConnectionItem::FlowConnectionItem(FlowNodeItem* fromNode, int fromPort,
                                       FlowNodeItem* toNode, int toPort,
                                       QGraphicsItem* parent)
    : QGraphicsItem(parent)
    , m_fromNode(fromNode)
    , m_toNode(toNode)
    , m_fromPort(fromPort)
    , m_toPort(toPort)
{
    setZValue(0);
    updatePath();
}

FlowConnectionItem::~FlowConnectionItem()
{
    // 不需要清理节点中的引用，因为我们现在通过 nodeId 来查找
}

QString FlowConnectionItem::fromNodeId() const
{
    return m_fromNode ? m_fromNode->nodeId() : QString();
}

QString FlowConnectionItem::toNodeId() const
{
    return m_toNode ? m_toNode->nodeId() : QString();
}

bool FlowConnectionItem::isValid() const
{
    // 检查节点是否仍然有效（在场景中且未被删除）
    return m_fromNode && m_toNode &&
           m_fromNode->scene() == scene() &&
           m_toNode->scene() == scene();
}

QRectF FlowConnectionItem::boundingRect() const
{
    return m_path.boundingRect();
}

void FlowConnectionItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
                                QWidget* widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)

    painter->setPen(QPen(QColor(200, 200, 100), 2));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(m_path);
}

void FlowConnectionItem::updatePath()
{
    // 检查节点是否有效
    if (!m_fromNode || !m_toNode) {
        m_path = QPainterPath();
        return;
    }

    // 检查节点是否在场景中
    if (m_fromNode->scene() != scene() || m_toNode->scene() != scene()) {
        m_path = QPainterPath();
        return;
    }

    QPointF start = m_fromNode->scenePos() + m_fromNode->outputPortPos(m_fromPort);
    QPointF end = m_toNode->scenePos() + m_toNode->inputPortPos(m_toPort);

    qreal dx = qAbs(end.x() - start.x()) / 2;

    m_path = QPainterPath(start);
    m_path.cubicTo(start.x() + dx, start.y(),
                   end.x() - dx, end.y(),
                   end.x(), end.y());

    update();
}

} // namespace DeepLux