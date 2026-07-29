#include "ProcessTreeController.h"

#include "../widgets/AppIconProvider.h"
#include "core/base/ModuleBase.h"
#include "core/common/Logger.h"
#include "core/engine/RunEngine.h"
#include "core/interface/IModule.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>

namespace DeepLux {

ProcessTreeController::ProcessTreeController(QTreeWidget* tree, QObject* parent)
    : QObject(parent), m_tree(tree) {
    if (m_tree) {
        connect(m_tree, &QTreeWidget::customContextMenuRequested,
                this, &ProcessTreeController::onContextMenu);
    }
}

ProcessTreeController::~ProcessTreeController() {
    if (m_hintLabel) {
        m_hintLabel->deleteLater();
        m_hintLabel = nullptr;
    }
}

void ProcessTreeController::setFlowModules(QMap<QString, IModule*>* modules) {
    m_flowModules = modules;
}

void ProcessTreeController::setModulesNeedSyncFlag(bool* flag) {
    m_modulesNeedSync = flag;
}

void ProcessTreeController::setMeasurementPickMaps(QMap<QString, int>* cursor, QMap<QString, int>* count) {
    m_measurementPickCursor = cursor;
    m_measurementPickCount = count;
}

void ProcessTreeController::setToolDisplayNameCallback(
    std::function<QString(const QString&, const QString&)> callback) {
    m_toolDisplayNameCallback = std::move(callback);
}

void ProcessTreeController::setClearMeasurementOverlaysCallback(std::function<void()> callback) {
    m_clearMeasurementOverlaysCallback = std::move(callback);
}

// ========== 主要操作 ==========

void ProcessTreeController::addModule(const ModuleInstance& inst) {
    if (m_instanceItemMap.contains(inst.id))
        return; // 已存在，防止重复

    hideHintLabel();

    QString displayName = inst.name;
    if (m_toolDisplayNameCallback) {
        displayName = m_toolDisplayNameCallback(inst.moduleId, inst.name);
    }

    QTreeWidgetItem* newItem = new QTreeWidgetItem();
    newItem->setFlags((newItem->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    newItem->setText(0, displayName);
    m_tree->addTopLevelItem(newItem);

    m_usedNames.insert(inst.id);

    if (m_flowModules) {
        PluginManager& pm = PluginManager::instance();
        IModule* module = pm.createModule(inst.moduleId);
        if (module) {
            newItem->setIcon(0, module->icon());
            if (!inst.params.isEmpty()) {
                module->setParams(inst.params);
            }
            if (module->initialize()) {
                m_flowModules->insert(inst.id, module);
                if (!module->icon().isNull()) {
                    newItem->setIcon(0, module->icon());
                }
            } else {
                delete module;
                module = nullptr;
                Logger::instance().warning(
                    QObject::tr("模块初始化失败：%1").arg(inst.moduleId), "Flow");
            }
        } else {
            Logger::instance().warning(
                QObject::tr("模块不支持克隆，无法创建运行时实例：%1").arg(inst.moduleId), "Flow");
        }
    }

    newItem->setData(0, Qt::UserRole, "flow_item");
    newItem->setData(0, Qt::UserRole + 1, inst.id);
    newItem->setData(0, Qt::UserRole + 2, inst.moduleId);

    m_instanceItemMap.insert(inst.id, newItem);
    markModulesNeedSync();
}

void ProcessTreeController::removeModule(const QString& instanceId) {
    if (RunEngine::instance().isBusy()) {
        Logger::instance().warning(QObject::tr("流程运行中，无法删除模块"), "Flow");
        return;
    }
    QTreeWidgetItem* item = m_instanceItemMap.value(instanceId);
    if (item) {
        int idx = m_tree->indexOfTopLevelItem(item);
        if (idx >= 0) {
            m_tree->takeTopLevelItem(idx);
        }
        delete item;
        m_instanceItemMap.remove(instanceId);
    }

    bool removedMeasurementInput = false;
    if (m_flowModules && m_flowModules->contains(instanceId)) {
        IModule* module = m_flowModules->take(instanceId);
        if (module) {
            removedMeasurementInput =
                module->moduleId() == QStringLiteral("com.deeplux.plugin.measurementinput");
            module->shutdown();
            delete module;
        }
    }

    m_usedNames.remove(instanceId);
    if (m_measurementPickCursor) {
        m_measurementPickCursor->remove(instanceId);
    }
    if (m_measurementPickCount) {
        m_measurementPickCount->remove(instanceId);
    }
    if (removedMeasurementInput && m_clearMeasurementOverlaysCallback) {
        m_clearMeasurementOverlaysCallback();
    }
    markModulesNeedSync();

    if (m_tree->topLevelItemCount() == 0 && !m_hintLabel) {
        showHintLabel();
    }
}

void ProcessTreeController::removeFlowModule(const QString& instanceId) {
    if (instanceId.isEmpty()) {
        return;
    }
    if (RunEngine::instance().isBusy()) {
        Logger::instance().warning(QObject::tr("流程运行中，无法删除模块"), "Flow");
        return;
    }

    Project* project = ProjectManager::instance().currentProject();
    if (project && project->findModule(instanceId)) {
        project->removeModule(instanceId);
        return;
    }

    removeModule(instanceId);
}

void ProcessTreeController::clear() {
    if (m_flowModules) {
        for (IModule* module : *m_flowModules) {
            if (module) {
                module->shutdown();
                delete module;
            }
        }
        m_flowModules->clear();
    }
    m_usedNames.clear();
    m_instanceItemMap.clear();
    if (m_measurementPickCursor) {
        m_measurementPickCursor->clear();
    }
    if (m_measurementPickCount) {
        m_measurementPickCount->clear();
    }
    if (m_clearMeasurementOverlaysCallback) {
        m_clearMeasurementOverlaysCallback();
    }
    m_tree->clear();

    if (!m_hintLabel) {
        showHintLabel();
    }
    markModulesNeedSync();
    emit modulesCleared();
}

// ========== 树控件访问 ==========

QTreeWidget* ProcessTreeController::tree() const {
    return m_tree;
}

QTreeWidgetItem* ProcessTreeController::currentItem() const {
    return m_tree->currentItem();
}

void ProcessTreeController::setCurrentItem(QTreeWidgetItem* item) {
    m_tree->setCurrentItem(item);
}

int ProcessTreeController::topLevelItemCount() const {
    return m_tree->topLevelItemCount();
}

QTreeWidgetItem* ProcessTreeController::topLevelItem(int index) const {
    return m_tree->topLevelItem(index);
}

int ProcessTreeController::indexOfTopLevelItem(QTreeWidgetItem* item) const {
    return m_tree->indexOfTopLevelItem(item);
}

void ProcessTreeController::insertTopLevelItem(int index, QTreeWidgetItem* item) {
    m_tree->insertTopLevelItem(index, item);
}

QTreeWidgetItem* ProcessTreeController::takeTopLevelItem(int index) {
    return m_tree->takeTopLevelItem(index);
}

QList<QTreeWidgetItem*> ProcessTreeController::selectedItems() const {
    return m_tree->selectedItems();
}

void ProcessTreeController::clearSelection() {
    m_tree->clearSelection();
}

// ========== 实例映射访问 ==========

QTreeWidgetItem* ProcessTreeController::instanceItem(const QString& instanceId) const {
    return m_instanceItemMap.value(instanceId, nullptr);
}

bool ProcessTreeController::containsInstance(const QString& instanceId) const {
    return m_instanceItemMap.contains(instanceId);
}

void ProcessTreeController::insertInstanceItem(const QString& instanceId, QTreeWidgetItem* item) {
    m_instanceItemMap.insert(instanceId, item);
}

void ProcessTreeController::setInstanceItem(const QString& instanceId, QTreeWidgetItem* item) {
    m_instanceItemMap[instanceId] = item;
}

void ProcessTreeController::clearInstanceItemMap() {
    m_instanceItemMap.clear();
}

void ProcessTreeController::rebuildInstanceItemMap() {
    m_instanceItemMap.clear();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        QString instanceName = item->data(0, Qt::UserRole + 1).toString();
        if (!instanceName.isEmpty()) {
            m_instanceItemMap[instanceName] = item;
        }
    }
}

// ========== 已用名称管理 ==========

bool ProcessTreeController::isUsedName(const QString& name) const {
    return m_usedNames.contains(name);
}

void ProcessTreeController::insertUsedName(const QString& name) {
    m_usedNames.insert(name);
}

void ProcessTreeController::removeUsedName(const QString& name) {
    m_usedNames.remove(name);
}

void ProcessTreeController::clearUsedNames() {
    m_usedNames.clear();
}

// ========== 提示标签 ==========

void ProcessTreeController::showHintLabel() {
    if (m_hintLabel) {
        return;
    }
    QWidget* parentWidget = m_tree->parentWidget();
    if (!parentWidget) {
        return;
    }
    QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(parentWidget->layout());
    if (!layout) {
        return;
    }
    m_hintLabel = new QLabel(QObject::tr("← 从左侧拖拽工具"));
    m_hintLabel->setAlignment(Qt::AlignCenter);
    m_hintLabel->setStyleSheet("color: #808080; padding: 10px;");
    m_hintLabel->setObjectName("ProcessTreeHintLabel");
    int index = layout->indexOf(m_tree);
    layout->insertWidget(index, m_hintLabel);
}

void ProcessTreeController::hideHintLabel() {
    if (m_hintLabel) {
        m_hintLabel->setVisible(false);
        m_hintLabel->deleteLater();
        m_hintLabel = nullptr;
    }
}

bool ProcessTreeController::hasHintLabel() const {
    return m_hintLabel != nullptr;
}

// ========== 同步标志 ==========

void ProcessTreeController::markModulesNeedSync() {
    if (m_modulesNeedSync) {
        *m_modulesNeedSync = true;
    }
}

// ========== 私有槽 ==========

void ProcessTreeController::onContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if (!item || item->data(0, Qt::UserRole).toString() != "flow_item") {
        return;
    }

    QMenu menu(m_tree);
    QAction* deleteAction = menu.addAction(QObject::tr("删除"));
    deleteAction->setIcon(
        AppIconProvider::icon(AppIconProvider::Icon::Delete, 18, QColor("#DC2626")));

    QAction* selectedAction = menu.exec(m_tree->mapToGlobal(pos));
    if (selectedAction == deleteAction) {
        QString instanceName = item->data(0, Qt::UserRole + 1).toString();
        removeFlowModule(instanceName);
    }
}

void ProcessTreeController::onModuleAddedFromProject(const ModuleInstance& module) {
    addModule(module);
    emit moduleAdded(module);
}

void ProcessTreeController::onModuleRemovedFromProject(const QString& instanceId) {
    removeModule(instanceId);
    emit moduleRemoved(instanceId);
}

} // namespace DeepLux
