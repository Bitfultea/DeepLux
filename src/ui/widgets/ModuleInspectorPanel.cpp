#include "ModuleInspectorPanel.h"

#include "../ThemeManager.h"
#include "AppIconProvider.h"
#include "PropertyPanel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMenu>
#include <QPair>
#include <QScrollArea>
#include <QSet>
#include <QVBoxLayout>

namespace DeepLux {

ModuleInspectorPanel::ModuleInspectorPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("ModuleInspectorPanel");
    setupUi();
}

ModuleInspectorPanel::~ModuleInspectorPanel() = default;

void ModuleInspectorPanel::setupUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    setupHeader();
    // P2: header 固定在顶部，不随内容滚动
    m_headerFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setupEmptyState();
    setupTabs();
    setupBottomBar();

    // 默认显示空状态
    m_emptyState->setVisible(!m_collapsed);
    m_tabWidget->setVisible(false);
    m_bottomBar->setVisible(false);
}

void ModuleInspectorPanel::setupHeader() {
    m_headerFrame = new QFrame();
    m_headerFrame->setObjectName("InspectorHeader");
    auto* headerLayout = new QHBoxLayout(m_headerFrame);
    headerLayout->setContentsMargins(8, 3, 4, 3);
    headerLayout->setSpacing(3);

    m_iconLabel = new QLabel();
    m_iconLabel->setObjectName("InspectorIcon");
    m_iconLabel->setFixedSize(20, 20);
    headerLayout->addWidget(m_iconLabel);

    m_nameLabel = new QLabel(tr("检查器"));
    m_nameLabel->setObjectName("InspectorModuleName");
    headerLayout->addWidget(m_nameLabel, 1);

    // 脏状态小色点：默认隐藏，由 setDirty 控制
    m_dirtyDot = new QLabel();
    m_dirtyDot->setObjectName("InspectorDirtyDot");
    m_dirtyDot->setFixedSize(10, 10);
    m_dirtyDot->setStyleSheet(QStringLiteral("background-color: #F59E0B; border-radius: 5px;"));
    m_dirtyDot->setVisible(false);
    headerLayout->addWidget(m_dirtyDot);

    m_statusLabel = new QLabel();
    m_statusLabel->setObjectName("InspectorStatus");
    headerLayout->addWidget(m_statusLabel);

    m_elapsedLabel = new QLabel();
    m_elapsedLabel->setObjectName("InspectorElapsed");
    headerLayout->addWidget(m_elapsedLabel);

    m_pinBtn = new QToolButton();
    m_pinBtn->setObjectName("InspectorPinBtn");
    m_pinBtn->setCheckable(true);
    m_pinBtn->setToolTip(tr("固定当前模块"));
    m_pinBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Pin, 16));
    connect(m_pinBtn, &QToolButton::toggled, this, &ModuleInspectorPanel::onPinToggled);
    headerLayout->addWidget(m_pinBtn);

    m_collapseBtn = new QToolButton();
    m_collapseBtn->setObjectName("InspectorCollapseBtn");
    m_collapseBtn->setToolTip(tr("折叠"));
    m_collapseBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::ChevronRight, 16));
    m_collapseBtn->setFixedSize(28, 28);
    connect(m_collapseBtn, &QToolButton::clicked, this, [this]() { onCollapseToggled(!m_collapsed); });
    headerLayout->addWidget(m_collapseBtn);

    m_closeBtn = new QToolButton();
    m_closeBtn->setObjectName("InspectorCloseBtn");
    m_closeBtn->setToolTip(tr("关闭"));
    m_closeBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Close, 16));
    m_closeBtn->setFixedSize(28, 28);
    connect(m_closeBtn, &QToolButton::clicked, this, &ModuleInspectorPanel::onCloseClicked);
    headerLayout->addWidget(m_closeBtn);

    layout()->addWidget(m_headerFrame);
}

void ModuleInspectorPanel::setupEmptyState() {
    m_emptyState = new QWidget();
    m_emptyState->setObjectName("InspectorEmptyState");
    auto* emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    auto* emptyLabel = new QLabel(tr("未选择模块"));
    emptyLabel->setObjectName("InspectorEmptyLabel");
    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLayout->addStretch();
    emptyLayout->addWidget(emptyLabel);
    emptyLayout->addStretch();

    layout()->addWidget(m_emptyState);
}

void ModuleInspectorPanel::setupTabs() {
    m_tabWidget = new QTabWidget();
    m_tabWidget->setObjectName("InspectorTabs");
    m_tabWidget->setDocumentMode(true);

    // 参数页 — 复用 PropertyPanel
    m_paramsTab = new QWidget();
    auto* paramsLayout = new QVBoxLayout(m_paramsTab);
    paramsLayout->setContentsMargins(0, 0, 0, 0);
    m_propertyPanel = new PropertyPanel(m_paramsTab);
    connect(m_propertyPanel, &PropertyPanel::paramsChanged, this, &ModuleInspectorPanel::onParamChanged);
    paramsLayout->addWidget(m_propertyPanel);
    m_tabWidget->addTab(m_paramsTab, tr("参数"));

    // 结果页
    setupResultsTab();
    m_tabWidget->addTab(m_resultsTab, tr("结果"));

    auto* mainLayout = qobject_cast<QVBoxLayout*>(layout());
    if (mainLayout) {
        mainLayout->addWidget(m_tabWidget);
        mainLayout->setStretchFactor(m_tabWidget, 1);
    } else {
        layout()->addWidget(m_tabWidget);
    }
}

void ModuleInspectorPanel::setupParamsTab() {
    // 已在 setupTabs() 中创建
}

void ModuleInspectorPanel::setupResultsTab() {
    m_resultsTab = new QWidget();
    auto* resultsLayout = new QVBoxLayout(m_resultsTab);
    resultsLayout->setContentsMargins(0, 0, 0, 0);

    m_resultsTable = new QTableWidget();
    m_resultsTable->setObjectName("InspectorResultsTable");
    m_resultsTable->setColumnCount(2);
    m_resultsTable->setHorizontalHeaderLabels(QStringList() << tr("名称") << tr("值"));
    m_resultsTable->horizontalHeader()->setStretchLastSection(true);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_resultsTable->verticalHeader()->setVisible(false);
    m_resultsTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_resultsTable->setWordWrap(true);
    m_resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultsLayout->addWidget(m_resultsTable);
}

void ModuleInspectorPanel::setupBottomBar() {
    m_bottomBar = new QWidget();
    m_bottomBar->setObjectName("InspectorBottomBar");
    auto* bottomLayout = new QHBoxLayout(m_bottomBar);
    bottomLayout->setContentsMargins(8, 6, 8, 6);
    bottomLayout->setSpacing(4);

    // 主按钮：重新运行 — 样式由全局 ThemeManager 提供
    m_rerunBtn = new QPushButton(tr("重新运行"));
    m_rerunBtn->setObjectName("InspectorRerunBtn");
    m_rerunBtn->setMinimumWidth(112);
    connect(m_rerunBtn, &QPushButton::clicked, this, &ModuleInspectorPanel::onRerunClicked);
    bottomLayout->addWidget(m_rerunBtn);

    m_moreBtn = new QToolButton();
    m_moreBtn->setObjectName("InspectorMoreBtn");
    m_moreBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Settings, 16));
    m_moreBtn->setToolTip(tr("更多操作"));
    m_moreBtn->setPopupMode(QToolButton::InstantPopup);

    auto* moreMenu = new QMenu(m_moreBtn);
    m_advancedAction = moreMenu->addAction(tr("高级配置"), this, &ModuleInspectorPanel::onAdvancedClicked);
    m_advancedAction->setObjectName("InspectorAdvancedAction");
    QAction* resetAction = moreMenu->addAction(tr("恢复默认参数"), this, &ModuleInspectorPanel::onResetClicked);
    resetAction->setObjectName("InspectorResetAction");
    m_moreBtn->setMenu(moreMenu);
    bottomLayout->addWidget(m_moreBtn);

    layout()->addWidget(m_bottomBar);
}

void ModuleInspectorPanel::setModule(IModule* module, const QString& instanceId, const PluginInfo& info) {
    m_currentModule = module;
    m_instanceId = instanceId;
    m_currentInfo = info;
    setDirty(false);

    if (!module) {
        clear();
        return;
    }

    // 显示模块内容
    m_emptyState->setVisible(false);
    m_tabWidget->setVisible(!m_collapsed);
    m_bottomBar->setVisible(!m_collapsed);

    // 阶段 6: 仅对需要独立窗口的插件显示"高级配置"
    // N点标定、MeasurementInput、ImageScript、通信硬件插件
    static const QSet<QString> advancedPlugins = {
        QStringLiteral("com.deeplux.plugin.npointcalibration"), QStringLiteral("com.deeplux.plugin.measurementinput"),
        QStringLiteral("com.deeplux.plugin.imagescript"),       QStringLiteral("com.deeplux.plugin.serialport"),
        QStringLiteral("com.deeplux.plugin.tcpclient"),         QStringLiteral("com.deeplux.plugin.tcpserver"),
        QStringLiteral("com.deeplux.plugin.plcread"),           QStringLiteral("com.deeplux.plugin.plcwrite"),
        QStringLiteral("com.deeplux.plugin.plccommunicate"),    QStringLiteral("com.deeplux.plugin.strformat"),
    };
    if (m_advancedAction) {
        m_advancedAction->setVisible(advancedPlugins.contains(module->moduleId()));
    }

    // 更新标题
    m_nameLabel->setText(module->name());
    m_statusLabel->setText(tr("就绪"));
    m_statusLabel->setStyleSheet(QString());
    m_elapsedLabel->clear();
    m_iconLabel->setPixmap(module->icon().pixmap(20, 20));

    // 更新参数页 — 先设置 PluginInfo（含 ui.parameters），再设置模块
    m_propertyPanel->setPluginInfo(info);
    m_propertyPanel->setModule(module, instanceId);

    // 清空结果
    m_resultsTable->setRowCount(0);
}

void ModuleInspectorPanel::setOutput(const ImageData& output, bool success, int elapsedMs) {
    setDirty(false);
    if (success) {
        m_statusLabel->setText(tr("成功"));
        m_statusLabel->setStyleSheet("color: #22C55E; font-size: 13px;");
    } else {
        m_statusLabel->setText(tr("失败"));
        m_statusLabel->setStyleSheet("color: #EF4444; font-size: 13px;");
    }
    m_elapsedLabel->setText(tr("%1 ms").arg(elapsedMs));

    if (!success) {
        // P1: 模块失败时显示错误原因
        m_resultsTable->setRowCount(0);
        const QMap<QString, QVariant> all = output.allData();
        QString errorMsg = all.value("error").toString();
        if (errorMsg.isEmpty())
            errorMsg = all.value("error_message").toString();
        if (errorMsg.isEmpty())
            errorMsg = tr("执行失败，请查看日志获取详细信息");
        m_resultsTable->insertRow(0);
        m_resultsTable->setItem(0, 0, new QTableWidgetItem(tr("错误")));
        auto* errorItem = new QTableWidgetItem(errorMsg);
        errorItem->setForeground(QColor("#EF4444"));
        errorItem->setToolTip(errorMsg);
        m_resultsTable->setItem(0, 1, errorItem);
        m_resultsTable->resizeRowsToContents();
    } else {
        refreshResults(output);
    }
}

void ModuleInspectorPanel::setDirty(bool dirty) {
    m_dirty = dirty;
    // 脏状态使用标题栏小色点指示，不再用文字
    if (m_dirtyDot) {
        m_dirtyDot->setVisible(dirty);
    }
    if (dirty) {
        m_statusLabel->clear();
        m_statusLabel->setStyleSheet(QString());
        m_elapsedLabel->clear();
        m_resultsTable->setRowCount(0);
    }
}

void ModuleInspectorPanel::clear() {
    m_currentModule = nullptr;
    m_instanceId.clear();
    m_currentInfo = PluginInfo{};

    m_emptyState->setVisible(!m_collapsed);
    m_tabWidget->setVisible(false);
    m_bottomBar->setVisible(false);

    m_nameLabel->setText(tr("检查器"));
    m_statusLabel->clear();
    m_statusLabel->setStyleSheet(QString());
    m_elapsedLabel->clear();
    m_iconLabel->clear();
    m_resultsTable->setRowCount(0);
    setDirty(false);

    m_propertyPanel->clear();
}

void ModuleInspectorPanel::setPinned(bool pinned) {
    m_pinned = pinned;
    m_pinBtn->blockSignals(true);
    m_pinBtn->setChecked(pinned);
    m_pinBtn->setToolTip(pinned ? tr("取消固定") : tr("固定当前模块"));
    m_pinBtn->blockSignals(false);
}

void ModuleInspectorPanel::showParamsTab() {
    if (m_tabWidget) {
        m_tabWidget->setCurrentIndex(0); // 参数页 = index 0
    }
}

void ModuleInspectorPanel::setLayoutMode(LayoutMode mode) {
    if (m_layoutMode == mode) {
        return;
    }
    m_layoutMode = mode;

    const LayoutMetrics metrics = ThemeManager::layoutMetrics();
    switch (mode) {
    case LayoutMode::Docked:
        // 停靠模式：回到 splitter 中
        if (m_originalParent) {
            setParent(m_originalParent);
        }
        setWindowFlags(Qt::Widget);
        setMaximumWidth(metrics.inspectorMaxWidth);
        setMinimumWidth(metrics.inspectorMinWidth);
        setMinimumSize(0, 0);
        onCollapseToggled(false);
        updateCollapsedState();
        show();
        break;
    case LayoutMode::Collapsed:
        // P1-3: 折叠模式 — 同步最小宽度和 m_collapsed 状态
        setMinimumWidth(32);
        setMaximumWidth(32);
        onCollapseToggled(true);
        break;
    case LayoutMode::Floating:
        // 浮动模式：脱离 splitter，变为独立工具窗口
        if (!m_originalParent) {
            m_originalParent = parentWidget();
        }
        setParent(nullptr);
        setWindowFlags(Qt::Tool); // P2: 标准工具窗口，有标题栏可拖动
        setWindowTitle(tr("模块检查器"));
        setMaximumWidth(metrics.inspectorMaxWidth);
        setMinimumSize(280, 400);
        resize(300, 500);
        updateCollapsedState();
        show();
        break;
    }
}

void ModuleInspectorPanel::applyTheme(bool isDark) {
    m_isDarkTheme = isDark;
    // P1: 结果表也需要主题
    const QString bg = isDark ? "#252525" : "#ffffff";
    const QString fg = isDark ? "#ffffff" : "#212121";
    const QString gridCol = isDark ? "#333333" : "#eeeeee";
    if (m_resultsTable) {
        m_resultsTable->setStyleSheet(
            QString("QTableWidget { background-color: %1; color: %2; border: none; gridline-color: %3; }"
                    "QTableWidget::item { border-bottom: 1px solid %3; }"
                    "QHeaderView::section { background-color: %1; color: %2; padding: 4px; border: none; }")
                .arg(bg, fg, gridCol));
    }
    if (m_propertyPanel) {
        m_propertyPanel->applyTheme(isDark);
    }
}

void ModuleInspectorPanel::refreshFromModule() {
    if (!m_currentModule) {
        return;
    }
    // 重新从当前模块读取参数，用于撤销/重做后同步显示
    m_propertyPanel->setPluginInfo(m_currentInfo);
    m_propertyPanel->setModule(m_currentModule, m_instanceId);
}

void ModuleInspectorPanel::refreshResults(const ImageData& output) {
    m_resultsTable->setRowCount(0);
    if (!output.isValid()) {
        return;
    }

    const QMap<QString, QVariant> all = output.allData();
    const QJsonObject uiResults = m_currentInfo.ui.value("results").toObject();

    // 根据 ui.results 中的 order 字段排序
    QList<QPair<int, QString>> ordered;
    QStringList unordered;
    for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
        const QString& key = it.key();
        // 跳过图像数据本身（不重复显示）
        if (key == "image" || key == "qimage" || key == "pixmap") {
            continue;
        }
        QJsonObject meta = uiResults.value(key).toObject();
        if (meta.contains("order")) {
            ordered.append({meta.value("order").toInt(), key});
        } else {
            unordered.append(key);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const QPair<int, QString>& a, const QPair<int, QString>& b) { return a.first < b.first; });

    QStringList sortedKeys;
    for (const auto& p : ordered) {
        sortedKeys.append(p.second);
    }
    sortedKeys.append(unordered);

    // 如果没有 ui.results 描述，通过 allData() 显示原始键值
    int row = 0;
    for (const QString& key : sortedKeys) {
        QVariant value = all.value(key);

        // 获取元数据描述
        QJsonObject meta = uiResults.value(key).toObject();
        QString labelText = meta.value("label").toString(key);
        QString unit = meta.value("unit").toString();
        if (!unit.isEmpty()) {
            labelText = QString("%1 (%2)").arg(labelText, unit);
        }

        // 格式化值
        int precision = meta.value("precision").toInt(-1);
        QString valueText;
        if (value.canConvert<double>() && precision >= 0) {
            valueText = QString::number(value.toDouble(), 'f', precision);
        } else if (value.canConvert<QVariantList>()) {
            // 复杂值（大数组）只显示摘要
            QVariantList list = value.toList();
            valueText = tr("[%1 项]").arg(list.size());
        } else {
            valueText = value.toString();
        }

        m_resultsTable->insertRow(row);
        m_resultsTable->setItem(row, 0, new QTableWidgetItem(labelText));
        auto* valueItem = new QTableWidgetItem(valueText);
        valueItem->setToolTip(valueText);
        m_resultsTable->setItem(row, 1, valueItem);
        row++;
    }
    m_resultsTable->resizeRowsToContents();
}

void ModuleInspectorPanel::updateCollapsedState() {
    if (m_collapsed) {
        if (m_emptyState)
            m_emptyState->setVisible(false);
        m_tabWidget->setVisible(false);
        m_bottomBar->setVisible(false);
        if (m_nameLabel)
            m_nameLabel->setVisible(false);
        if (m_statusLabel)
            m_statusLabel->setVisible(false);
        if (m_elapsedLabel)
            m_elapsedLabel->setVisible(false);
        if (m_dirtyDot)
            m_dirtyDot->setVisible(false);
        if (m_pinBtn)
            m_pinBtn->setVisible(false);
        if (m_iconLabel)
            m_iconLabel->setVisible(false);
        // 高: 折叠时隐藏关闭按钮，只保留展开按钮，避免 32px 内重叠
        if (m_closeBtn)
            m_closeBtn->setVisible(false);
    } else {
        if (m_emptyState)
            m_emptyState->setVisible(!m_currentModule);
        if (m_currentModule) {
            m_tabWidget->setVisible(true);
            m_bottomBar->setVisible(true);
        }
        if (m_nameLabel)
            m_nameLabel->setVisible(true);
        if (m_statusLabel)
            m_statusLabel->setVisible(true);
        if (m_elapsedLabel)
            m_elapsedLabel->setVisible(true);
        // 中: 脏状态恢复时用 m_dirty 而非无条件 true
        if (m_dirtyDot)
            m_dirtyDot->setVisible(m_dirty);
        if (m_pinBtn)
            m_pinBtn->setVisible(true);
        if (m_iconLabel)
            m_iconLabel->setVisible(true);
        if (m_closeBtn)
            m_closeBtn->setVisible(true);
    }
}

// ===== Slots =====

void ModuleInspectorPanel::onPinToggled(bool checked) {
    m_pinned = checked;
    m_pinBtn->setToolTip(checked ? tr("取消固定") : tr("固定当前模块"));
    emit pinChanged(checked);
}

void ModuleInspectorPanel::onCollapseToggled(bool collapsed) {
    m_collapsed = collapsed;
    m_collapseBtn->setIcon(AppIconProvider::icon(
        collapsed ? AppIconProvider::Icon::ChevronLeft : AppIconProvider::Icon::ChevronRight, 16));
    // 高: 手动折叠时调整自身宽度
    if (collapsed) {
        setMaximumWidth(32);
        setMinimumWidth(32);
    } else {
        const LayoutMetrics metrics = ThemeManager::layoutMetrics();
        setMaximumWidth(metrics.inspectorMaxWidth);
        setMinimumWidth(metrics.inspectorMinWidth);
    }
    updateCollapsedState();
    // 通知 MainWindow 重分配 splitter 空间
    emit collapseToggled(collapsed);
}

void ModuleInspectorPanel::onCloseClicked() {
    emit closeRequested();
}

void ModuleInspectorPanel::onRerunClicked() {
    emit rerunRequested();
}

void ModuleInspectorPanel::onResetClicked() {
    if (!m_currentModule) {
        return;
    }
    // P1-7: 发单个 resetDefaultsRequested 信号，由 MainWindow 用 beginMacro/endMacro 批量推入撤销栈
    emit resetDefaultsRequested(m_instanceId);
    // 刷新面板显示
    m_propertyPanel->setModule(m_currentModule, m_instanceId);
}

void ModuleInspectorPanel::onAdvancedClicked() {
    emit advancedConfigRequested(m_instanceId);
}

void ModuleInspectorPanel::onParamChanged(const QString& /*moduleId*/, const QString& key, const QVariant& value) {
    emit paramsChanged(m_instanceId, key, value);
}

} // namespace DeepLux
