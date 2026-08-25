#pragma once

#include "core/model/Project.h"

#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <functional>

class QLabel;

namespace DeepLux {
class IModule;

class ProcessTreeController : public QObject {
    Q_OBJECT
public:
    explicit ProcessTreeController(QTreeWidget* tree, QObject* parent = nullptr);
    ~ProcessTreeController() override;

    // 引用 MainWindow 中保留的数据
    void setFlowModules(QMap<QString, IModule*>* modules);
    void setModulesNeedSyncFlag(bool* flag);
    void setMeasurementPickMaps(QMap<QString, int>* cursor, QMap<QString, int>* count);
    void setToolDisplayNameCallback(std::function<QString(const QString&, const QString&)> callback);
    void setClearMeasurementOverlaysCallback(std::function<void()> callback);

    // 主要操作
    void addModule(const ModuleInstance& inst);
    void removeModule(const QString& instanceId);
    void removeFlowModule(const QString& instanceId);
    void clear();

    // 树控件访问（供 MainWindow 的 eventFilter / executeFlowOnce 使用）
    QTreeWidget* tree() const;
    QTreeWidgetItem* currentItem() const;
    void setCurrentItem(QTreeWidgetItem* item);
    int topLevelItemCount() const;
    QTreeWidgetItem* topLevelItem(int index) const;
    int indexOfTopLevelItem(QTreeWidgetItem* item) const;
    void insertTopLevelItem(int index, QTreeWidgetItem* item);
    QTreeWidgetItem* takeTopLevelItem(int index);
    QList<QTreeWidgetItem*> selectedItems() const;
    void clearSelection();

    // 实例映射访问（供 MainWindow 的拖放 / ensureMeasurementInputForMode 使用）
    QTreeWidgetItem* instanceItem(const QString& instanceId) const;
    bool containsInstance(const QString& instanceId) const;
    void insertInstanceItem(const QString& instanceId, QTreeWidgetItem* item);
    void setInstanceItem(const QString& instanceId, QTreeWidgetItem* item);
    void clearInstanceItemMap();
    void rebuildInstanceItemMap();

    // 已用名称管理
    bool isUsedName(const QString& name) const;
    void insertUsedName(const QString& name);
    void removeUsedName(const QString& name);
    void clearUsedNames();

    // 提示标签
    void showHintLabel();
    void hideHintLabel();
    bool hasHintLabel() const;

    // 同步标志
    void markModulesNeedSync();

signals:
    void moduleAdded(const ModuleInstance& module);
    void moduleRemoved(const QString& instanceId);
    void modulesCleared();
    void moduleBeingRemoved(const QString& instanceId); // Fix: 删除前通知，让 MainWindow 清空检查器和撤销栈

public slots:
    void onModuleAddedFromProject(const ModuleInstance& module);
    void onModuleRemovedFromProject(const QString& instanceId);

private slots:
    void onContextMenu(const QPoint& pos);

private:
    QTreeWidget* m_tree;
    QMap<QString, QTreeWidgetItem*> m_instanceItemMap;
    QSet<QString> m_usedNames;
    QLabel* m_hintLabel = nullptr;

    QMap<QString, IModule*>* m_flowModules = nullptr;
    bool* m_modulesNeedSync = nullptr;
    QMap<QString, int>* m_measurementPickCursor = nullptr;
    QMap<QString, int>* m_measurementPickCount = nullptr;

    std::function<QString(const QString&, const QString&)> m_toolDisplayNameCallback;
    std::function<void()> m_clearMeasurementOverlaysCallback;
};

} // namespace DeepLux
