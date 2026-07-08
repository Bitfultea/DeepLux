#pragma once

#include "core/common/Logger.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"

#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMap>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
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

class IModule;
struct PointCloudData;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onNewSolution();
    void onSolutionList();
    void onOpenProject();
    void onSaveProject();
    void onQuickMode();
    void onRunOnce();
    void onRunCycle();
    void onStop();
    void executeFlowOnce();
    void onUserLogin();
    void onGlobalVar();
    void onCameraSettings();
    void onCommSettings();
    void onHardwareConfig();
    void onHome();
    void onUIDesign();
    void onLaserSet();
    void onToggleTheme();
    void onDeviceSettings();
    void onSystemSettings();
    void onCanvasSettings();
    void onScreenshot();
    void onSaveLayout();
    void onLoadLayout();
    void onLicenseManager();
    void onTest3DRender();
    void onHelp();
    void onAbout();
    void onSchemeManagement();
    void onLogAdded(const LogEntry& entry);
    void onLogFilterChanged(int index);
    void showLogLevelMenu();
    void onImportImage();
    void onToggleToolPanel(bool checked);
    void onToggleProcessPanel(bool checked);
    void onProjectOpened(Project* project);
    void onProjectClosed();
    void onDataSourceAdded(const DataSource& ds);
    void onDataSourceRemoved(const QString& id);
    void onDisplayDataSource(const QString& dataSourceId);
    void onRemoveDataSource(const QString& dataSourceId);
    void onShowDataSourceInFolder(const QString& dataSourceId);
    void onCopyDataSourcePath(const QString& dataSourceId);
    void onViewportCreated(const QString& viewportId, ViewportWidget* viewport);
    void onPoint2DPicked(const QPointF& point);
    void onPoint3DPicked(const QVector3D& point);

private:
    void setupUi();
    void setupMenuBar();
    void setupToolBar();
    void setupStatusBar();
    void setupMainLayout();
    void addToolBoxItem(QTreeWidgetItem* parent, const QString& displayName, const QString& pluginName);
    QString toolDisplayName(const QString& pluginName, const QString& fallback = QString()) const;
    void updateToolBoxPluginItem(const QString& pluginName);

    // 创建工具栏图标
    QIcon createIcon(const QString& name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    // 左侧面板
    QDockWidget* m_toolBoxDock = nullptr;
    // 工具箱控件
    QTreeWidget* m_toolBoxTree = nullptr;
    QTreeWidgetItem* m_currentToolBoxItem = nullptr; // 当前选中的工具箱项
    QMap<QString, QString> m_toolDisplayNames;

    // 流程栏控件
    QTreeWidget* m_processTree = nullptr;
    QLabel* m_processTimeLabel = nullptr;
    QWidget* m_processStatusWidget = nullptr;
    QToolButton* m_btnStartPause = nullptr;
    QToolButton* m_btnStop = nullptr;
    bool m_isRunning = false;
    bool m_isCycleMode = false;
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

    // 状态栏
    QLabel* m_userLabel = nullptr;
    QLabel* m_projectLabel = nullptr;
    QWidget* m_processTabContent = nullptr;
    QLabel* m_timeLabel = nullptr;
    // 最后导入的图像路径
    QString m_lastImportedImagePath;
    // 自动配置流程中第一个GrabImage模块的图像路径
    void autoConfigureGrabImage(const QString& filePath);

    // 主题
    bool m_isDarkTheme = false;
    void applyTheme();
    void setUiRunningState(bool running, bool cycleMode);
    void syncModulesToRunEngine();
    void displayImage(const ImageData& image, const QString& label = QString());
    bool importFile(const QString& filePath);
    bool importImageFile(const QString& filePath);
    bool importPointCloudFile(const QString& filePath);
    void clearCentralDisplay();
    void addMeasurementConfigAction(QVBoxLayout* layout, const QString& consumerModuleId,
                                    const QString& consumerInstanceId, QDialog* dialog);
    QString ensureMeasurementInputForMode(const QString& mode, const QString& consumerInstanceId);
    void refreshMeasurementOverlay(const QJsonObject& params, int visibleSteps);
    void refreshMeasurementOverlay3D(const QJsonObject& params, int visibleSteps);
    void clearMeasurementOverlays();

    // 流程中的插件实例
    QMap<QString, IModule*> m_flowModules;
    QMap<QString, int> m_measurementPickCursor;
    QMap<QString, int> m_measurementPickCount;

    // 流程画布（图形化节点编辑器）
    FlowCanvas* m_flowCanvas = nullptr;

    // 视图菜单动作
    QAction* m_viewToolPanelAction = nullptr;
    QAction* m_viewProcessPanelAction = nullptr;

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

    // 3D 渲染模式（视图菜单内）
    QAction* m_renderActions[6] = {};
    void updateRenderModeCombo();
    void updateRenderModeComboForData(const PointCloudData& pc);
    void onRenderModeChanged(int index);

    // 流程树控制器
    ProcessTreeController* m_processTreeController = nullptr;
};

} // namespace DeepLux
