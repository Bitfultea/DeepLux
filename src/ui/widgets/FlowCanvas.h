#pragma once

#include "deeplux/DataContract.h"
#include "core/manager/PluginManager.h"

#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QWidget>

namespace DeepLux {

class FlowNodeItem;
class FlowConnectionItem;
class Project;

/**
 * @brief 流程画布
 *
 * 显示和编辑模块流程图。端口使用字符串 ID（来自 PortSpec），
 * 连接按完整 4 元组（源/源端口/目标/目标端口）管理。
 */
class FlowCanvas : public QGraphicsView {
    Q_OBJECT

public:
    explicit FlowCanvas(QWidget* parent = nullptr);
    ~FlowCanvas() override;

    // 节点操作
    QString addNode(const QString& moduleId, const QString& name, const QPointF& pos,
                    const QString& instanceId = QString());
    void removeNode(const QString& nodeId);
    void clearNodes();
    void loadFromProject(Project* project);

    // 连接操作（字符串端口 ID）
    void addConnection(const QString& fromNodeId, const QString& fromPortId,
                       const QString& toNodeId, const QString& toPortId,
                       const QString& edgeType = QStringLiteral("data"));
    void removeConnection(const QString& fromNodeId, const QString& fromPortId,
                           const QString& toNodeId, const QString& toPortId);
    void clearConnections();

    // 查询
    QStringList nodeIds() const;
    FlowNodeItem* nodeItem(const QString& nodeId) const;
    QList<FlowNodeItem*> getOrderedModules() const;

    // 按 ID 选择节点（程序化调用）
    void selectNode(const QString& nodeId);
    void setNodeExecutionState(const QString& nodeId, const QString& status, const QString& timeText);

    // 端口兼容性检查
    static bool portsCompatible(const PortSpec& fromPort, const PortSpec& toPort);

signals:
    void nodeAdded(const QString& nodeId);
    void nodeRemoved(const QString& nodeId);
    void nodeSelected(const QString& nodeId);
    void connectionCreated(const QString& fromId, const QString& toId);
    void connectionRemoved(const QString& fromId, const QString& toId);
    void connectionRequest(const QString& fromId, const QString& fromPort,
                           const QString& toId, const QString& toPort);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;

private:
    void removeConnectionsForNode(const QString& nodeId);
    static QString connectionKey(const QString& fromId, const QString& fromPort,
                                  const QString& toId, const QString& toPort);

public:
    void updateConnectionsForNode(const QString& nodeId);
    void applyTheme(bool isDark);

    QGraphicsScene* m_scene;
    QMap<QString, FlowNodeItem*> m_nodes;
    QList<FlowConnectionItem*> m_connections;

    QString m_nextNodeId;
    int m_nodeCounter = 0;
    bool m_loadingProject = false;
    bool m_syncingFromProject = false; // P0-fix: 防止 Project→Canvas→Project 回写循环
};

/**
 * @brief 流程节点项
 */
class FlowNodeItem : public QGraphicsItem {
public:
    explicit FlowNodeItem(const QString& nodeId, const QString& name, const QString& moduleId,
                          QGraphicsItem* parent = nullptr);

    QString nodeId() const { return m_nodeId; }
    QString moduleId() const { return m_moduleId; }
    QString name() const { return m_name; }
    void setName(const QString& name);
    void setExecutionState(const QString& status, const QString& timeText);

    // 端口声明
    void setPortSpecs(const QList<PortSpec>& inputs, const QList<PortSpec>& outputs);
    const QList<PortSpec>& inputPortSpecs() const { return m_inputPortSpecs; }
    const QList<PortSpec>& outputPortSpecs() const { return m_outputPortSpecs; }

    // 端口位置（按端口 ID 查找）
    QPointF inputPortPos(const QString& portId) const;
    QPointF outputPortPos(const QString& portId) const;
    QPointF inputPortPos(int index) const;
    QPointF outputPortPos(int index) const;
    int inputPortCount() const { return m_inputPortSpecs.size(); }
    int outputPortCount() const { return m_outputPortSpecs.size(); }

    // 端口命中测试
    QString inputPortAt(const QPointF& pos) const;
    QString outputPortAt(const QPointF& pos) const;

    // 视觉状态
    void setDisabledVisual(bool disabled) { m_disabledVisual = disabled; update(); }
    void setBreakpointVisual(bool bp) { m_breakpointVisual = bp; update(); }
    bool isDisabledVisual() const { return m_disabledVisual; }
    bool hasBreakpointVisual() const { return m_breakpointVisual; }

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    friend class FlowCanvas;

    QString m_nodeId;
    QString m_moduleId;
    QString m_name;

    qreal m_width = 220;
    qreal m_height = 64;
    QList<PortSpec> m_inputPortSpecs;
    QList<PortSpec> m_outputPortSpecs;

    QString m_status;
    QString m_timeText;
    bool m_disabledVisual = false;
    bool m_breakpointVisual = false;

    // 拖线状态
    bool m_draggingConnection = false;
    QString m_dragFromPortId;
    QPointF m_dragCurrentPos;
};

/**
 * @brief 流程连接项
 */
class FlowConnectionItem : public QGraphicsItem {
public:
    explicit FlowConnectionItem(FlowNodeItem* fromNode, const QString& fromPortId,
                                FlowNodeItem* toNode, const QString& toPortId,
                                const QString& edgeType = QStringLiteral("data"),
                                QGraphicsItem* parent = nullptr);
    ~FlowConnectionItem() override;

    QString fromNodeId() const;
    QString toNodeId() const;
    QString fromPortId() const { return m_fromPortId; }
    QString toPortId() const { return m_toPortId; }
    QString edgeType() const { return m_edgeType; }
    bool isControlEdge() const { return m_edgeType == QLatin1String("control"); }

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;

    void updatePath();
    bool isValid() const;

private:
    FlowNodeItem* m_fromNode;
    FlowNodeItem* m_toNode;
    QString m_fromPortId;
    QString m_toPortId;
    QString m_edgeType;
    QPainterPath m_path;
};

} // namespace DeepLux
