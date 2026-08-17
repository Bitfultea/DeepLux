#pragma once

#include "core/common/Logger.h"
#include "core/display/DisplayData.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"

#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVector>

class QDialog;
class QVBoxLayout;

namespace DeepLux {
class FlowCanvas;
class DisplayManager;
class ViewportWidget;
class TerminalWidget;
class AgentActionLogWidget;
class AgentChatPanel;
class ProcessTreeController;
class ModuleInspectorPanel;
class SamAnnotatorDialog;

class IModule;
struct PointCloudData;
struct PluginInfo;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // 测试辅助：直接注册运行时模块到 m_flowModules
    Q_INVOKABLE void registerFlowModule(const QString& instanceId, IModule* module);

    // 测试辅助：重置检查器关闭状态
    void resetInspectorClosed();

private slots:
    void onNewSolution();
    void onSolutionList();
    void onOpenProject();
    void onSaveProject();
    void onQuickMode();
    void onQuickMeasure();
    void onQuickAnnotate();
    void onRunOnce();
    void onRunCycle();
    void onStepRun();
    void onStop();
    void executeFlowOnce();
    void onUserLogin();
    void onGlobalVar();
    void onCameraSettings();
    void onCommSettings();
    void onHardwareConfig();
    void onHome();
    void onToggleTheme();
    void onDeviceSettings();
    void onSystemSettings();
    void onScreenshot();
    void onTest3DRender();
    void onAbout();
    void onLogAdded(const LogEntry& entry);
    void onLogFilterChanged(int index);
    void showLogLevelMenu();
    void onImportImage();
    void onToggleToolPanel(bool checked);
    void onToggleProcessPanel(bool checked);
    void onToggleBottomPanel(bool checked);
    void onProjectOpened(Project* project);
    void onProjectClosed();
    void onDataSourceAdded(const DataSource& ds);
    void onDataSourceRemoved(const QString& id);
    void onDataSourceVisibilityChanged(const QString& dataSourceId, bool visible);
    void onDisplayDataSource(const QString& dataSourceId);
    void onDisplayDataSourceInNewViewport(const QString& dataSourceId);
    void onRemoveDataSource(const QString& dataSourceId);
    void onShowDataSourceInFolder(const QString& dataSourceId);
    void onCopyDataSourcePath(const QString& dataSourceId);
    void onViewportCreated(const QString& viewportId, ViewportWidget* viewport);
    void onPoint2DPicked(const QPointF& point);
    void onPoint3DPicked(const QVector3D& point);
    void _phase8_openAdvancedPluginConfig(const QString& instanceId);
    void _phase8_toggleFocusMode();

private:
    void setupUi();
    void setupMenuBar();
    void setupToolBar();
    void setupStatusBar();
    void setupMainLayout();
    void updateProjectContext(Project* project);
    void addToolBoxItem(QTreeWidgetItem* parent, const QString& displayName, const QString& pluginName);
    QString toolDisplayName(const QString& pluginName, const QString& fallback = QString()) const;
    void updateToolBoxPluginItem(const QString& pluginName);

    // 创建工具栏图标
    QIcon createIcon(const QString& name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    // 左侧面板
    QDockWidget* m_toolBoxDock = nullptr;
    // 工具箱控件
    QTreeWidget* m_toolBoxTree = nullptr;
    QMap<QString, QString> m_toolDisplayNames;
    class QLineEdit* m_toolSearchEdit = nullptr;

    // 流程栏控件
    QTreeWidget* m_processTree = nullptr;
    QLabel* m_processTimeLabel = nullptr;
    QWidget* m_processStatusWidget = nullptr;
    bool m_isRunning = false;
    bool m_isCycleMode = false;
    bool m_isStepMode = false;
    bool m_closeWhenRunFinishes = false;
    QTimer* m_cycleTimer = nullptr;

    // 流程执行时间和高亮
    QMap<QString, int> m_moduleExecutionTimes;         // 模块实例名 -> 执行时间(ms)
    QTreeWidgetItem* m_currentExecutingItem = nullptr; // 当前正在执行的项目
    int m_currentExecutingIndex = 0;                   // 当前执行索引
    ImageData m_flowInput;                             // 流程执行时的输入数据
    int m_flowTotalTime = 0;                           // 总耗时
    bool m_modulesNeedSync = true;                     // 流程树变化后才能重新同步到 RunEngine

    // 底部面板
    QDockWidget* m_logDock = nullptr;
    QTableWidget* m_logTable = nullptr;
    QComboBox* m_logFilterCombo = nullptr;
    int m_logFilterLevel = 0; // 0=全部, 1=Debug, 2=Info, 3=Warning, 4=Error

    // 终端面板（Tab 方案）
    QTabWidget* m_logTerminalTabs = nullptr;
    TerminalWidget* m_terminalWidget = nullptr;
    AgentActionLogWidget* m_agentActionLogWidget = nullptr;
    AgentChatPanel* m_agentChatPanel = nullptr;
    int m_agentChatTabIndex = -1;

    // 顶部工程上下文与状态栏
    QLabel* m_userLabel = nullptr;
    QLabel* m_projectLabel = nullptr;
    QWidget* m_processTabContent = nullptr;
    QLabel* m_runStatusLabel = nullptr;
    QLabel* m_timeLabel = nullptr;
    // 最后导入的图像路径
    QString m_lastImportedImagePath;
    // 仅记录用户主动打开的额外视图，默认数据源都渲染在主视图。
    QMap<QString, QString> m_dataSourceViewportIds;
    QMap<QString, DisplayData> m_dataSourceDisplayData;
    QMap<QString, QImage> m_dataSourcePreviewImages;
    QSet<QString> m_hiddenDataSourceIds;
    QString m_primaryDataSourceViewportId;
    // 自动配置流程中第一个GrabImage模块的图像路径
    void autoConfigureGrabImage(const QString& filePath);

    // 主题
    bool m_isDarkTheme = false;
    void applyTheme();
    void setUiRunningState(bool running, bool cycleMode);
    bool syncModulesToRunEngine();
    void clearExecutionHighlight(QTreeWidgetItem* item);
    void showProcessModuleOutput(QTreeWidgetItem* item, bool userInitiated = false);
    void displayImage(const ImageData& image, const QString& label = QString());
    bool importFile(const QString& filePath);
    bool importImageFile(const QString& filePath, const QString& existingDataSourceId = QString());
    bool importPointCloudFile(const QString& filePath, const QString& existingDataSourceId = QString());
    void clearCentralDisplay();
    ViewportWidget* primaryDataSourceViewport();
    void rebuildPrimaryDataSourceDisplay();
    QString ensureDataSourceViewport(const QString& dataSourceId, const QString& title);
    void clearDataSourceViewports();

    // 阶段 7: 流程树 data-role 高亮（替代 setBackground）
    void setProcessItemStatus(QTreeWidgetItem* item, const QString& status, const QString& timeText = QString());
    // 阶段 7: 工具面板搜索
    void filterToolBox(const QString& text);
    // 阶段 7: 分类名称去除数字前缀
    static QString stripCategoryPrefix(const QString& text);
    // 阶段 8: 提取高级配置弹窗
    void openAdvancedPluginConfig(const QString& instanceId);
    // 阶段 8: 聚焦模式
    void toggleFocusMode();
    bool m_focusMode = false;
    bool m_focusSavedLogVisible = false;
    void addMeasurementConfigAction(QVBoxLayout* layout, const QString& consumerModuleId,
                                    const QString& consumerInstanceId, QDialog* dialog);
    QString ensureMeasurementInputForMode(const QString& mode, const QString& consumerInstanceId);
    IModule* measurementInputForPicking(QString& instanceId);
    bool requestMeasurementInputForRun();
    void finishMeasurementPick(const QString& instanceId, const QJsonObject& params, bool is3D);
    void refreshMeasurementOverlay(const QJsonObject& params, int visibleSteps);
    void refreshMeasurementOverlay3D(const QJsonObject& params, int visibleSteps);
    void updateMeasurementResultOnOverlay();
    void clearMeasurementOverlays();

    // 统一选择入口 — 同步流程树、画布和检查器
    void selectModule(const QString& instanceId, bool revealInspector, bool force = false);

    // 流程中的插件实例
    QMap<QString, IModule*> m_flowModules;
    QMap<QString, int> m_measurementPickCursor;
    QMap<QString, int> m_measurementPickCount;
    QString m_activeMeasurementInputId;
    QString m_pendingMeasurementInputId;

    // 快速测量（无需流程）
    bool m_quickMeasureActive = false;
    int m_quickMeasureType = 0;
    QList<QPointF> m_quickMeasurePoints;
    QList<QVector3D> m_quickMeasurePoints3D;
    QAction* m_quickMeasureAction = nullptr;

    // 流程画布（图形化节点编辑器）
    FlowCanvas* m_flowCanvas = nullptr;

    // 主 Splitter 成员（提升自局部变量）
    QSplitter* m_mainSplitter = nullptr;
    QSplitter* m_rightSplitter = nullptr;
    QSplitter* m_rightTopSplitter = nullptr;

    // 聚焦模式：保存/恢复 splitter 尺寸
    QList<int> m_focusSavedMainSizes;
    QList<int> m_focusSavedRightTopSizes;
    QList<int> m_focusSavedRightSplitterSizes;

    // 右侧检查器面板
    ModuleInspectorPanel* m_inspectorPanel = nullptr;

    // 当前唯一选中的模块实例 ID
    QString m_selectedModuleId;

    // 视图菜单动作
    QAction* m_viewToolPanelAction = nullptr;
    QAction* m_viewProcessPanelAction = nullptr;
    QAction* m_viewBottomPanelAction = nullptr;

    // Agent 菜单动作
    QAction* m_agentPermissionAction = nullptr;

    // 启动画面
    class SplashScreen* m_splashScreen = nullptr;
    QStringList m_failedPlugins;
    void showSplashScreen();
    void hideSplashScreen();
    void loadPluginsWithProgress();
    void loadAgentSettings();
    void updateAgentPermissionDisplay();

    class DataSourcePanel* m_dataSourcePanel = nullptr;
    QTabWidget* m_processTabWidget = nullptr;

    // 显示管理
    DisplayManager* m_displayManager = nullptr;
    QPointer<SamAnnotatorDialog> m_samAnnotatorDialog;

    // 3D 渲染模式（视图菜单内）
    QAction* m_renderSeparator = nullptr;
    QAction* m_renderActions[6] = {};
    void updateRenderModeCombo();
    void updateRenderModeComboForData(const PointCloudData& pc);
    void onRenderModeChanged(int index);

    // 流程树控制器
    ProcessTreeController* m_processTreeController = nullptr;

    // ===== 阶段 5: 参数编辑撤销栈 =====
    // 仅服务手动 UI 编辑的撤销栈（不合并 Agent 自己的撤销栈）
    QUndoStack* m_paramUndoStack = nullptr;
    // 参数修改后标记为脏的模块集合
    QSet<QString> m_dirtyModuleIds;

    // 参数菜单动作
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;

    // 设置参数并推入撤销栈
    void pushParamCommand(const QString& instanceId, const QString& key, const QVariant& value);
    void markModuleDirty(const QString& instanceId);

    // ===== 阶段 9: 自适应布局与状态保存 =====
    void saveSettings();
    void loadSettings();
    void adaptInspectorLayout();
    bool m_inspectorClosed = false;
    bool m_toolPanelUserClosed = false;
    bool m_toolPanelSuppressSignal = false; // 自适应隐藏时抑制 visibilityChanged 设 userClosed
};

} // namespace DeepLux
