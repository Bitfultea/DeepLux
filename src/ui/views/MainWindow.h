#pragma once

#include <QMainWindow>
#include <QDockWidget>
#include <QTreeWidget>
#include <QTableWidget>
#include <QLabel>
#include <QStatusBar>
#include <QToolBar>
#include <QSplitter>
#include <QLineEdit>
#include <QVector>
#include <QPushButton>
#include <QTimer>
#include <QElapsedTimer>
#include <QMap>
#include <QSet>
#include <QToolButton>
#include <QComboBox>
#include <QTabWidget>

#include "core/common/Logger.h"
#include "core/model/ImageData.h"

namespace DeepLux {
class PropertyPanel;
class FlowCanvas;
class DisplayManager;
class TerminalWidget;
class AgentActionLogWidget;
class AgentChatPanel;

class IModule;

class MainWindow : public QMainWindow
{
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
    void onReportQuery();
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
    void onHelp();
    void onAbout();
    void onSchemeManagement();
    void onLogAdded(const LogEntry& entry);
    void onLogFilterChanged(int index);
    void showLogLevelMenu();
    void onBarcodeEntered();
    void onImportImage();
    void onToggleToolPanel(bool checked);
    void onToggleProcessPanel(bool checked);
    void onProcessTreeContextMenu(const QPoint& pos);
private:
    void setupUi();
    void setupMenuBar();
    void setupToolBar();
    void setupStatusBar();
    void setupMainLayout();
    void addToolBoxItem(QTreeWidgetItem* parent, const QString& displayName, const QString& pluginName);

    // 创建工具栏图标
    QIcon createIcon(const QString& name);
    QIcon createNewIcon();
    QIcon createOpenIcon();
    QIcon createSaveIcon();
    QIcon createListIcon();
    QIcon createPlayIcon();
    QIcon createPauseIcon();
    QIcon createStopIcon();
    QIcon createCycleIcon();
    QIcon createUserIcon();
    QIcon createVariableIcon();
    QIcon createCameraIcon();
    QIcon createCommIcon();
    QIcon createHardwareIcon();
    QIcon createReportIcon();
    QIcon createHomeIcon();
    QIcon createUiDesignIcon();
    QIcon createLaserIcon();
    QIcon createQuickModeIcon();
    QIcon createToggleThemeIcon();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    // 左侧面板
    QDockWidget* m_toolBoxDock = nullptr;
    QDockWidget* m_processDock = nullptr;
    QDockWidget* m_propertyDock = nullptr;
    PropertyPanel* m_propertyPanel = nullptr;

    // 工具箱控件
    QTreeWidget* m_toolBoxTree = nullptr;
    QTreeWidgetItem* m_currentToolBoxItem = nullptr;  // 当前选中的工具箱项

    // 流程栏控件
    QTreeWidget* m_processTree = nullptr;
    QLabel* m_hintLabel = nullptr;  // 流程栏提示标签
    QLabel* m_processTimeLabel = nullptr;
    QWidget* m_processStatusWidget = nullptr;
    QToolButton* m_btnStartPause = nullptr;
    QToolButton* m_btnStop = nullptr;
    bool m_isRunning = false;
    bool m_isCycleMode = false;
    QTimer* m_cycleTimer = nullptr;

    // 流程执行时间和高亮
    QMap<QString, int> m_moduleExecutionTimes;  // 模块实例名 -> 执行时间(ms)
    QTreeWidgetItem* m_currentExecutingItem = nullptr;  // 当前正在执行的项目
    int m_currentExecutingIndex = 0;  // 当前执行索引
    ImageData m_flowInput;  // 流程执行时的输入数据
    int m_flowTotalTime = 0;  // 总耗时
    bool m_modulesNeedSync = true;  // 流程树变化后才能重新同步到 RunEngine
    QMap<QString, QTreeWidgetItem*> m_instanceItemMap;  // instanceName → 流程树 item

    // 底部面板
    QDockWidget* m_logDock = nullptr;
    QTableWidget* m_logTable = nullptr;
    QComboBox* m_logFilterCombo = nullptr;
    int m_logFilterLevel = 0;  // 0=全部, 1=Debug, 2=Info, 3=Warning, 4=Error

    // 终端面板（Tab 方案）
    QTabWidget* m_logTerminalTabs = nullptr;
    TerminalWidget* m_terminalWidget = nullptr;
    AgentActionLogWidget* m_agentActionLogWidget = nullptr;
    AgentChatPanel* m_agentChatPanel = nullptr;

    // 状态栏
    QLabel* m_userLabel = nullptr;
    QLabel* m_projectLabel = nullptr;
    QLabel* m_timeLabel = nullptr;
    QLineEdit* m_barcodeInput = nullptr;

    // 最后导入的图像路径
    QString m_lastImportedImagePath;
    // 自动配置流程中第一个GrabImage模块的图像路径
    void autoConfigureGrabImage(const QString& filePath);

    // 主题
    bool m_isDarkTheme = false;
    void applyTheme();
    void syncModulesToRunEngine();
    void displayImage(const ImageData& image, const QString& label = QString());
    bool importImageFile(const QString& filePath);
    void clearCentralDisplay();

    // 流程中的插件实例
    QMap<QString, IModule*> m_flowModules;
    QSet<QString> m_usedPluginNames;

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

    // 显示管理
    DisplayManager* m_displayManager = nullptr;
};

} // namespace DeepLux
