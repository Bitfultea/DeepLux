#pragma once

#include <QObject>
#include <QWidget>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QMap>
#include <QMutex>

namespace DeepLux {

class FlowNodeItem;
class FlowConnectionItem;

/**
 * @brief 流程画布
 *
 * 显示和编辑模块流程图
 */
class FlowCanvas : public QGraphicsView
{
    Q_OBJECT

public:
    explicit FlowCanvas(QWidget* parent = nullptr);
    ~FlowCanvas() override;

    // 节点操作
    void addNode(const QString& moduleId, const QString& name, const QPointF& pos);
    void removeNode(const QString& nodeId);
    void clearNodes();

    // 连接操作
    void addConnection(const QString& fromNodeId, int fromPort,
                       const QString& toNodeId, int toPort);
    void removeConnection(const QString& fromNodeId, const QString& toNodeId);
    void clearConnections();

    // 查询
    QStringList nodeIds() const;
    FlowNodeItem* nodeItem(const QString& nodeId) const;
    QList<FlowNodeItem*> getOrderedModules() const;

signals:
    void nodeAdded(const QString& nodeId);
    void nodeRemoved(const QString& nodeId);
    void nodeSelected(const QString& nodeId);
    void connectionCreated(const QString& fromId, const QString& toId);
    void connectionRemoved(const QString& fromId, const QString& toId);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void removeConnectionsForNode(const QString& nodeId);

public:
    void updateConnectionsForNode(const QString& nodeId);

    QGraphicsScene* m_scene;
    QMap<QString, FlowNodeItem*> m_nodes;
    QList<FlowConnectionItem*> m_connections;

    QString m_nextNodeId;
    int m_nodeCounter = 0;
};

/**
 * @brief 流程节点项
 */
class FlowNodeItem : public QGraphicsItem
{
public:
    explicit FlowNodeItem(const QString& nodeId, const QString& name,
                          const QString& moduleId, QGraphicsItem* parent = nullptr);

    QString nodeId() const { return m_nodeId; }
    QString moduleId() const { return m_moduleId; }
    QString name() const { return m_name; }
    void setName(const QString& name);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget = nullptr) override;

    // 端口位置
    QPointF inputPortPos(int index) const;
    QPointF outputPortPos(int index) const;
    int inputPortCount() const { return m_inputPorts; }
    int outputPortCount() const { return m_outputPorts; }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

private:
    friend class FlowCanvas;

    QString m_nodeId;
    QString m_moduleId;
    QString m_name;

    qreal m_width = 150;
    qreal m_height = 80;
    int m_inputPorts = 1;
    int m_outputPorts = 1;

    bool m_selected = false;
};

/**
 * @brief 流程连接项
 */
class FlowConnectionItem : public QGraphicsItem
{
public:
    explicit FlowConnectionItem(FlowNodeItem* fromNode, int fromPort,
                                FlowNodeItem* toNode, int toPort,
                                QGraphicsItem* parent = nullptr);
    ~FlowConnectionItem() override;

    QString fromNodeId() const;
    QString toNodeId() const;

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option,
               QWidget* widget = nullptr) override;

    void updatePath();

    // 检查连接是否有效
    bool isValid() const;

private:
    FlowNodeItem* m_fromNode;
    FlowNodeItem* m_toNode;
    int m_fromPort;
    int m_toPort;
    QPainterPath m_path;
};

} // namespace DeepLux