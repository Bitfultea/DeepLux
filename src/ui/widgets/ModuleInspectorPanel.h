#pragma once

#include "core/interface/IModule.h"
#include "core/manager/PluginManager.h"
#include "core/model/ImageData.h"

#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QWidget>

namespace DeepLux {

/**
 * @brief 模块检查器面板
 *
 * 显示当前选中模块的参数、结果和状态信息。
 * 包含标题栏（模块名、状态、耗时、固定、折叠、关闭）、
 * 参数/结果两个页签、底部操作按钮和空状态。
 */
class ModuleInspectorPanel : public QWidget {
    Q_OBJECT

public:
    /// 检查器布局模式
    enum class LayoutMode {
        Docked,    ///< 宽屏：停靠在 RightTopSplitter
        Collapsed, ///< 中等宽度：折叠为 32px 侧栏
        Floating,  ///< 小窗口：非模态浮动工具窗
    };
    Q_ENUM(LayoutMode)

    explicit ModuleInspectorPanel(QWidget* parent = nullptr);
    ~ModuleInspectorPanel() override;

    /**
     * 设置当前检查的模块。
     * @param module 模块实例指针（可能为 nullptr）
     * @param instanceId 实例 ID
     * @param info 插件信息
     */
    void setModule(IModule* module, const QString& instanceId, const PluginInfo& info);

    /**
     * 设置模块输出结果。
     * @param output 输出图像数据
     * @param success 是否成功
     * @param elapsedMs 耗时（毫秒）
     */
    void setOutput(const ImageData& output, bool success, int elapsedMs);

    /**
     * 设置模块脏标记。
     */
    void setDirty(bool dirty);

    /**
     * 清除检查器内容，回到空状态。
     */
    void clear();

    /**
     * 设置固定状态。
     */
    void setPinned(bool pinned);

    /**
     * @return 当前实例 ID
     */
    QString currentInstanceId() const {
        return m_instanceId;
    }

    /**
     * @return 是否已固定
     */
    bool isPinned() const {
        return m_pinned;
    }

    /**
     * @return 是否处于折叠状态
     */
    bool isCollapsed() const {
        return m_collapsed;
    }

    /**
     * 切换到参数页签。
     */
    void showParamsTab();

    /**
     * @return 当前布局模式
     */
    LayoutMode layoutMode() const {
        return m_layoutMode;
    }

    /**
     * 设置布局模式（由 MainWindow 自适应逻辑调用）。
     */
    void setLayoutMode(LayoutMode mode);

    /**
     * 标记用户已手动指定模式，自适应切换不再自动发生。
     */
    void setUserOverrideMode(bool override) {
        m_userOverrideMode = override;
    }
    bool userOverrideMode() const {
        return m_userOverrideMode;
    }

    /**
     * 应用主题。
     */
    void applyTheme(bool isDark);

    /**
     * @brief 从当前模块重新读取参数并刷新控件（用于撤销/重做后同步显示）。
     */
    void refreshFromModule();

signals:
    void paramsChanged(const QString& instanceId, const QString& key, const QVariant& value);
    void resetDefaultsRequested(const QString& instanceId);
    void rerunRequested();
    void advancedConfigRequested(const QString& instanceId);
    void pinChanged(bool pinned);
    void closeRequested();
    void collapseToggled(bool collapsed); // 高: 手动折叠时通知 MainWindow 调整 splitter

private slots:
    void onPinToggled(bool checked);
    void onCollapseToggled(bool checked);
    void onCloseClicked();
    void onRerunClicked();
    void onResetClicked();
    void onAdvancedClicked();
    void onParamChanged(const QString& moduleId, const QString& key, const QVariant& value);

private:
    void setupUi();
    void setupHeader();
    void setupTabs();
    void setupParamsTab();
    void setupResultsTab();
    void setupBottomBar();
    void setupEmptyState();
    void updateCollapsedState();
    void refreshResults(const ImageData& output);

    // 标题栏控件
    QFrame* m_headerFrame = nullptr;
    QLabel* m_iconLabel = nullptr;
    QLabel* m_nameLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_elapsedLabel = nullptr;
    QLabel* m_dirtyDot = nullptr; // 脏状态小色点
    QToolButton* m_pinBtn = nullptr;
    QToolButton* m_collapseBtn = nullptr;
    QToolButton* m_closeBtn = nullptr;

    // Tab 容器
    QTabWidget* m_tabWidget = nullptr;

    // 参数页（复用 PropertyPanel 的渲染逻辑）
    class PropertyPanel* m_propertyPanel = nullptr;
    QWidget* m_paramsTab = nullptr;

    // 结果页
    QTableWidget* m_resultsTable = nullptr;
    QWidget* m_resultsTab = nullptr;

    // 底部按钮栏
    QWidget* m_bottomBar = nullptr;
    QPushButton* m_rerunBtn = nullptr;
    QToolButton* m_moreBtn = nullptr;

    // 空状态
    QWidget* m_emptyState = nullptr;

    // 当前状态
    IModule* m_currentModule = nullptr;
    QString m_instanceId;
    PluginInfo m_currentInfo;
    bool m_pinned = false;
    bool m_collapsed = false;
    bool m_dirty = false;
    bool m_isDarkTheme = false;
    LayoutMode m_layoutMode = LayoutMode::Docked;
    bool m_userOverrideMode = false;
    QPointer<QWidget> m_originalParent;
};

} // namespace DeepLux
