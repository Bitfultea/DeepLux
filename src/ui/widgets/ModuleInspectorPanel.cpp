#include "ModuleInspectorPanel.h"
#include "AppIconProvider.h"
#include "PropertyPanel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QScrollArea>
#include <QVBoxLayout>

namespace DeepLux {

ModuleInspectorPanel::ModuleInspectorPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ModuleInspectorPanel");
    setupUi();
}

ModuleInspectorPanel::~ModuleInspectorPanel() = default;

void ModuleInspectorPanel::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    setupHeader();
    setupEmptyState();
    setupTabs();
    setupBottomBar();

    // 默认显示空状态
    m_emptyState->setVisible(true);
    m_tabWidget->setVisible(false);
    m_bottomBar->setVisible(false);
}

void ModuleInspectorPanel::setupHeader()
{
    m_headerFrame = new QFrame();
    m_headerFrame->setObjectName("InspectorHeader");
    auto* headerLayout = new QHBoxLayout(m_headerFrame);
    headerLayout->setContentsMargins(8, 2, 4, 2);
    headerLayout->setSpacing(4);

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
    m_pinBtn->setToolTip(tr("固定"));
    m_pinBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Pin, 16));
    connect(m_pinBtn, &QToolButton::toggled, this, &ModuleInspectorPanel::onPinToggled);
    headerLayout->addWidget(m_pinBtn);

    m_collapseBtn = new QToolButton();
    m_collapseBtn->setObjectName("InspectorCollapseBtn");
    m_collapseBtn->setToolTip(tr("折叠"));
    m_collapseBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::ChevronRight, 16));
    connect(m_collapseBtn, &QToolButton::clicked, this, [this]() { onCollapseToggled(!m_collapsed); });
    headerLayout->addWidget(m_collapseBtn);

    m_closeBtn = new QToolButton();
    m_closeBtn->setObjectName("InspectorCloseBtn");
    m_closeBtn->setToolTip(tr("关闭"));
    m_closeBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Close, 16));
    connect(m_closeBtn, &QToolButton::clicked, this, &ModuleInspectorPanel::onCloseClicked);
    headerLayout->addWidget(m_closeBtn);

    layout()->addWidget(m_headerFrame);
}

void ModuleInspectorPanel::setupEmptyState()
{
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

void ModuleInspectorPanel::setupTabs()
{
    m_tabWidget = new QTabWidget();
    m_tabWidget->setObjectName("InspectorTabs");
    m_tabWidget->setDocumentMode(true);

    // 参数页 — 复用 PropertyPanel
    m_paramsTab = new QWidget();
    auto* paramsLayout = new QVBoxLayout(m_paramsTab);
    paramsLayout->setContentsMargins(0, 0, 0, 0);
    m_propertyPanel = new PropertyPanel(m_paramsTab);
    connect(m_propertyPanel, &PropertyPanel::paramsChanged,
            this, &ModuleInspectorPanel::onParamChanged);
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

void ModuleInspectorPanel::setupParamsTab()
{
    // 已在 setupTabs() 中创建
}

void ModuleInspectorPanel::setupResultsTab()
{
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
    m_resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultsLayout->addWidget(m_resultsTable);
}

void ModuleInspectorPanel::setupBottomBar()
{
    m_bottomBar = new QWidget();
    m_bottomBar->setObjectName("InspectorBottomBar");
    auto* bottomLayout = new QHBoxLayout(m_bottomBar);
    bottomLayout->setContentsMargins(4, 4, 4, 4);
    bottomLayout->setSpacing(4);

    // 主按钮：重新运行 — 样式由全局 ThemeManager 提供
    m_rerunBtn = new QPushButton(tr("重新运行"));
    m_rerunBtn->setObjectName("InspectorRerunBtn");
    connect(m_rerunBtn, &QPushButton::clicked, this, &ModuleInspectorPanel::onRerunClicked);
    bottomLayout->addWidget(m_rerunBtn);

    bottomLayout->addStretch();

    // 次要按钮：高级配置 — 样式由全局 ThemeManager 提供
    m_advancedBtn = new QPushButton(tr("高级配置"));
    m_advancedBtn->setObjectName("InspectorAdvancedBtn");
    connect(m_advancedBtn, &QPushButton::clicked, this, &ModuleInspectorPanel::onAdvancedClicked);
    bottomLayout->addWidget(m_advancedBtn);

    // 恢复默认降级为菜单项 — 样式由全局 ThemeManager 提供
    m_resetBtn = new QPushButton(tr("恢复默认"));
    m_resetBtn->setObjectName("InspectorResetBtn");
    m_resetBtn->setToolTip(tr("恢复默认参数"));
    connect(m_resetBtn, &QPushButton::clicked, this, &ModuleInspectorPanel::onResetClicked);
    bottomLayout->addWidget(m_resetBtn);

    layout()->addWidget(m_bottomBar);
}

void ModuleInspectorPanel::setModule(IModule* module, const QString& instanceId, const PluginInfo& info)
{
    m_currentModule = module;
    m_instanceId = instanceId;
    m_currentInfo = info;

    if (!module) {
        clear();
        return;
    }

    // 显示模块内容
    m_emptyState->setVisible(false);
    m_tabWidget->setVisible(!m_collapsed);
    m_bottomBar->setVisible(!m_collapsed);

    // 更新标题
    m_nameLabel->setText(module->name());
    m_statusLabel->setText(tr("就绪"));
    m_elapsedLabel->clear();
    m_iconLabel->setPixmap(module->icon().pixmap(20, 20));

    // 更新参数页 — 先设置 PluginInfo（含 ui.parameters），再设置模块
    m_propertyPanel->setPluginInfo(info);
    m_propertyPanel->setModule(module, instanceId);

    // 清空结果
    m_resultsTable->setRowCount(0);
}

void ModuleInspectorPanel::setOutput(const ImageData& output, bool success, int elapsedMs)
{
    if (success) {
        m_statusLabel->setText(tr("成功"));
    } else {
        m_statusLabel->setText(tr("失败"));
    }
    m_elapsedLabel->setText(tr("%1 ms").arg(elapsedMs));

    refreshResults(output);
}

void ModuleInspectorPanel::setDirty(bool dirty)
{
    m_dirty = dirty;
    // 脏状态使用标题栏小色点指示，不再用文字
    if (m_dirtyDot) {
        m_dirtyDot->setVisible(dirty);
    }
}

void ModuleInspectorPanel::clear()
{
    m_currentModule = nullptr;
    m_instanceId.clear();
    m_currentInfo = PluginInfo{};

    m_emptyState->setVisible(true);
    m_tabWidget->setVisible(false);
    m_bottomBar->setVisible(false);

    m_nameLabel->setText(tr("检查器"));
    m_statusLabel->clear();
    m_elapsedLabel->clear();
    m_iconLabel->clear();
    m_resultsTable->setRowCount(0);

    m_propertyPanel->clear();
}

void ModuleInspectorPanel::setPinned(bool pinned)
{
    m_pinned = pinned;
    m_pinBtn->blockSignals(true);
    m_pinBtn->setChecked(pinned);
    m_pinBtn->blockSignals(false);
}

void ModuleInspectorPanel::showParamsTab()
{
    if (m_tabWidget) {
        m_tabWidget->setCurrentIndex(0); // 参数页 = index 0
    }
}

void ModuleInspectorPanel::setLayoutMode(LayoutMode mode)
{
    if (m_layoutMode == mode) {
        return;
    }
    m_layoutMode = mode;

    switch (mode) {
    case LayoutMode::Docked:
        // 停靠模式：回到 splitter 中
        if (m_originalParent) {
            setParent(m_originalParent);
        }
        setWindowFlags(Qt::Widget);
        setMaximumWidth(360);
        setMinimumSize(0, 0);  // P2: 重置浮动时的最小尺寸约束
        updateCollapsedState();
        show();
        break;
    case LayoutMode::Collapsed:
        // 折叠为 32px 侧栏，只显示图标栏
        setMaximumWidth(32);
        m_tabWidget->setVisible(false);
        m_bottomBar->setVisible(false);
        break;
    case LayoutMode::Floating:
        // 浮动模式：脱离 splitter，变为独立工具窗口
        if (!m_originalParent) {
            m_originalParent = parentWidget();
        }
        setParent(nullptr);
        setWindowFlags(Qt::Tool);  // P2: 标准工具窗口，有标题栏可拖动
        setWindowTitle(tr("模块检查器"));
        setMaximumWidth(360);
        setMinimumSize(280, 400);
        resize(300, 500);
        updateCollapsedState();
        show();
        break;
    }
}

void ModuleInspectorPanel::applyTheme(bool isDark)
{
    m_isDarkTheme = isDark;
    // 颜色和尺寸由 ThemeManager 统一管理
    if (m_propertyPanel) {
        m_propertyPanel->applyTheme(isDark);
    }
}

void ModuleInspectorPanel::refreshFromModule()
{
    if (!m_currentModule) {
        return;
    }
    // 重新从当前模块读取参数，用于撤销/重做后同步显示
    m_propertyPanel->setPluginInfo(m_currentInfo);
    m_propertyPanel->setModule(m_currentModule, m_instanceId);
}

void ModuleInspectorPanel::refreshResults(const ImageData& output)
{
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
              [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
                  return a.first < b.first;
              });

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
        m_resultsTable->setItem(row, 1, new QTableWidgetItem(valueText));
        row++;
    }
}

void ModuleInspectorPanel::updateCollapsedState()
{
    if (m_collapsed) {
        m_tabWidget->setVisible(false);
        m_bottomBar->setVisible(false);
    } else {
        if (m_currentModule) {
            m_tabWidget->setVisible(true);
            m_bottomBar->setVisible(true);
        }
    }
}

// ===== Slots =====

void ModuleInspectorPanel::onPinToggled(bool checked)
{
    m_pinned = checked;
    emit pinChanged(checked);
}

void ModuleInspectorPanel::onCollapseToggled(bool collapsed)
{
    m_collapsed = collapsed;
    // 折叠时显示向左箭头（展开），展开时显示向右箭头（可折叠）
    m_collapseBtn->setIcon(AppIconProvider::icon(
        collapsed ? AppIconProvider::Icon::ChevronLeft
                  : AppIconProvider::Icon::ChevronRight,
        16));
    updateCollapsedState();
}

void ModuleInspectorPanel::onCloseClicked()
{
    emit closeRequested();
}

void ModuleInspectorPanel::onRerunClicked()
{
    emit rerunRequested();
}

void ModuleInspectorPanel::onResetClicked()
{
    if (!m_currentModule) {
        return;
    }
    // 对每个参数发 paramsChanged 信号，由 MainWindow 推入撤销栈并同步 Project
    QJsonObject defaults = m_currentModule->defaultParams();
    QJsonObject current = m_currentModule->currentParams();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const QString& key = it.key();
        // 跳过 _options 元数据键
        if (key.endsWith("_options")) {
            continue;
        }
        // 只对实际存在的参数发信号
        if (!current.contains(key) && !defaults.contains(key)) {
            continue;
        }
        QJsonValue val = it.value();
        QVariant variantValue;
        if (val.isBool()) {
            variantValue = val.toBool();
        } else if (val.isDouble()) {
            variantValue = val.toDouble();
        } else if (val.isString()) {
            variantValue = val.toString();
        } else {
            variantValue = val.toVariant();
        }
        emit paramsChanged(m_instanceId, key, variantValue);
    }
    // 刷新面板显示
    m_propertyPanel->setModule(m_currentModule, m_instanceId);
}

void ModuleInspectorPanel::onAdvancedClicked()
{
    emit advancedConfigRequested(m_instanceId);
}

void ModuleInspectorPanel::onParamChanged(const QString& /*moduleId*/, const QString& key, const QVariant& value)
{
    emit paramsChanged(m_instanceId, key, value);
}

} // namespace DeepLux
