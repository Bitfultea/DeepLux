#include "MainWindow.h"

#include "../dialogs/LoginDialog.h"
#include "../display/DisplayManager.h"
#include "../widgets/FlowCanvas.h"
#include "../widgets/PropertyPanel.h"
#include "../widgets/TerminalWidget.h"
#include "../widgets/AgentActionLogWidget.h"
#include "../widgets/AgentChatPanel.h"
#include "../dialogs/AgentSettingsDialog.h"
#include "../bridge/TerminalBridge.h"
#include "core/agent/AgentController.h"
#include "core/agent/OpenAILLMClient.h"
#include "CameraSetView.h"
#include "CommunicationSetView.h"
#include "GlobalVarView.h"
#include "SplashScreen.h"
#include "SystemParamView.h"
#include "core/common/ConfigWidgetHelper.h"
#include "core/common/Logger.h"
#include "core/manager/ConfigManager.h"
#include "core/display/DisplayData.h"
#include "core/display/IDisplayPort.h"
#include "core/interface/IModule.h"
#include "core/base/ModuleBase.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/engine/RunEngine.h"
#include "core/model/Project.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

namespace DeepLux {

// 前向声明
class PropertyPanel;
class FlowCanvas;

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_displayManager(new DisplayManager(this)) {

    setupUi();
    applyTheme();

    // RunEngine 信号连接 — 统一执行入口，MainWindow 只做 UI 高亮/状态更新
    // 使用 instanceName → item 映射实现 O(1) 查找
    connect(&RunEngine::instance(), &RunEngine::moduleStarted, this, [this](const QString& moduleName) {
        QTreeWidgetItem* item = m_instanceItemMap.value(moduleName);
        if (item) {
            m_currentExecutingItem = item;
            item->setBackground(0, QBrush(QColor("#0078d7")));
            item->setForeground(0, QBrush(Qt::white));
            item->setBackground(1, QBrush(QColor("#0078d7")));
            item->setForeground(1, QBrush(Qt::white));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            item->setText(1, tr("执行中..."));
        }
    });

    connect(&RunEngine::instance(), &RunEngine::moduleFinished, this, [this](const QString& moduleName, bool success) {
        Q_UNUSED(moduleName)
        if (m_currentExecutingItem) {
            m_currentExecutingItem->setBackground(0, QBrush());
            m_currentExecutingItem->setForeground(0, QBrush());
            m_currentExecutingItem->setBackground(1, QBrush());
            m_currentExecutingItem->setForeground(1, QBrush());
            m_currentExecutingItem->setText(1, success ? tr("OK") : tr("FAIL"));
            if (!success) {
                m_currentExecutingItem->setForeground(1, QBrush(Qt::red));
            }
        }
        // Display output if available
        const ImageData& out = RunEngine::instance().lastOutput();
        if (out.isValid()) {
            displayImage(out);
        }
    });

    connect(&RunEngine::instance(), &RunEngine::runFinished, this, [this](const RunResult& result) {
        if (m_processTimeLabel) {
            m_processTimeLabel->setText(tr("总耗时：%1 ms").arg(result.elapsedMs));
        }
        m_currentExecutingItem = nullptr;
        if (m_isCycleMode && m_isRunning) {
            QTimer::singleShot(1, this, &MainWindow::executeFlowOnce);
        }
    });

    // Initialize AgentController (Phase 1)
    AgentController::instance().initialize();

    // Load Agent settings from ConfigManager (Phase 3)
    loadAgentSettings();

    // Connect Agent action log to UI (will be set after m_agentActionLogWidget is created)
}

MainWindow::~MainWindow() {
    if (m_splashScreen) {
        m_splashScreen->deleteLater();
    }
}

void MainWindow::showSplashScreen() {
    if (!m_splashScreen) {
        m_splashScreen = new SplashScreen();
    }
    m_splashScreen->show();
    m_splashScreen->setProgress(0, tr("正在初始化..."));
    QApplication::processEvents();
}

void MainWindow::hideSplashScreen() {
    if (m_splashScreen) {
        m_splashScreen->setProgress(100, tr("加载完成"));
        QApplication::processEvents();
        QTimer::singleShot(300, this, [this]() {
            if (m_splashScreen) {
                m_splashScreen->close();
                m_splashScreen->deleteLater();
                m_splashScreen = nullptr;
            }
            // splash 关闭后显示主窗口
            show();
        });
    }
}

void MainWindow::loadPluginsWithProgress() {
    showSplashScreen();
    m_splashScreen->setProgress(5, tr("初始化插件管理器..."));
    m_splashScreen->appendLog(tr("正在扫描插件..."));
    QApplication::processEvents();

    PluginManager::instance().initialize();

    m_failedPlugins.clear();

    int totalPlugins =
        PluginManager::instance().availableModules().size() + PluginManager::instance().availableCameras().size();

    connect(
        &PluginManager::instance(), &PluginManager::pluginLoaded, this,
        [this](const QString& name) {
            m_splashScreen->appendLog(QString("<span style='color: #3498db;'>✓</span> %1").arg(name));
        },
        Qt::DirectConnection);

    connect(
        &PluginManager::instance(), &PluginManager::pluginLoadFailed, this,
        [this](const QString& name, const QString& error) {
            Q_UNUSED(error);
            m_failedPlugins.append(name);
            m_splashScreen->appendLog(QString("<span style='color: #e94560;'>✗</span> %1").arg(name));
        },
        Qt::DirectConnection);

    connect(
        &PluginManager::instance(), &PluginManager::pluginLoadProgress, this,
        [this, totalPlugins](int current, int total, const QString& name) {
            int progress = 30 + (current * 65) / total;
            m_splashScreen->setProgress(progress, tr("加载: %1 (%2/%3)").arg(name).arg(current).arg(total));
        },
        Qt::DirectConnection);

    connect(
        &PluginManager::instance(), &PluginManager::allPluginsLoaded, this,
        [this]() {
            if (m_failedPlugins.isEmpty()) {
                m_splashScreen->appendLog("");
                m_splashScreen->appendLog(tr("<span style='color: #2ecc71;'>全部插件加载完成</span>"));
                m_splashScreen->setProgress(100, tr("加载完成"));
                QTimer::singleShot(500, this, [this]() { hideSplashScreen(); });
            } else {
                m_splashScreen->setProgress(100, tr("加载完成，有 %1 个插件失败").arg(m_failedPlugins.size()));
                m_splashScreen->showFailedPlugins(m_failedPlugins);
                QTimer::singleShot(3000, this, [this]() { hideSplashScreen(); });
            }
        },
        Qt::DirectConnection);

    m_splashScreen->appendLog(tr("开始加载 %1 个插件...").arg(totalPlugins));
    m_splashScreen->setProgress(30, tr("正在加载插件..."));
    QApplication::processEvents();
    PluginManager::instance().loadAllPluginsAsync();
}

void MainWindow::setupUi() {
    setWindowTitle(tr("DeepLux Vision 1.0.0"));
    resize(1280, 800);

    setupMenuBar();
    setupToolBar();
    setupStatusBar();
    setupMainLayout();

    // 显示启动画面并加载插件
    loadPluginsWithProgress();
}

void MainWindow::setupMenuBar() {
    // 文件菜单
    QMenu* fileMenu = menuBar()->addMenu(tr("文件 (&F)"));
    fileMenu->addAction(createNewIcon(), tr("新建方案"), this, &MainWindow::onNewSolution);
    fileMenu->addAction(createListIcon(), tr("方案列表"), this, &MainWindow::onSolutionList);
    fileMenu->addAction(createOpenIcon(), tr("打开"), this, &MainWindow::onOpenProject);
    fileMenu->addAction(createSaveIcon(), tr("保存"), this, &MainWindow::onSaveProject);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("退出"), qApp, &QApplication::quit);

    // 参数菜单
    QMenu* paramMenu = menuBar()->addMenu(tr("参数 (&P)"));
    paramMenu->addAction(createVariableIcon(), tr("全局变量"), this, &MainWindow::onGlobalVar);
    paramMenu->addAction(createUserIcon(), tr("用户登录"), this, &MainWindow::onUserLogin);

    // 视图菜单
    QMenu* viewMenu = menuBar()->addMenu(tr("视图 (&V)"));
    m_viewToolPanelAction = new QAction(tr("工具"), this);
    m_viewToolPanelAction->setCheckable(true);
    m_viewToolPanelAction->setChecked(true);
    connect(m_viewToolPanelAction, &QAction::toggled, this, &MainWindow::onToggleToolPanel);
    viewMenu->addAction(m_viewToolPanelAction);

    m_viewProcessPanelAction = new QAction(tr("流程"), this);
    m_viewProcessPanelAction->setCheckable(true);
    m_viewProcessPanelAction->setChecked(true);
    connect(m_viewProcessPanelAction, &QAction::toggled, this, &MainWindow::onToggleProcessPanel);
    viewMenu->addAction(m_viewProcessPanelAction);

    viewMenu->addSeparator();
    viewMenu->addAction(createQuickModeIcon(), tr("快捷模式"), this, &MainWindow::onQuickMode);
    viewMenu->addSeparator();
    viewMenu->addAction(createToggleThemeIcon(), tr("切换主题"), this, &MainWindow::onToggleTheme);

    // 工具菜单
    QMenu* toolMenu = menuBar()->addMenu(tr("工具 (&T)"));
    toolMenu->addAction(createCameraIcon(), tr("相机设置"), this, &MainWindow::onCameraSettings);
    toolMenu->addAction(createCommIcon(), tr("通讯设置"), this, &MainWindow::onCommSettings);
    toolMenu->addAction(createHardwareIcon(), tr("硬件配置"), this, &MainWindow::onHardwareConfig);
    toolMenu->addAction(createReportIcon(), tr("报表查询"), this, &MainWindow::onReportQuery);
    toolMenu->addSeparator();
    toolMenu->addAction(tr("Agent 设置"), this, [this]() {
        AgentSettingsDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            AgentController& ctrl = AgentController::instance();
            ctrl.setPermissionLevel(dlg.permissionLevel());
            OpenAILLMClient* client = qobject_cast<OpenAILLMClient*>(ctrl.llmClient());
            if (!client) {
                client = new OpenAILLMClient(&ctrl);
                ctrl.setLLMClient(client);
            }
            client->setEndpoint(dlg.endpoint());
            client->setApiKey(dlg.apiKey());
            client->setModel(dlg.model());
            client->setTemperature(dlg.temperature());
            client->setMaxTokens(dlg.maxTokens());
            client->setToolsEnabled(dlg.toolsEnabled());
        }
    });

    // 帮助菜单
    QMenu* helpMenu = menuBar()->addMenu(tr("帮助 (&H)"));
    helpMenu->addAction(tr("关于"), this, &MainWindow::onAbout);
}

void MainWindow::setupToolBar() {
    QToolBar* mainToolbar = addToolBar(tr("主工具栏"));
    mainToolbar->setMovable(false);
    mainToolbar->setIconSize(QSize(24, 24));
    mainToolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

    // 文件操作
    mainToolbar->addAction(createNewIcon(), tr("新建方案"), this, &MainWindow::onNewSolution);
    mainToolbar->addAction(createListIcon(), tr("方案列表"), this, &MainWindow::onSolutionList);
    mainToolbar->addAction(createOpenIcon(), tr("打开"), this, &MainWindow::onOpenProject);
    mainToolbar->addAction(createSaveIcon(), tr("保存"), this, &MainWindow::onSaveProject);
    mainToolbar->addSeparator();

    // 运行模式
    mainToolbar->addAction(createQuickModeIcon(), tr("快捷模式"), this, &MainWindow::onQuickMode);
    mainToolbar->addSeparator();

    // 运行控制
    mainToolbar->addAction(createPlayIcon(), tr("单次运行"), this, &MainWindow::onRunOnce);
    mainToolbar->addAction(createCycleIcon(), tr("循环运行"), this, &MainWindow::onRunCycle);
    mainToolbar->addAction(createStopIcon(), tr("停止"), this, &MainWindow::onStop);
    mainToolbar->addSeparator();

    // 其他功能
    mainToolbar->addAction(createUserIcon(), tr("用户登录"), this, &MainWindow::onUserLogin);
    mainToolbar->addAction(createVariableIcon(), tr("全局变量"), this, &MainWindow::onGlobalVar);
    mainToolbar->addAction(createCameraIcon(), tr("相机设置"), this, &MainWindow::onCameraSettings);
    mainToolbar->addAction(createCommIcon(), tr("通讯设置"), this, &MainWindow::onCommSettings);
    mainToolbar->addAction(createHardwareIcon(), tr("硬件配置"), this, &MainWindow::onHardwareConfig);
    mainToolbar->addAction(createReportIcon(), tr("报表查询"), this, &MainWindow::onReportQuery);
    mainToolbar->addSeparator();

    // 主题切换按钮
    mainToolbar->addAction(createToggleThemeIcon(), tr("切换主题"), this, &MainWindow::onToggleTheme);
    mainToolbar->addSeparator();

    // 右侧功能
    mainToolbar->addAction(createHomeIcon(), tr("主页"), this, &MainWindow::onHome);
    mainToolbar->addAction(createUiDesignIcon(), tr("UI 设计"), this, &MainWindow::onUIDesign);
    mainToolbar->addAction(createLaserIcon(), tr("激光设置"), this, &MainWindow::onLaserSet);
    mainToolbar->addSeparator();

    // 扫码框只保留在状态栏，工具栏不再重复添加
}

void MainWindow::setupStatusBar() {
    QStatusBar* status = statusBar();
    status->setSizeGripEnabled(false); // 禁用右下角 resize grip，避免黑块

    m_userLabel = new QLabel(tr("用户：未登录"));
    m_userLabel->setMinimumWidth(150);
    status->addPermanentWidget(m_userLabel);

    m_projectLabel = new QLabel(tr("当前工程：无"));
    m_projectLabel->setMinimumWidth(200);
    status->addPermanentWidget(m_projectLabel);

    m_timeLabel = new QLabel();
    m_timeLabel->setMinimumWidth(150);
    status->addPermanentWidget(m_timeLabel);

    // 更新时间的定时器
    QTimer* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this,
            [this]() { m_timeLabel->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")); });
    timer->start(1000);

    m_barcodeInput = new QLineEdit();
    m_barcodeInput->setPlaceholderText(tr("扫码枪输入"));
    m_barcodeInput->setMaximumWidth(200);
    status->addPermanentWidget(m_barcodeInput);
    connect(m_barcodeInput, &QLineEdit::returnPressed, this, &MainWindow::onBarcodeEntered);
}

void MainWindow::setupMainLayout() {
    // 创建主 Splitter - 水平分割左右
    QSplitter* mainSplitter = new QSplitter(Qt::Horizontal);
    mainSplitter->setObjectName("MainSplitter");
    mainSplitter->setHandleWidth(1);

    // ========== 左侧：工具面板（贯穿整个高度）==========
    QWidget* toolPanelWidget = new QWidget();
    toolPanelWidget->setObjectName("ToolPanelWidget");
    QVBoxLayout* toolPanelLayout = new QVBoxLayout(toolPanelWidget);
    toolPanelLayout->setContentsMargins(0, 0, 0, 0);
    toolPanelLayout->setSpacing(0);

    // 工具面板顶部工具栏
    QWidget* toolToolBar = new QWidget();
    QHBoxLayout* toolToolBarLayout = new QHBoxLayout(toolToolBar);
    toolToolBarLayout->setContentsMargins(5, 16, 5, 5);
    toolToolBarLayout->setSpacing(2);
    toolToolBar->setObjectName("ToolToolBar");
    toolToolBar->setMinimumHeight(50);

    QToolButton* createProcessBtn = new QToolButton();
    createProcessBtn->setToolTip(tr("新建流程"));
    createProcessBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    createProcessBtn->setMinimumHeight(36);
    createProcessBtn->setMaximumHeight(36);
    createProcessBtn->setIcon(createNewIcon());
    createProcessBtn->setIconSize(QSize(24, 24));
    createProcessBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    createProcessBtn->setObjectName("ToolCreateProcessBtn");

    QToolButton* deleteProcessBtn = new QToolButton();
    deleteProcessBtn->setToolTip(tr("删除流程"));
    deleteProcessBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    deleteProcessBtn->setMinimumHeight(36);
    deleteProcessBtn->setMaximumHeight(36);
    deleteProcessBtn->setIcon(createStopIcon());
    deleteProcessBtn->setIconSize(QSize(24, 24));
    deleteProcessBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    deleteProcessBtn->setObjectName("ToolDeleteProcessBtn");

    QToolButton* createMethodBtn = new QToolButton();
    createMethodBtn->setToolTip(tr("新建方法"));
    createMethodBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    createMethodBtn->setMinimumHeight(36);
    createMethodBtn->setMaximumHeight(36);
    createMethodBtn->setIcon(createListIcon());
    createMethodBtn->setIconSize(QSize(24, 24));
    createMethodBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    createMethodBtn->setObjectName("ToolCreateMethodBtn");

    QToolButton* toolSettingsBtn = new QToolButton();
    toolSettingsBtn->setToolTip(tr("设置"));
    toolSettingsBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolSettingsBtn->setMinimumHeight(36);
    toolSettingsBtn->setMaximumHeight(36);
    toolSettingsBtn->setIcon(createHardwareIcon());
    toolSettingsBtn->setIconSize(QSize(24, 24));
    toolSettingsBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toolSettingsBtn->setObjectName("ToolSettingsBtn");

    toolToolBarLayout->addWidget(createProcessBtn);
    toolToolBarLayout->addWidget(deleteProcessBtn);
    toolToolBarLayout->addWidget(createMethodBtn);
    toolToolBarLayout->addWidget(toolSettingsBtn);
    toolPanelLayout->addWidget(toolToolBar);
    toolPanelLayout->setStretchFactor(toolToolBar, 0);

    // 流程树（上方区域）
    QTreeWidget* processTree = new QTreeWidget();
    processTree->setHeaderHidden(true);
    processTree->setObjectName("ToolProcessTree");
    QTreeWidgetItem* processRoot = new QTreeWidgetItem(processTree, QStringList(tr("流程 1")));
    processRoot->setExpanded(true);
    processTree->addTopLevelItem(processRoot);
    processTree->setMaximumHeight(250);
    toolPanelLayout->addWidget(processTree);
    toolPanelLayout->setStretchFactor(processTree, 0);

    // 分割线
    QFrame* toolPanelSeparator = new QFrame();
    toolPanelSeparator->setFrameShape(QFrame::HLine);
    toolPanelSeparator->setMaximumHeight(4);
    toolPanelSeparator->setObjectName("ToolPanelSeparator");
    toolPanelLayout->addWidget(toolPanelSeparator);

    // 工具分类区域（带滚动）
    QScrollArea* toolCategoryScroll = new QScrollArea();
    toolCategoryScroll->setWidgetResizable(true);
    toolCategoryScroll->setObjectName("ToolCategoryScroll");

    QWidget* toolCategoryWidget = new QWidget();
    QVBoxLayout* toolCategoryLayout = new QVBoxLayout(toolCategoryWidget);
    toolCategoryLayout->setContentsMargins(0, 0, 0, 0);
    toolCategoryLayout->setSpacing(0);
    toolCategoryLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    toolCategoryWidget->setObjectName("ToolCategoryWidget");

    m_toolBoxTree = new QTreeWidget();
    m_toolBoxTree->setHeaderHidden(true);
    m_toolBoxTree->setDragEnabled(true);
    m_toolBoxTree->setDragDropMode(QAbstractItemView::DragOnly);
    m_toolBoxTree->setAnimated(true);
    m_toolBoxTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_toolBoxTree->setMouseTracking(true);
    m_toolBoxTree->setAcceptDrops(false); // 禁用工具箱内部拖放，防止生成嵌套菜单
    m_toolBoxTree->setObjectName("ToolBoxTree");
    m_toolBoxTree->installEventFilter(this);
    connect(m_toolBoxTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item && item->data(0, Qt::UserRole).toString() == "plugin") {
            m_currentToolBoxItem = item;
        }
    });

    // 创建工具分类的 lambda，禁用分类标题的拖拽
    auto createCategoryItem = [&](QTreeWidget* tree, const QString& text) -> QTreeWidgetItem* {
        QTreeWidgetItem* item = new QTreeWidgetItem(tree, QStringList(text));
        item->setData(0, Qt::UserRole, "category");
        // 禁用拖拽
        item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
        return item;
    };

    // 工具分类（使用 createCategoryItem 禁用拖拽）
    QTreeWidgetItem* commonItem = createCategoryItem(m_toolBoxTree, tr("00 - 常用工具"));
    commonItem->setExpanded(true);

    QTreeWidgetItem* imgProcItem = createCategoryItem(m_toolBoxTree, tr("01 - 图像处理"));
    imgProcItem->setExpanded(false);
    addToolBoxItem(imgProcItem, tr("📷 图像采集"), "GrabImage");
    addToolBoxItem(imgProcItem, tr("💾 保存图像"), "SaveImage");
    addToolBoxItem(imgProcItem, tr("🖼️ 显示图像"), "ShowImage");
    addToolBoxItem(imgProcItem, tr("🔄 图像预处理"), "PerProcessing");
    addToolBoxItem(imgProcItem, tr("🎨 颜色识别"), "ColorRecognition");
    addToolBoxItem(imgProcItem, tr("🔍 斑点分析"), "Blob");

    QTreeWidgetItem* detectItem = createCategoryItem(m_toolBoxTree, tr("02 - 检测识别"));
    detectItem->setExpanded(false);
    addToolBoxItem(detectItem, tr("🔍 模板匹配"), "Matching");
    addToolBoxItem(detectItem, tr("🔍 二维码识别"), "QRCode");

    QTreeWidgetItem* geometryItem = createCategoryItem(m_toolBoxTree, tr("03 - 几何测量"));
    geometryItem->setExpanded(false);
    geometryItem->setData(0, Qt::UserRole, "category");
    addToolBoxItem(geometryItem, tr("📏 距离测量 (点到点)"), "DistancePP");
    addToolBoxItem(geometryItem, tr("📏 距离测量 (点到线)"), "DistancePL");
    addToolBoxItem(geometryItem, tr("📏 线段距离"), "LinesDistance");
    addToolBoxItem(geometryItem, tr("📐 测量矩形"), "MeasureRect");
    addToolBoxItem(geometryItem, tr("📐 测量直线"), "MeasureLine");
    addToolBoxItem(geometryItem, tr("📐 测量间隙"), "MeasureGap");

    QTreeWidgetItem* geoRelationItem = createCategoryItem(m_toolBoxTree, tr("04 - 几何关系"));
    geoRelationItem->setExpanded(false);
    addToolBoxItem(geoRelationItem, tr("🔵 找圆"), "FindCircle");
    addToolBoxItem(geoRelationItem, tr("⭕ 圆拟合"), "FitCircle");
    addToolBoxItem(geoRelationItem, tr("📐 直线拟合"), "FitLine");

    QTreeWidgetItem* calibItem = createCategoryItem(m_toolBoxTree, tr("05 - 坐标标定"));
    calibItem->setExpanded(false);
    addToolBoxItem(calibItem, tr("📐 N 点标定"), "NPointCalibration");

    QTreeWidgetItem* alignItem = createCategoryItem(m_toolBoxTree, tr("06 - 对位工具"));
    alignItem->setExpanded(false);

    QTreeWidgetItem* logicItem = createCategoryItem(m_toolBoxTree, tr("07 - 逻辑工具"));
    logicItem->setExpanded(false);
    addToolBoxItem(logicItem, tr("▶️ 如果"), "If");
    addToolBoxItem(logicItem, tr("🔁 循环"), "Loop");
    addToolBoxItem(logicItem, tr("🔁 While 循环"), "While");
    addToolBoxItem(logicItem, tr("⏹ 停止循环"), "StopWhile");
    addToolBoxItem(logicItem, tr("🔀 条件判断"), "Condition");

    QTreeWidgetItem* systemItem = createCategoryItem(m_toolBoxTree, tr("08 - 系统工具"));
    systemItem->setExpanded(false);
    addToolBoxItem(systemItem, tr("🕐 系统时间"), "SystemTime");
    addToolBoxItem(systemItem, tr("📁 文件夹操作"), "Folder");

    QTreeWidgetItem* varItem = createCategoryItem(m_toolBoxTree, tr("09 - 变量工具"));
    varItem->setExpanded(false);
    addToolBoxItem(varItem, tr("🔢 变量定义"), "VarDefine");
    addToolBoxItem(varItem, tr("🔢 变量设置"), "VarSet");
    addToolBoxItem(varItem, tr("🔢 数学运算"), "Math");
    addToolBoxItem(varItem, tr("📊 数据检查"), "DataCheck");
    addToolBoxItem(varItem, tr("🔢 显示数据"), "DisplayData");

    QTreeWidgetItem* fileCommItem = createCategoryItem(m_toolBoxTree, tr("10 - 文件通讯"));
    fileCommItem->setExpanded(false);
    addToolBoxItem(fileCommItem, tr("💾 保存数据"), "SaveData");
    addToolBoxItem(fileCommItem, tr("📊 表格输出"), "TableOutPut");
    addToolBoxItem(fileCommItem, tr("📝 写入文本"), "WriteText");

    QTreeWidgetItem* tool3DItem = createCategoryItem(m_toolBoxTree, tr("11 - 3D 工具"));
    tool3DItem->setExpanded(false);

    QTreeWidgetItem* dlItem = createCategoryItem(m_toolBoxTree, tr("12 - 深度学习"));
    dlItem->setExpanded(false);

    QTreeWidgetItem* strItem = createCategoryItem(m_toolBoxTree, tr("13 - 字符串处理"));
    strItem->setExpanded(false);
    addToolBoxItem(strItem, tr("✂️ 分割字符串"), "SplitString");
    addToolBoxItem(strItem, tr("📝 字符串格式化"), "StrFormat");
    addToolBoxItem(strItem, tr("➕ 创建字符串"), "CreateString");

    QTreeWidgetItem* commItem = createCategoryItem(m_toolBoxTree, tr("14 - 通信"));
    commItem->setExpanded(false);
    addToolBoxItem(commItem, tr("🔌 PLC 通信"), "PLCCommunicate");
    addToolBoxItem(commItem, tr("📡 PLC 读取"), "PLCRead");
    addToolBoxItem(commItem, tr("📡 PLC 写入"), "PLCWrite");
    addToolBoxItem(commItem, tr("🌐 TCP 客户端"), "TCPClient");
    addToolBoxItem(commItem, tr("🌐 TCP 服务器"), "TCPServer");
    addToolBoxItem(commItem, tr("🔌 串口通信"), "SerialPort");

    toolCategoryLayout->addWidget(m_toolBoxTree);
    toolCategoryScroll->setWidget(toolCategoryWidget);
    toolPanelLayout->addWidget(toolCategoryScroll);
    toolPanelLayout->setStretchFactor(toolCategoryScroll, 10);

    // 创建工具 DockWidget
    m_toolBoxDock = new QDockWidget(this);
    m_toolBoxDock->setObjectName("ToolPanelDock");
    m_toolBoxDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    m_toolBoxDock->setWidget(toolPanelWidget);
    m_toolBoxDock->setMinimumWidth(320);
    m_toolBoxDock->setMaximumWidth(500);
    // 自定义标题栏
    QWidget* toolTitleWidget = new QWidget();
    QHBoxLayout* toolTitleLayout = new QHBoxLayout(toolTitleWidget);
    toolTitleLayout->setContentsMargins(10, 0, 5, 0);
    toolTitleLayout->setSpacing(5);
    QLabel* toolTitleLabel = new QLabel(tr("工具"));
    toolTitleLabel->setObjectName("ToolTitleLabel");
    toolTitleLayout->addWidget(toolTitleLabel);
    toolTitleLayout->addStretch();
    QToolButton* toolCloseBtn = new QToolButton();
    toolCloseBtn->setText("×");
    toolCloseBtn->setToolTip(tr("关闭"));
    toolCloseBtn->setFixedSize(20, 20);
    toolCloseBtn->setObjectName("ToolCloseBtn");
    toolTitleLayout->addWidget(toolCloseBtn);
    toolTitleWidget->setMinimumHeight(36);
    toolTitleWidget->setObjectName("ToolTitleWidget");
    m_toolBoxDock->setTitleBarWidget(toolTitleWidget);
    connect(toolCloseBtn, &QToolButton::clicked, m_toolBoxDock, &QDockWidget::close);

    // 将工具面板添加到主 Splitter 左侧
    mainSplitter->addWidget(m_toolBoxDock);

    // ========== 右侧：垂直 Splitter（上方：流程 + 显示区域，下方：日志栏）==========
    QSplitter* rightSplitter = new QSplitter(Qt::Vertical);
    rightSplitter->setObjectName("RightSplitter");
    rightSplitter->setHandleWidth(1);

    // 右上方区域：水平 Splitter（流程面板 + 显示区域）
    QSplitter* rightTopSplitter = new QSplitter(Qt::Horizontal);
    rightTopSplitter->setObjectName("RightTopSplitter");
    rightTopSplitter->setHandleWidth(1);

    // 流程面板
    QWidget* processPanelWidget = new QWidget();
    processPanelWidget->setObjectName("ProcessPanelWidget");
    QVBoxLayout* processPanelLayout = new QVBoxLayout(processPanelWidget);
    processPanelLayout->setContentsMargins(0, 0, 0, 0);
    processPanelLayout->setSpacing(0);

    // 流程面板工具栏
    QWidget* processToolBar = new QWidget();
    QHBoxLayout* processToolBarLayout = new QHBoxLayout(processToolBar);
    processToolBarLayout->setContentsMargins(5, 16, 5, 5);
    processToolBarLayout->setSpacing(2);
    processToolBar->setObjectName("ProcessToolBar");
    processToolBar->setMinimumHeight(50);

    m_btnStartPause = new QToolButton();
    m_btnStartPause->setToolTip(tr("单次运行"));
    m_btnStartPause->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_btnStartPause->setMinimumHeight(36);
    m_btnStartPause->setMaximumHeight(36);
    m_btnStartPause->setAutoRaise(true);
    m_btnStartPause->setIcon(createPlayIcon());
    m_btnStartPause->setIconSize(QSize(24, 24));
    m_btnStartPause->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnStartPause->setObjectName("ProcessStartPauseBtn");

    m_btnStop = new QToolButton();
    m_btnStop->setToolTip(tr("停止"));
    m_btnStop->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_btnStop->setMinimumHeight(36);
    m_btnStop->setMaximumHeight(36);
    m_btnStop->setAutoRaise(true);
    m_btnStop->setIcon(createStopIcon());
    m_btnStop->setIconSize(QSize(24, 24));
    m_btnStop->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnStop->setObjectName("ProcessStopBtn");
    m_btnStop->setEnabled(false);

    QToolButton* runCycleBtn = new QToolButton();
    runCycleBtn->setToolTip(tr("循环运行"));
    runCycleBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    runCycleBtn->setMinimumHeight(36);
    runCycleBtn->setMaximumHeight(36);
    runCycleBtn->setAutoRaise(true);
    runCycleBtn->setIcon(createCycleIcon());
    runCycleBtn->setIconSize(QSize(24, 24));
    runCycleBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    runCycleBtn->setObjectName("ProcessCycleBtn");

    processToolBarLayout->addWidget(m_btnStartPause);
    processToolBarLayout->addWidget(m_btnStop);
    processToolBarLayout->addWidget(runCycleBtn);
    processPanelLayout->addWidget(processToolBar);

    // 模块树
    m_processTree = new QTreeWidget();
    m_processTree->setHeaderHidden(true);
    m_processTree->setAcceptDrops(true);
    m_processTree->setDragDropMode(QAbstractItemView::DropOnly);
    m_processTree->setAnimated(true);
    m_processTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_processTree->setDefaultDropAction(Qt::MoveAction);
    m_processTree->setDropIndicatorShown(true);
    m_processTree->setObjectName("ProcessTree");
    // 配置两列布局：模块名 + 耗时
    m_processTree->setColumnCount(2);
    m_processTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_processTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_processTree->header()->setMinimumSectionSize(60);
    m_processTree->header()->setHidden(true);
    // 安装事件过滤器以处理拖拽放置和右键菜单
    m_processTree->viewport()->installEventFilter(this);
    m_processTree->installEventFilter(this);
    connect(m_processTree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onProcessTreeContextMenu);

    // 提示标签（单独创建，不放在树中）
    m_hintLabel = new QLabel(tr("← 从左侧拖拽工具"));
    m_hintLabel->setAlignment(Qt::AlignCenter);
    m_hintLabel->setStyleSheet("color: #808080; padding: 10px;");
    m_hintLabel->setObjectName("ProcessTreeHintLabel");
    processPanelLayout->addWidget(m_hintLabel);

    processPanelLayout->addWidget(m_processTree);
    processPanelLayout->setStretchFactor(m_processTree, 1);

    // 流程状态栏
    m_processStatusWidget = new QWidget();
    QHBoxLayout* processStatusLayout = new QHBoxLayout(m_processStatusWidget);
    processStatusLayout->setContentsMargins(5, 5, 5, 5);
    m_processTimeLabel = new QLabel(tr("总耗时：0 ms"));
    processStatusLayout->addWidget(m_processTimeLabel);
    processStatusLayout->addStretch();
    processPanelLayout->addWidget(m_processStatusWidget);
    processPanelLayout->setStretchFactor(m_processStatusWidget, 0);

    // 流程 DockWidget
    m_processDock = new QDockWidget(this);
    m_processDock->setObjectName("ProcessPanelDock");
    m_processDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    m_processDock->setWidget(processPanelWidget);
    m_processDock->setMinimumWidth(320);
    m_processDock->setMaximumWidth(500);
    // 自定义标题栏
    QWidget* processTitleWidget = new QWidget();
    QHBoxLayout* processTitleLayout = new QHBoxLayout(processTitleWidget);
    processTitleLayout->setContentsMargins(10, 0, 5, 0);
    processTitleLayout->setSpacing(5);
    QLabel* processTitleLabel = new QLabel(tr("流程"));
    processTitleLabel->setObjectName("ProcessTitleLabel");
    processTitleLayout->addWidget(processTitleLabel);
    processTitleLayout->addStretch();
    QToolButton* processCloseBtn = new QToolButton();
    processCloseBtn->setText("×");
    processCloseBtn->setToolTip(tr("关闭"));
    processCloseBtn->setFixedSize(20, 20);
    processCloseBtn->setObjectName("ProcessCloseBtn");
    processTitleLayout->addWidget(processCloseBtn);
    processTitleWidget->setMinimumHeight(36);
    processTitleWidget->setObjectName("ProcessTitleWidget");
    m_processDock->setTitleBarWidget(processTitleWidget);
    connect(processCloseBtn, &QToolButton::clicked, m_processDock, &QDockWidget::close);

    // 图像显示区域 - 使用 DisplayManager 的中央视口
    QWidget* imageDisplayWidget = m_displayManager->centralDisplay();
    imageDisplayWidget->setObjectName("ImageDisplayWidget");
    imageDisplayWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    imageDisplayWidget->setMinimumSize(0, 0);

    // 添加到右上方水平 Splitter
    rightTopSplitter->addWidget(m_processDock);
    rightTopSplitter->addWidget(imageDisplayWidget);
    rightTopSplitter->setStretchFactor(0, 1);
    rightTopSplitter->setStretchFactor(1, 10);

    rightSplitter->addWidget(rightTopSplitter);

    // 日志面板 - 下方区域
    m_logDock = new QDockWidget(this);
    m_logDock->setObjectName("LogDock");
    m_logDock->setFeatures(QDockWidget::NoDockWidgetFeatures);

    // 创建 Tab 容器（日志 + 终端）
    m_logTerminalTabs = new QTabWidget();
    m_logTerminalTabs->setObjectName("LogTerminalTabs");
    m_logTerminalTabs->setMovable(false);
    m_logTerminalTabs->setDocumentMode(true);

    // 隐藏 dock 标题栏，节省空间
    m_logDock->setTitleBarWidget(new QWidget());

    // ===== Tab 1: 日志面板 =====
    QWidget* logWidget = new QWidget();
    QVBoxLayout* logLayout = new QVBoxLayout(logWidget);
    logLayout->setContentsMargins(0, 0, 0, 0);
    logLayout->setSpacing(0);

    m_logTable = new QTableWidget();
    m_logTable->setColumnCount(3);
    m_logTable->setHorizontalHeaderLabels(QStringList() << tr("时间") << tr("级别 ▼") << tr("消息"));
    m_logTable->horizontalHeader()->setStretchLastSection(true);
    m_logTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_logTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_logTable->setColumnWidth(0, 90);
    m_logTable->setColumnWidth(1, 70);

    // vertical header 固定宽度，替代重复的水平序号列
    m_logTable->verticalHeader()->setDefaultSectionSize(24);
    m_logTable->verticalHeader()->setMinimumWidth(40);
    m_logTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_logTable->setObjectName("LogTable");

    logLayout->addWidget(m_logTable);

    // 连接日志信号到表格
    connect(&Logger::instance(), &Logger::logAdded, this, &MainWindow::onLogAdded);

    // 点击表头"级别"列弹出筛选菜单
    connect(m_logTable->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int logicalIndex) {
        if (logicalIndex == 1) showLogLevelMenu();
    });

    int logTabIndex = m_logTerminalTabs->addTab(logWidget, tr("📄 日志"));

    // Tab "📄 日志" 再次点击时打开日志文件（第一次点击切换，第二次点击打开）
    connect(m_logTerminalTabs, &QTabWidget::tabBarClicked, this, [this, logTabIndex](int index) {
        if (index == logTabIndex && m_logTerminalTabs->currentIndex() == logTabIndex) {
            QString logPath = Logger::instance().logFilePath();
            QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
        }
    });

    // ===== Tab 2: 终端 =====
    m_terminalWidget = new TerminalWidget();
    TerminalBridge::instance().initialize(m_terminalWidget);
    m_logTerminalTabs->addTab(m_terminalWidget, tr("🖥️ 终端"));

    // ===== Tab 3: Agent Chat =====
    m_agentChatPanel = new AgentChatPanel();
    m_logTerminalTabs->addTab(m_agentChatPanel, tr("🤖 Agent Chat"));
    connect(m_agentChatPanel, &AgentChatPanel::userMessageSent,
            this, [this](const QString& msg) {
                m_agentChatPanel->setThinking(true);
                AgentController::instance().sendUserMessage(msg);
            });
    connect(m_agentChatPanel, &AgentChatPanel::userMessageWithImagesSent,
            this, [this](const QString& msg, const QList<QPixmap>& images) {
                m_agentChatPanel->setThinking(true);
                AgentController::instance().sendUserMessageWithImages(msg, images);
            });
    connect(&AgentController::instance(), &AgentController::llmResponseReceived,
            this, [this](const QString& content, const QJsonArray& toolCalls) {
                Q_UNUSED(toolCalls);
                m_agentChatPanel->setThinking(false);
                m_agentChatPanel->addMessage(AgentMessageBubble::Sender::Agent, content);
            });
    connect(&AgentController::instance(), &AgentController::llmErrorOccurred,
            this, [this](const QString& error) {
                m_agentChatPanel->setThinking(false);
                m_agentChatPanel->addMessage(AgentMessageBubble::Sender::System,
                                             QString("Error: %1").arg(error));
                AgentActionLogEntry entry;
                entry.timestamp = QDateTime::currentDateTime();
                entry.actor = "System";
                entry.action = "LLM Error";
                entry.params = error;
                entry.result = "error";
                entry.undoable = false;
                m_agentActionLogWidget->addEntry(entry);
            });
    connect(&AgentController::instance(), &AgentController::toolsPendingConfirmation,
            this, [this](const QJsonArray& toolCalls) {
                m_agentChatPanel->setThinking(false);
                QList<AgentToolPreviewCard::ToolItem> items;
                for (const QJsonValue& v : toolCalls) {
                    QJsonObject tc = v.toObject();
                    AgentToolPreviewCard::ToolItem item;
                    item.name = tc["name"].toString();
                    if (item.name.isEmpty()) {
                        item.name = tc["function"].toObject()["name"].toString();
                        item.params = QJsonDocument::fromJson(
                            tc["function"].toObject()["arguments"].toString().toUtf8()).object();
                    } else {
                        item.params = tc["arguments"].toObject();
                    }
                    items.append(item);
                }
                m_agentChatPanel->showToolPreview(items);
            });
    connect(m_agentChatPanel, &AgentChatPanel::toolPreviewConfirmed,
            &AgentController::instance(), &AgentController::confirmPendingTools);
    connect(m_agentChatPanel, &AgentChatPanel::toolPreviewCancelled,
            &AgentController::instance(), &AgentController::rejectPendingTools);

    // ===== Tab 4: Agent Action Log =====
    m_agentActionLogWidget = new AgentActionLogWidget();
    m_logTerminalTabs->addTab(m_agentActionLogWidget, tr("📋 Agent Log"));
    connect(&AgentController::instance(), &AgentController::actionLogEntryAdded,
            m_agentActionLogWidget, &AgentActionLogWidget::addEntry);
    connect(&AgentController::instance(), &AgentController::agentLoopIterating,
            this, [this]() { m_agentChatPanel->setThinking(true); });

    m_logDock->setWidget(m_logTerminalTabs);
    m_logDock->setMinimumHeight(250);

    rightSplitter->addWidget(m_logDock);
    rightSplitter->setStretchFactor(0, 10);
    rightSplitter->setStretchFactor(1, 5);

    // 将右侧 Splitter 添加到主 Splitter
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 4);

    // 设置中央 widget
    setCentralWidget(mainSplitter);

    // 连接 DockWidget 关闭信号以同步菜单勾选状态
    connect(m_toolBoxDock, &QDockWidget::topLevelChanged, this,
            [this](bool) { m_viewToolPanelAction->setChecked(!m_toolBoxDock->isHidden()); });
    connect(m_processDock, &QDockWidget::topLevelChanged, this,
            [this](bool) { m_viewProcessPanelAction->setChecked(!m_processDock->isHidden()); });

    // 连接按钮信号
    connect(m_btnStartPause, &QToolButton::clicked, this, [this]() {
        if (m_isRunning) {
            onStop();
        } else {
            onRunOnce();
        }
    });
    connect(m_btnStop, &QToolButton::clicked, this, &MainWindow::onStop);
}

void MainWindow::addToolBoxItem(QTreeWidgetItem* parent, const QString& displayName, const QString& pluginName) {
    QTreeWidgetItem* item = new QTreeWidgetItem(parent, QStringList(displayName));
    item->setData(0, Qt::UserRole, "plugin");
    item->setData(0, Qt::UserRole + 1, pluginName);
}

void MainWindow::onToggleToolPanel(bool checked) {
    if (m_toolBoxDock) {
        m_toolBoxDock->setVisible(checked);
    }
}

void MainWindow::onToggleProcessPanel(bool checked) {
    if (m_processDock) {
        m_processDock->setVisible(checked);
    }
}

void MainWindow::onProcessTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_processTree->itemAt(pos);
    if (!item || item->data(0, Qt::UserRole).toString() != "flow_item") {
        return;
    }

    QMenu menu(this);
    QAction* deleteAction = menu.addAction(tr("删除"));
    deleteAction->setIcon(QIcon(":/icons/delete.png"));

    QAction* selectedAction = menu.exec(m_processTree->mapToGlobal(pos));
    if (selectedAction == deleteAction) {
        QString instanceName = item->data(0, Qt::UserRole + 1).toString();
        if (!instanceName.isEmpty() && m_flowModules.contains(instanceName)) {
            IModule* module = m_flowModules.value(instanceName);
            module->shutdown();
            delete module;
            m_flowModules.remove(instanceName);
            m_usedPluginNames.remove(instanceName);
        }
        delete item;
        m_modulesNeedSync = true;

        // 如果所有 item 都被删除，重新创建提示标签
        if (m_processTree->topLevelItemCount() == 0 && !m_hintLabel) {
            QWidget* parentWidget = m_processTree->parentWidget();
            if (parentWidget) {
                QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(parentWidget->layout());
                if (layout) {
                    m_hintLabel = new QLabel(tr("← 从左侧拖拽工具"));
                    m_hintLabel->setAlignment(Qt::AlignCenter);
                    m_hintLabel->setStyleSheet("color: #808080; padding: 10px;");
                    m_hintLabel->setObjectName("ProcessTreeHintLabel");
                    int index = layout->indexOf(m_processTree);
                    layout->insertWidget(index, m_hintLabel);
                }
            }
        }
    }
}

void MainWindow::applyTheme() {
    // 首先更新 ConfigWidgetHelper 的全局主题状态
    ConfigWidgetHelper::setGlobalDarkTheme(m_isDarkTheme);

    // 更新所有子控件的 ConfigWidgetHelper 样式（包括对话框中的控件）
    ConfigWidgetHelper::updateAllWidgetsStyle(this, m_isDarkTheme);

    if (m_isDarkTheme) {
        // 深色主题
        setStyleSheet(
            "QMainWindow { background-color: #1e1e1e; color: #ffffff; }"
            "QDockWidget { background-color: #252525; color: #ffffff; border: none; }"
            "QDockWidget::title { "
            "    background-color: #2d2d2d; "
            "    color: #ffffff; "
            "    font-weight: bold; "
            "    font-size: 13px; "
            "    padding: 8px 10px; "
            "    border-bottom: 1px solid #444444; }"
            "QDockWidget::title:hover { background-color: #333333; }"
            "QTreeWidget { background-color: #252525; color: #ffffff; border: none; }"
            "QTreeWidget::item { height: 28px; }"
            "QTreeWidget::item:hover { background-color: #3a3a3a; }"
            "QTreeWidget::item:selected { background-color: #0078d7; }"
            "QTableWidget { background-color: #252525; color: #ffffff; border: none; }"
            "QTableWidget::item { border-bottom: 1px solid #333; }"
            "QTableWidget::item:selected { background-color: #0078d7; }"
            "QHeaderView::section { background-color: #333333; padding: 5px; border: none; }"
            "QTableCornerButton::section { background-color: #333333; border: none; }"
            "QScrollBar:vertical { background-color: #252525; width: 12px; border: none; }"
            "QScrollBar::handle:vertical { background-color: #555555; min-height: 20px; border-radius: 6px; }"
            "QScrollBar::handle:vertical:hover { background-color: #666666; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
            "QScrollBar:horizontal { background-color: #252525; height: 12px; border: none; }"
            "QScrollBar::handle:horizontal { background-color: #555555; min-width: 20px; border-radius: 6px; }"
            "QScrollBar::handle:horizontal:hover { background-color: #666666; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
            "QToolBar { background-color: #252525; border: 1px solid #444444; spacing: 5px; padding: 2px; }"
            "QToolBar QToolButton { background-color: transparent; padding: 5px; border: 1px solid transparent; }"
            "QToolBar QToolButton:hover { background-color: #3a3a3a; border: 1px solid #555555; }"
            "QMenuBar { background-color: #252525; color: #ffffff; }"
            "QMenuBar::item:selected { background-color: #3a3a3a; }"
            "QMenu { background-color: #252525; color: #ffffff; border: 1px solid #333; }"
            "QMenu::item:selected { background-color: #0078d7; }"
            "QStatusBar { background-color: #252525; color: #ffffff; }"
            "QPushButton { background-color: #0078d7; color: white; padding: 5px 15px; border: none; }"
            "QPushButton:hover { background-color: #1e8ad6; }"
            "QPushButton:disabled { background-color: #555555; }"
            "QLineEdit { background-color: #333333; color: white; border: 1px solid #555; padding: 5px; }"
            "QComboBox { background-color: #333333; color: white; border: 1px solid #555; padding: 5px; }"
            "QSpinBox { background-color: #333333; color: white; border: 1px solid #555; padding: 5px; }"
            "QSplitter::handle { background-color: transparent; }"
            "QLabel { color: #ffffff; }"
            "QScrollArea { background-color: #252525; }"
            "QFrame { background-color: #444444; }"
            "QTabWidget::pane { border: none; background-color: #252525; }"
            "QTabBar { background-color: #252525; }"
            "QTabBar::tab { background-color: #333333; color: #ffffff; padding: 5px 12px; border: none; }"
            "QTabBar::tab:selected { background-color: #444444; }"
            "QTabBar::tab:hover:!selected { background-color: #3a3a3a; }");
    } else {
        // 白色主题 - 适合亮光环境
        setStyleSheet(
            "QMainWindow { background-color: #f5f5f5; color: #212121; }"
            "QDockWidget { background-color: #ffffff; color: #212121; border: none; }"
            "QDockWidget::title { "
            "    background-color: #e8e8e8; "
            "    color: #212121; "
            "    font-weight: bold; "
            "    font-size: 13px; "
            "    padding: 8px 10px; "
            "    border-bottom: 1px solid #cccccc; }"
            "QDockWidget::title:hover { background-color: #d0d0d0; }"
            "QTreeWidget { background-color: #ffffff; color: #212121; border: 1px solid #dddddd; }"
            "QTreeWidget::item { height: 28px; }"
            "QTreeWidget::item:hover { background-color: #e5f3ff; }"
            "QTreeWidget::item:selected { background-color: #0078d7; color: #ffffff; }"
            "QTableWidget { background-color: #ffffff; color: #212121; border: 1px solid #dddddd; }"
            "QTableWidget::item { border-bottom: 1px solid #eeeeee; }"
            "QTableWidget::item:selected { background-color: #0078d7; color: #ffffff; }"
            "QHeaderView::section { background-color: #f0f0f0; color: #212121; padding: 5px; border: 1px solid "
            "#dddddd; }"
            "QTableCornerButton::section { background-color: #f0f0f0; border: none; }"
            "QScrollBar:vertical { background-color: #f0f0f0; width: 12px; border: none; }"
            "QScrollBar::handle:vertical { background-color: #c0c0c0; min-height: 20px; border-radius: 6px; }"
            "QScrollBar::handle:vertical:hover { background-color: #a0a0a0; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
            "QScrollBar:horizontal { background-color: #f0f0f0; height: 12px; border: none; }"
            "QScrollBar::handle:horizontal { background-color: #c0c0c0; min-width: 20px; border-radius: 6px; }"
            "QScrollBar::handle:horizontal:hover { background-color: #a0a0a0; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
            "QToolBar { background-color: #f8f8f8; border: 1px solid #dddddd; spacing: 5px; padding: 2px; }"
            "QToolBar QToolButton { background-color: transparent; color: #212121; padding: 5px; border: 1px solid "
            "transparent; }"
            "QToolBar QToolButton:hover { background-color: #e5f3ff; border: 1px solid #0078d7; }"
            "QMenuBar { background-color: #f8f8f8; color: #212121; }"
            "QMenuBar::item:selected { background-color: #e5f3ff; }"
            "QMenu { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; }"
            "QMenu::item:selected { background-color: #0078d7; color: #ffffff; }"
            "QStatusBar { background-color: #f8f8f8; color: #212121; }"
            "QPushButton { background-color: #0078d7; color: white; padding: 5px 15px; border: 1px solid #005a9e; }"
            "QPushButton:hover { background-color: #1e8ad6; }"
            "QPushButton:disabled { background-color: #cccccc; color: #999999; }"
            "QLineEdit { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; padding: 5px; }"
            "QLineEdit:focus { border: 1px solid #0078d7; }"
            "QComboBox { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; padding: 5px; }"
            "QComboBox:focus { border: 1px solid #0078d7; }"
            "QSpinBox { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; padding: 5px; }"
            "QSpinBox:focus { border: 1px solid #0078d7; }"
            "QSplitter::handle { background-color: transparent; }"
            "QLabel { color: #212121; }"
            "QScrollArea { background-color: #ffffff; }"
            "QFrame { background-color: #dddddd; }"
            "QTabWidget::pane { border: none; background-color: #ffffff; }"
            "QTabBar { background-color: #ffffff; }"
            "QTabBar::tab { background-color: #e8e8e8; color: #212121; padding: 5px 12px; border: none; }"
            "QTabBar::tab:selected { background-color: #f5f5f5; }"
            "QTabBar::tab:hover:!selected { background-color: #d0d0d0; }");
    }

    // 更新自定义标题栏样式
    QString bgColor = m_isDarkTheme ? "#2d2d2d" : "#e8e8e8";
    QString borderColor = m_isDarkTheme ? "#444444" : "#cccccc";
    QString textColor = m_isDarkTheme ? "#ffffff" : "#212121";
    QString btnColor = m_isDarkTheme ? "#ffffff" : "#212121";
    QString treeBgColor = m_isDarkTheme ? "#252525" : "#ffffff";
    QString treeTextColor = m_isDarkTheme ? "#ffffff" : "#212121";
    QString treeHoverColor = m_isDarkTheme ? "#3a3a3a" : "#e5f3ff";
    QString scrollBgColor = m_isDarkTheme ? "#252525" : "#ffffff";
    QString sepColor = m_isDarkTheme ? "#444444" : "#dddddd";

    if (m_toolBoxDock && m_toolBoxDock->titleBarWidget()) {
        m_toolBoxDock->titleBarWidget()->setStyleSheet(
            QString("background-color: %1; border: none; border-bottom: 1px solid %2;").arg(bgColor, borderColor));
        QLabel* label = m_toolBoxDock->titleBarWidget()->findChild<QLabel*>();
        if (label)
            label->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; font-size: 14px; }").arg(textColor));
        QToolButton* btn = m_toolBoxDock->titleBarWidget()->findChild<QToolButton*>();
        if (btn)
            btn->setStyleSheet(
                QString("QToolButton { background-color: transparent; color: %1; font-size: 18px; border: none; }"
                        "QToolButton:hover { background-color: #e74c3c; }")
                    .arg(btnColor));
    }
    if (m_processDock && m_processDock->titleBarWidget()) {
        m_processDock->titleBarWidget()->setStyleSheet(
            QString("background-color: %1; border: none; border-bottom: 1px solid %2;").arg(bgColor, borderColor));
        QLabel* label = m_processDock->titleBarWidget()->findChild<QLabel*>();
        if (label)
            label->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; font-size: 14px; }").arg(textColor));
        QToolButton* btn = m_processDock->titleBarWidget()->findChild<QToolButton*>();
        if (btn)
            btn->setStyleSheet(
                QString("QToolButton { background-color: transparent; color: %1; font-size: 18px; border: none; }"
                        "QToolButton:hover { background-color: #e74c3c; }")
                    .arg(btnColor));
    }
    if (m_logDock && m_logDock->titleBarWidget()) {
        m_logDock->titleBarWidget()->setStyleSheet(
            QString("background-color: %1; border: none; border-bottom: 1px solid %2;").arg(bgColor, borderColor));
        QLabel* label = m_logDock->titleBarWidget()->findChild<QLabel*>();
        if (label)
            label->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; font-size: 14px; }").arg(textColor));
    }

    // 更新 TreeWidget 样式
    if (m_toolBoxTree) {
        m_toolBoxTree->setStyleSheet(
            QString("QTreeWidget { background-color: %1; color: %2; border: none; }"
                    "QTreeWidget::item { height: 28px; }"
                    "QTreeWidget::item:hover { background-color: %3; }"
                    "QTreeWidget::item:selected { background-color: #0078d7; color: #ffffff; }")
                .arg(treeBgColor, treeTextColor, treeHoverColor));
    }
    if (m_processTree) {
        m_processTree->setStyleSheet(
            QString("QTreeWidget { background-color: %1; color: %2; border: none; }"
                    "QTreeWidget::item { height: 28px; }"
                    "QTreeWidget::item:hover { background-color: %3; }"
                    "QTreeWidget::item:selected { background-color: #0078d7; color: #ffffff; }")
                .arg(treeBgColor, treeTextColor, treeHoverColor));
    }

    // 更新滚动区域样式
    QScrollArea* toolCategoryScroll = findChild<QScrollArea*>("ToolCategoryScroll");
    if (toolCategoryScroll) {
        toolCategoryScroll->setStyleSheet(QString("QScrollArea { background-color: %1; border: none; }"
                                                  "QScrollArea > QWidget > QWidget { background-color: %1; }")
                                              .arg(scrollBgColor));
    }

    // 更新分割线样式
    QFrame* toolPanelSeparator = findChild<QFrame*>("ToolPanelSeparator");
    if (toolPanelSeparator) {
        toolPanelSeparator->setStyleSheet(QString("background-color: %1;").arg(sepColor));
    }

    // 更新流程状态栏样式
    if (m_processStatusWidget) {
        m_processStatusWidget->setStyleSheet(QString("background-color: %1;").arg(bgColor));
        if (m_processTimeLabel) {
            m_processTimeLabel->setStyleSheet(QString("QLabel { color: %1; }").arg(textColor));
        }
    }

    // 更新工具栏按钮区域样式
    QWidget* toolToolBar = findChild<QWidget*>("ToolToolBar");
    if (toolToolBar) {
        toolToolBar->setStyleSheet(QString("background-color: %1;").arg(scrollBgColor));
    }

    // 更新工具栏按钮样式
    QString toolBtnBgColor = m_isDarkTheme ? "#3a3a3a" : "#e0e0e0";
    QString toolBtnHoverColor = m_isDarkTheme ? "#4a4a4a" : "#d0d0d0";
    QString toolBtnTextColor = m_isDarkTheme ? "#ffffff" : "#212121";
    QString toolBtnBorderColor = m_isDarkTheme ? "#555555" : "#cccccc";
    QToolButton* createProcessBtn = findChild<QToolButton*>("ToolCreateProcessBtn");
    if (createProcessBtn) {
        createProcessBtn->setStyleSheet(
            QString("QToolButton { background-color: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: "
                    "4px 6px; }"
                    "QToolButton:hover { background-color: %4; }")
                .arg(toolBtnBgColor, toolBtnTextColor, toolBtnBorderColor, toolBtnHoverColor));
    }
    QToolButton* deleteProcessBtn = findChild<QToolButton*>("ToolDeleteProcessBtn");
    if (deleteProcessBtn) {
        deleteProcessBtn->setStyleSheet(
            QString("QToolButton { background-color: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: "
                    "4px 6px; }"
                    "QToolButton:hover { background-color: %4; }")
                .arg(toolBtnBgColor, toolBtnTextColor, toolBtnBorderColor, toolBtnHoverColor));
    }
    QToolButton* createMethodBtn = findChild<QToolButton*>("ToolCreateMethodBtn");
    if (createMethodBtn) {
        createMethodBtn->setStyleSheet(
            QString("QToolButton { background-color: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: "
                    "4px 6px; }"
                    "QToolButton:hover { background-color: %4; }")
                .arg(toolBtnBgColor, toolBtnTextColor, toolBtnBorderColor, toolBtnHoverColor));
    }
    QToolButton* toolSettingsBtn = findChild<QToolButton*>("ToolSettingsBtn");
    if (toolSettingsBtn) {
        toolSettingsBtn->setStyleSheet(
            QString("QToolButton { background-color: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: "
                    "4px 6px; }"
                    "QToolButton:hover { background-color: %4; }")
                .arg(toolBtnBgColor, toolBtnTextColor, toolBtnBorderColor, toolBtnHoverColor));
    }

    // 更新视图切换按钮样式（已移除）

    // 更新流程工具栏按钮样式
    QString btnBgColor = m_isDarkTheme ? "#3a3a3a" : "#e0e0e0";
    QString btnTextColor = m_isDarkTheme ? "#ffffff" : "#212121";
    QString btnHoverColor = m_isDarkTheme ? "#4a4a4a" : "#d0d0d0";
    QPushButton* startPauseBtn = findChild<QPushButton*>("ProcessStartPauseBtn");
    if (startPauseBtn) {
        startPauseBtn->setStyleSheet(QString("QPushButton { background-color: %1; color: %2; border: 1px solid "
                                             "#005a9e; border-radius: 3px; padding: 5px 10px; }"
                                             "QPushButton:hover { background-color: %3; }"
                                             "QPushButton:disabled { background-color: #cccccc; color: #999999; }")
                                         .arg(btnBgColor, btnTextColor, btnHoverColor));
    }
    QPushButton* stopBtn = findChild<QPushButton*>("ProcessStopBtn");
    if (stopBtn) {
        stopBtn->setStyleSheet(QString("QPushButton { background-color: #dc3545; color: white; border: 1px solid "
                                       "#c82333; border-radius: 3px; padding: 5px 10px; }"
                                       "QPushButton:hover { background-color: #e04242; }"
                                       "QPushButton:disabled { background-color: #cccccc; color: #999999; }"));
    }
    QPushButton* cycleBtn = findChild<QPushButton*>("ProcessCycleBtn");
    if (cycleBtn) {
        cycleBtn->setStyleSheet(QString("QPushButton { background-color: %1; color: %2; border: 1px solid #005a9e; "
                                        "border-radius: 3px; padding: 5px 10px; }"
                                        "QPushButton:hover { background-color: %3; }")
                                    .arg(btnBgColor, btnTextColor, btnHoverColor));
    }

    // 更新流程工具栏背景
    QWidget* procToolBar = findChild<QWidget*>("ProcessToolBar");
    if (procToolBar) {
        procToolBar->setStyleSheet(QString("background-color: %1;").arg(scrollBgColor));
    }

    // 更新视图切换按钮容器背景
    QWidget* viewToggleWidget = findChild<QWidget*>("ViewToggleWidget");
    if (viewToggleWidget) {
        viewToggleWidget->setStyleSheet(QString("background-color: %1;").arg(scrollBgColor));
    }

    // 更新面板容器背景
    QWidget* toolPanelWidget = findChild<QWidget*>("ToolPanelWidget");
    if (toolPanelWidget) {
        toolPanelWidget->setStyleSheet(QString("background-color: %1;").arg(scrollBgColor));
    }
    QWidget* processPanelWidget = findChild<QWidget*>("ProcessPanelWidget");
    if (processPanelWidget) {
        processPanelWidget->setStyleSheet(QString("background-color: %1;").arg(scrollBgColor));
    }

    // 更新 DisplayManager 中的 Viewport 样式
    if (m_displayManager) {
        m_displayManager->applyTheme(m_isDarkTheme);
    }

    // 更新 Terminal 主题颜色
    if (m_terminalWidget) {
        QColor termFg = m_isDarkTheme ? QColor("#d4d4d4") : QColor("#212121");
        QColor termBg = m_isDarkTheme ? QColor("#1e1e1e") : QColor("#ffffff");
        QColor termSel = m_isDarkTheme ? QColor("#264f78") : QColor("#b4d7ff");
        m_terminalWidget->setThemeColors(termFg, termBg, termSel);
    }

    // 更新 Agent Chat 面板主题
    if (m_agentChatPanel) {
        m_agentChatPanel->applyTheme(m_isDarkTheme);
    }

    // 更新 Agent Action Log 主题
    if (m_agentActionLogWidget) {
        QString logBg = m_isDarkTheme ? "#252525" : "#ffffff";
        QString logText = m_isDarkTheme ? "#ffffff" : "#212121";
        m_agentActionLogWidget->setStyleSheet(QString(
            "AgentActionLogWidget { background-color: %1; }"
            "AgentActionLogWidget QTableWidget { background-color: %1; color: %2; border: none; }"
            "AgentActionLogWidget QTableWidget::item { border-bottom: 1px solid %3; }"
            "AgentActionLogWidget QTableWidget::item:selected { background-color: #0078d7; }"
            "AgentActionLogWidget QHeaderView::section { background-color: %4; color: %2; padding: 5px; border: none; }"
            "AgentActionLogWidget QPushButton { background-color: #0078d7; color: white; padding: 4px 10px; border: none; }"
            "AgentActionLogWidget QPushButton:hover { background-color: #1e8ad6; }")
            .arg(logBg, logText,
                 m_isDarkTheme ? "#333333" : "#eeeeee",
                 m_isDarkTheme ? "#333333" : "#f0f0f0"));
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // 处理工具箱的拖拽开始事件 - 更新当前选中的项
    if (m_toolBoxTree && watched == m_toolBoxTree && event->type() == QEvent::DragEnter) {
        QTreeWidgetItem* item = m_toolBoxTree->itemAt(m_toolBoxTree->mapFromGlobal(QCursor::pos()));
        if (item && item->data(0, Qt::UserRole).toString() == "plugin") {
            m_currentToolBoxItem = item;
        } else {
            // 不是插件项，拒绝拖拽
            m_currentToolBoxItem = nullptr;
        }
    }

    // 处理流程树的双击事件
    if (m_processTree && event->type() == QEvent::MouseButtonDblClick) {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
        QTreeWidgetItem* item = nullptr;

        if (watched == m_processTree) {
            item = m_processTree->itemAt(mouseEvent->pos());
        } else if (m_processTree->viewport() && watched == m_processTree->viewport()) {
            item = m_processTree->itemAt(mouseEvent->pos());
        }

        if (item && item->data(0, Qt::UserRole).toString() == "flow_item") {
            QString instanceName = item->data(0, Qt::UserRole + 1).toString();
            if (!instanceName.isEmpty() && m_flowModules.contains(instanceName)) {
                IModule* module = m_flowModules.value(instanceName);
                QWidget* configWidget = module->createConfigWidget();
                if (configWidget) {
                    // 创建对话框来显示配置控件
                    QDialog dialog(this);
                    dialog.setWindowTitle(tr("配置 - %1").arg(item->text(0)));
                    dialog.setMinimumSize(400, 300);
                    QVBoxLayout* layout = new QVBoxLayout(&dialog);
                    // configWidget 的父对象已经是 module，不需要重新设置 parent
                    // 使用 layout->addWidget 会自动转移所有权到 dialog
                    layout->addWidget(configWidget);
                    QHBoxLayout* btnLayout = new QHBoxLayout();
                    QPushButton* okBtn = new QPushButton(tr("确定"));
                    QPushButton* cancelBtn = new QPushButton(tr("取消"));
                    btnLayout->addStretch();
                    btnLayout->addWidget(okBtn);
                    btnLayout->addWidget(cancelBtn);
                    layout->addLayout(btnLayout);

                    connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
                    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

                    if (dialog.exec() == QDialog::Accepted) {
                        Logger::instance().info(tr("模块参数已更新：%1").arg(item->text(0)), "Config");
                    }
                } else {
                    Logger::instance().warning(tr("该模块没有配置选项：%1").arg(item->text(0)), "Config");
                }
            }
            return true;
        }
    }

    // 处理流程树的键盘删除
    if (m_processTree && (watched == m_processTree || watched == m_processTree->viewport()) &&
        event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Delete) {
            QList<QTreeWidgetItem*> selected = m_processTree->selectedItems();
            if (!selected.isEmpty()) {
                QTreeWidgetItem* item = selected.first();
                QString instanceName = item->data(0, Qt::UserRole + 1).toString();
                if (!instanceName.isEmpty() && m_flowModules.contains(instanceName)) {
                    IModule* module = m_flowModules.value(instanceName);
                    module->shutdown();
                    delete module;
                    m_flowModules.remove(instanceName);
                    m_usedPluginNames.remove(instanceName);
                }
                delete item;
                m_modulesNeedSync = true;
                // 如果所有 item 都被删除，重新创建提示标签
                if (m_processTree->topLevelItemCount() == 0 && !m_hintLabel) {
                    // 查找 processPanelLayout
                    QWidget* parentWidget = m_processTree->parentWidget();
                    if (parentWidget) {
                        QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(parentWidget->layout());
                        if (layout) {
                            m_hintLabel = new QLabel(tr("← 从左侧拖拽工具"));
                            m_hintLabel->setAlignment(Qt::AlignCenter);
                            m_hintLabel->setStyleSheet("color: #808080; padding: 10px;");
                            m_hintLabel->setObjectName("ProcessTreeHintLabel");
                            int index = layout->indexOf(m_processTree);
                            layout->insertWidget(index, m_hintLabel);
                        }
                    }
                }
            }
            return true;
        }
    }

    // 处理拖放到流程树
    if (m_processTree) {
        QWidget* processViewport = m_processTree->viewport();
        if (processViewport && watched == processViewport && event->type() == QEvent::Drop) {
            QDropEvent* dropEvent = static_cast<QDropEvent*>(event);

            // 获取拖放的数据
            const QMimeData* mimeData = dropEvent->mimeData();
            if (!mimeData) {
                return QMainWindow::eventFilter(watched, event);
            }

            // 检查是否有来自工具箱的拖放
            if (mimeData->hasFormat("application/x-qabstractitemmodeldatalist")) {
                // 优先使用已记录的选中项，否则尝试获取当前项
                QTreeWidgetItem* sourceItem = m_currentToolBoxItem;
                if (!sourceItem) {
                    sourceItem = m_toolBoxTree->currentItem();
                }
                // 只允许插件类型的项被拖拽到流程树
                if (sourceItem && sourceItem->data(0, Qt::UserRole).toString() == "plugin") {
                    QString pluginName = sourceItem->data(0, Qt::UserRole + 1).toString();
                    if (!pluginName.isEmpty()) {
                        // 隐藏提示标签
                        if (m_hintLabel) {
                            m_hintLabel->setVisible(false);
                            m_hintLabel->deleteLater();
                            m_hintLabel = nullptr;
                        }

                        // 获取放置位置 - 根据鼠标在 item 内的高度判断插入上方还是下方
                        QTreeWidgetItem* hoverItem = m_processTree->itemAt(dropEvent->pos());
                        int insertRow;
                        if (!hoverItem) {
                            // 悬停在空白处，添加到末尾
                            insertRow = m_processTree->topLevelItemCount();
                        } else {
                            // 获取 item 的几何区域，判断鼠标在上方还是下方
                            QRect itemRect = m_processTree->visualItemRect(hoverItem);
                            int itemMiddle = itemRect.top() + itemRect.height() / 2;
                            if (dropEvent->pos().y() < itemMiddle) {
                                // 鼠标在上半部分，插入到此 item 之前
                                insertRow = m_processTree->indexOfTopLevelItem(hoverItem);
                            } else {
                                // 鼠标在下半部分，插入到此 item 之后
                                insertRow = m_processTree->indexOfTopLevelItem(hoverItem) + 1;
                            }
                        }

                        // 手动添加 item 到树中
                        QString displayName = sourceItem->text(0);
                        QTreeWidgetItem* newItem = new QTreeWidgetItem();
                        newItem->setText(0, displayName);
                        m_processTree->insertTopLevelItem(insertRow, newItem);

                        // 生成唯一的实例名称
                        QString instanceName = pluginName;
                        int counter = 1;
                        while (m_usedPluginNames.contains(instanceName)) {
                            instanceName = QString("%1_%2").arg(pluginName).arg(counter++);
                        }
                        m_usedPluginNames.insert(instanceName);

                        // 创建插件实例
                        DeepLux::PluginManager& pm = DeepLux::PluginManager::instance();
                        IModule* module = pm.createModule(pluginName);

                        if (!module) {
                            Logger::instance().error(tr("无法创建插件：%1").arg(pluginName), "Flow");
                            delete newItem;
                            m_usedPluginNames.remove(instanceName);
                            dropEvent->acceptProposedAction();
                            return true;
                        }

                        if (!module->initialize()) {
                            Logger::instance().error(tr("插件初始化失败：%1").arg(pluginName), "Flow");
                            delete module;
                            delete newItem;
                            dropEvent->acceptProposedAction();
                            return true;
                        }

                        // 存储模块实例
                        m_flowModules.insert(instanceName, module);

                        // 更新 item 的数据
                        newItem->setData(0, Qt::UserRole, "flow_item");
                        newItem->setData(0, Qt::UserRole + 1, instanceName);
                        newItem->setData(0, Qt::UserRole + 2, pluginName);

                        Logger::instance().info(tr("已添加插件到流程：%1 (%2)").arg(displayName).arg(instanceName),
                                                "Flow");

                        m_modulesNeedSync = true;

                        // 更新项目修改状态
                        if (ProjectManager::instance().currentProject()) {
                            ProjectManager::instance().currentProject()->setModified(true);
                        }

                        dropEvent->acceptProposedAction();
                        return true;
                    }
                }
                // 非插件类型的拖拽，拒绝处理
                return true;
            }
            // Not a plugin drop, let default handling occur
            return QMainWindow::eventFilter(watched, event);
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    // 只接受文件拖入
    if (event->mimeData()->hasUrls()) {
        QList<QUrl> urls = event->mimeData()->urls();
        if (!urls.isEmpty()) {
            QString filePath = urls.first().toLocalFile();
            QFileInfo fileInfo(filePath);
            QString ext = fileInfo.suffix().toLower();
            QStringList supportedExts = {"png", "jpg", "jpeg", "bmp", "tif", "tiff", "ply"};
            if (supportedExts.contains(ext)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QMimeData* mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        QList<QUrl> urls = mimeData->urls();
        if (!urls.isEmpty()) {
            QString filePath = urls.first().toLocalFile();
            if (importImageFile(filePath)) {
                Logger::instance().info(tr("拖放导入图像：%1").arg(QFileInfo(filePath).fileName()), "System");
            }
        }
    }
    event->acceptProposedAction();
}

bool MainWindow::importImageFile(const QString& filePath) {
    if (filePath.isEmpty()) {
        return false;
    }

    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        Logger::instance().error(tr("文件不存在：%1").arg(filePath), "System");
        return false;
    }

    m_lastImportedImagePath = filePath;
    Logger::instance().info(tr("导入图像：%1").arg(filePath), "System");

    // 自动配置流程中第一个 GrabImage 模块的图像路径
    autoConfigureGrabImage(filePath);

    return true;
}

void MainWindow::autoConfigureGrabImage(const QString& filePath) {
    // 查找流程中第一个 GrabImage 模块
    for (auto it = m_flowModules.begin(); it != m_flowModules.end(); ++it) {
        IModule* module = it.value();
        if (module && module->moduleId().contains("GrabImage", Qt::CaseInsensitive)) {
            module->setParam("filePath", filePath);
            module->setParam("grabSource", "File");
            Logger::instance().info(tr("已自动配置 GrabImage 模块使用文件：%1").arg(filePath), "System");
            return;
        }
    }
}

void MainWindow::clearCentralDisplay() {
    if (m_displayManager) {
        m_displayManager->clearAll();
    }
}

void MainWindow::displayImage(const ImageData& image, const QString& label) {
    Q_UNUSED(label);
    DisplayData data(image);
    if (m_displayManager) {
        m_displayManager->displayData(data);
    }
}

void MainWindow::onNewSolution() {
    qDebug() << "onNewSolution called";
    Logger::instance().info(tr("新建方案"), "System");
    TerminalBridge::instance().onGuiAction("create-project", "new_solution");
    Project* proj = ProjectManager::instance().newProject();
    if (proj) {
        TerminalBridge::instance().onGuiAction("create-project-done", proj->name());
        if (m_projectLabel) {
            m_projectLabel->setText(tr("当前工程：%1").arg(proj->name()));
        }
        QMessageBox::information(this, tr("新建方案"), tr("方案「%1」已创建").arg(proj->name()));
    }
}

void MainWindow::onSolutionList() {
    // TODO: 打开最近工程列表对话框
    QStringList recent = ProjectManager::instance().recentProjects();
    if (recent.isEmpty()) {
        QMessageBox::information(this, tr("方案列表"), tr("没有最近的工程"));
    } else {
        QStringList items;
        for (const QString& path : recent) {
            items.append(QFileInfo(path).fileName() + " - " + path);
        }
        bool ok;
        QString selected = QInputDialog::getItem(this, tr("方案列表"), tr("选择工程:"), items, 0, false, &ok);
        if (ok && !selected.isEmpty()) {
            // 从路径中提取实际路径
            QString path = selected.mid(selected.lastIndexOf(" - ") + 3);
            ProjectManager::instance().openProject(path);
        }
    }
}

void MainWindow::onOpenProject() {
    QString filePath = QFileDialog::getOpenFileName(this, tr("打开工程"), QString(), tr("工程文件 (*.dproj)"));
    if (!filePath.isEmpty()) {
        TerminalBridge::instance().onGuiAction("open-project", filePath);
        if (ProjectManager::instance().openProject(filePath)) {
            if (m_projectLabel) {
                m_projectLabel->setText(tr("当前工程：%1").arg(QFileInfo(filePath).fileName()));
            }
        }
    }
}

void MainWindow::onSaveProject() {
    if (!ProjectManager::instance().hasProject()) {
        // 如果没有项目，弹出另存为对话框
        QString filePath = QFileDialog::getSaveFileName(this, tr("保存工程"), QString(), tr("工程文件 (*.dproj)"));
        if (!filePath.isEmpty()) {
            TerminalBridge::instance().onGuiAction("save-project", filePath);
            ProjectManager::instance().saveAsProject(filePath);
        }
    } else {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) {
            TerminalBridge::instance().onGuiAction("save-project", proj->filePath());
        }
        ProjectManager::instance().saveProject();
    }
}

void MainWindow::onQuickMode() {
    // 快速模式：直接运行当前流程
    executeFlowOnce();
}

void MainWindow::onRunOnce() {
    if (!m_isRunning) {
        m_isRunning = true;
        m_btnStartPause->setIcon(createPauseIcon());
        m_btnStartPause->setToolTip(tr("暂停"));
        m_btnStop->setEnabled(true);
        Logger::instance().info(tr("运行一次"), "System");
        executeFlowOnce();
    } else {
        onStop();
    }
}

void MainWindow::onRunCycle() {
    if (!m_isRunning) {
        m_isRunning = true;
        m_isCycleMode = true;
        m_btnStartPause->setIcon(createPauseIcon());
        m_btnStartPause->setToolTip(tr("暂停"));
        m_btnStop->setEnabled(true);
        Logger::instance().info(tr("循环运行"), "System");
        executeFlowOnce();
    }
    // 已经在运行时，不做操作，保持循环模式
}

void MainWindow::onStop() {
    m_isRunning = false;
    m_isCycleMode = false;
    m_btnStartPause->setIcon(createPlayIcon());
    m_btnStartPause->setToolTip(tr("单次运行"));
    m_btnStop->setEnabled(false);
    RunEngine::instance().stop();
    Logger::instance().info(tr("停止"), "System");
}

void MainWindow::executeFlowOnce() {
    // 清空之前的执行时间和所有高亮
    m_moduleExecutionTimes.clear();
    m_flowTotalTime = 0;
    m_flowInput = ImageData();
    m_currentExecutingIndex = 0;

    // 清除选中状态和所有项目的高亮
    m_processTree->clearSelection();
    m_processTree->setCurrentItem(nullptr);
    for (int i = 0; i < m_processTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_processTree->topLevelItem(i);
        item->setBackground(0, QBrush());
        item->setForeground(0, QBrush());
        item->setBackground(1, QBrush());
        item->setForeground(1, QBrush());
    }
    m_currentExecutingItem = nullptr;

    // 如果没有模块，直接返回
    if (m_processTree->topLevelItemCount() == 0) {
        if (m_processTimeLabel) {
            m_processTimeLabel->setText(tr("总耗时：0 ms"));
        }
        return;
    }

    RunEngine& engine = RunEngine::instance();

    // 仅在模块变更后重新同步（避免循环模式下每轮重复 addModule）
    if (m_modulesNeedSync) {
        m_instanceItemMap.clear();
        engine.clearModules();
        for (int i = 0; i < m_processTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = m_processTree->topLevelItem(i);
            QString instanceName = item->data(0, Qt::UserRole + 1).toString();
            if (instanceName.isEmpty() || !m_flowModules.contains(instanceName)) continue;
            IModule* im = m_flowModules.value(instanceName);
            if (!im || !im->isInitialized()) continue;
            ModuleBase* mb = qobject_cast<ModuleBase*>(im);
            if (mb) {
                mb->setInstanceName(instanceName);
                engine.addModule(mb);
                // 建立 instanceName → item 映射（用于信号处理 O(1) 查找）
                m_instanceItemMap[instanceName] = item;
            }
        }
        m_modulesNeedSync = false;
    }

    // 委托给 RunEngine 执行
    engine.runOnce();
}

void MainWindow::onUserLogin() {
    // 使用已实现的 LoginDialog
    LoginDialog dialog(this);
    dialog.setWindowTitle(tr("用户登录"));
    dialog.exec();
    Logger::instance().info(tr("用户登录"), "System");
}

void MainWindow::onGlobalVar() {
    // 使用已实现的 GlobalVarView
    GlobalVarView dialog(this);
    dialog.setWindowTitle(tr("全局变量"));
    dialog.exec();
    Logger::instance().info(tr("全局变量"), "System");
}

void MainWindow::onCameraSettings() {
    // 使用已实现的 CameraSetView
    CameraSetView dialog(this);
    dialog.setWindowTitle(tr("相机设置"));
    dialog.exec();
    Logger::instance().info(tr("相机设置"), "System");
}

void MainWindow::onCommSettings() {
    // 使用已实现的 CommunicationSetView
    CommunicationSetView dialog(this);
    dialog.setWindowTitle(tr("通信设置"));
    dialog.exec();
    Logger::instance().info(tr("通信设置"), "System");
}

void MainWindow::onHardwareConfig() {
    // 使用已实现的 SystemParamView
    SystemParamView dialog(this);
    dialog.setWindowTitle(tr("硬件配置"));
    dialog.exec();
    Logger::instance().info(tr("硬件配置"), "System");
}

void MainWindow::onReportQuery() {
    QMessageBox::information(this, tr("报表查询"), tr("报表查询功能开发中"));
    Logger::instance().info(tr("报表查询"), "System");
}

void MainWindow::onHome() {
    // TODO: 主页功能 - 可以显示欢迎界面或隐藏侧边面板
    Logger::instance().info(tr("主页"), "System");
}

void MainWindow::onUIDesign() {
    // 显示/激活 FlowCanvas（图形化节点编辑器）
    if (m_flowCanvas) {
        m_flowCanvas->show();
        m_flowCanvas->activateWindow();
        Logger::instance().info(tr("界面设计"), "System");
    } else {
        QMessageBox::information(this, tr("UI 设计"), tr("流程编辑器开发中"));
    }
}

void MainWindow::onLaserSet() {
    QMessageBox::information(this, tr("激光设置"), tr("激光设置功能开发中"));
    Logger::instance().info(tr("激光设置"), "System");
}

void MainWindow::onToggleTheme() {
    m_isDarkTheme = !m_isDarkTheme;
    applyTheme();
    Logger::instance().info(m_isDarkTheme ? tr("切换到深色主题") : tr("切换到浅色主题"), "System");
}

void MainWindow::onDeviceSettings() {
    // 使用已实现的 SystemParamView
    SystemParamView dialog(this);
    dialog.setWindowTitle(tr("设备设置"));
    dialog.exec();
    Logger::instance().info(tr("设备设置"), "System");
}

void MainWindow::onSystemSettings() {
    // 使用已实现的 SystemParamView
    SystemParamView dialog(this);
    dialog.setWindowTitle(tr("系统设置"));
    dialog.exec();
    Logger::instance().info(tr("系统设置"), "System");
}

void MainWindow::onCanvasSettings() {
    Logger::instance().info(tr("画布设置"), "System");
}

void MainWindow::onScreenshot() {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen) {
        QPixmap screenshot = screen->grabWindow(winId());
        QString fileName = QFileDialog::getSaveFileName(
            this, tr("保存截图"), QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png",
            tr("PNG 文件 (*.png)"));
        if (!fileName.isEmpty()) {
            screenshot.save(fileName);
            Logger::instance().info(tr("已保存截图：%1").arg(fileName), "System");
        }
    }
}

void MainWindow::onSaveLayout() {
    Logger::instance().info(tr("保存布局"), "System");
}

void MainWindow::onLoadLayout() {
    Logger::instance().info(tr("加载布局"), "System");
}

void MainWindow::onLicenseManager() {
    Logger::instance().info(tr("许可证管理"), "System");
}

void MainWindow::onHelp() {
    Logger::instance().info(tr("帮助"), "System");
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("关于 DeepLux"),
                       tr("<h2>DeepLux Vision 1.0.0</h2>"
                          "<p>工业视觉检测软件</p>"
                          "<p>&copy; 2024 DeepLux. All rights reserved.</p>"));
}

void MainWindow::onSchemeManagement() {
    Logger::instance().info(tr("方案管理"), "System");
}

void MainWindow::onLogAdded(const LogEntry& entry) {
    if (!m_logTable)
        return;

    // 根据过滤器检查是否应该显示
    int filterLevel = m_logFilterLevel;
    if (filterLevel > 0) {
        // filterLevel: 1=Debug, 2=Info, 3=Warning, 4=Error
        // 只显示 >= filterLevel 的日志
        int entryLevel = 0;
        switch (entry.level) {
        case LogLevel::Debug:
            entryLevel = 1;
            break;
        case LogLevel::Info:
            entryLevel = 2;
            break;
        case LogLevel::Warning:
            entryLevel = 3;
            break;
        case LogLevel::Error:
            entryLevel = 4;
            break;
        case LogLevel::Success:
            entryLevel = 2;
            break; // Success 按 Info 处理
        }
        if (entryLevel < filterLevel) {
            return; // 不显示
        }
    }

    int row = m_logTable->rowCount();
    m_logTable->insertRow(row);

    // 时间
    QTableWidgetItem* timeItem = new QTableWidgetItem(entry.timestamp.toString("hh:mm:ss.zzz"));
    timeItem->setForeground(QColor("#888888"));
    m_logTable->setItem(row, 0, timeItem);

    // 级别
    QString levelStr = Logger::levelToString(entry.level);
    QTableWidgetItem* levelItem = new QTableWidgetItem(levelStr);
    levelItem->setForeground(QColor(entry.level == LogLevel::Error     ? "#e94560"
                                    : entry.level == LogLevel::Warning ? "#f39c12"
                                    : entry.level == LogLevel::Success ? "#27ae60"
                                                                       : "#3498db"));
    m_logTable->setItem(row, 1, levelItem);

    // 消息
    QString fullMessage =
        entry.category.isEmpty() ? entry.message : QString("[%1] %2").arg(entry.category, entry.message);
    QTableWidgetItem* msgItem = new QTableWidgetItem(fullMessage);
    m_logTable->setItem(row, 2, msgItem);

    // 自动滚动到最后一行
    m_logTable->scrollToBottom();
}

void MainWindow::onLogFilterChanged(int index) {
    if (!m_logTable)
        return;

    m_logFilterLevel = index;

    // 表头文字保持不变（避免固定宽度下文字被遮挡）
    Q_UNUSED(index)

    // 遍历所有行，根据过滤器显示/隐藏
    for (int row = 0; row < m_logTable->rowCount(); ++row) {
        QTableWidgetItem* levelItem = m_logTable->item(row, 1);
        if (!levelItem)
            continue;

        QString levelStr = levelItem->text();
        int entryLevel = 0;
        if (levelStr == "DEBUG")
            entryLevel = 1;
        else if (levelStr == "INFO")
            entryLevel = 2;
        else if (levelStr == "WARN")
            entryLevel = 3;
        else if (levelStr == "ERROR")
            entryLevel = 4;
        else if (levelStr == "SUCCESS")
            entryLevel = 2;

        // filterLevel: 0=全部, 1=Debug, 2=Info, 3=Warning, 4=Error
        if (index == 0 || entryLevel >= index) {
            m_logTable->showRow(row);
        } else {
            m_logTable->hideRow(row);
        }
    }
}

void MainWindow::showLogLevelMenu()
{
    QMenu menu(this);
    QStringList items = QStringList() << tr("全部") << tr("Debug") << tr("Info") << tr("Warning") << tr("Error");
    for (int i = 0; i < items.size(); ++i) {
        QAction* action = menu.addAction(items[i]);
        action->setCheckable(true);
        action->setChecked(m_logFilterLevel == i);
        connect(action, &QAction::triggered, this, [this, i]() { onLogFilterChanged(i); });
    }

    // 在表头"级别"列下方弹出
    QHeaderView* header = m_logTable->horizontalHeader();
    QRect sectionRect = header->sectionViewportPosition(1) >= 0
                            ? QRect(header->sectionViewportPosition(1), header->height(),
                                    header->sectionSize(1), 0)
                            : QRect();
    QPoint pos = m_logTable->mapToGlobal(QPoint(sectionRect.x(), sectionRect.y()));
    menu.exec(pos);
}

void MainWindow::onBarcodeEntered() {
    QString barcode = m_barcodeInput->text();
    if (!barcode.isEmpty()) {
        Logger::instance().info(tr("扫码输入：%1").arg(barcode), "System");
        m_barcodeInput->clear();
    }
}

void MainWindow::onImportImage() {
    QString filePath = QFileDialog::getOpenFileName(this, tr("导入图像"), QString(),
                                                    tr("图像文件 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
    if (!filePath.isEmpty()) {
        if (importImageFile(filePath)) {
            Logger::instance().info(tr("导入图像：%1").arg(QFileInfo(filePath).fileName()), "System");
        }
    }
}

void MainWindow::syncModulesToRunEngine() {
    // 同步模块到运行引擎
}

// ========== 图标创建辅助方法 ==========

QIcon MainWindow::createIcon(const QString& name) {
    Q_UNUSED(name);
    return QIcon();
}

QIcon MainWindow::createNewIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 蓝色文档图标表示新建
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(66, 133, 244));

    // 文档形状
    QPainterPath docPath;
    docPath.moveTo(7, 3);
    docPath.lineTo(14, 3);
    docPath.lineTo(17, 6);
    docPath.lineTo(17, 19);
    docPath.lineTo(7, 19);
    docPath.closeSubpath();
    painter.drawPath(docPath);

    // 白色加号
    painter.setBrush(QColor(255, 255, 255));
    painter.drawRect(10, 10, 4, 1);
    painter.drawRect(11, 9, 1, 4);

    return QIcon(pixmap);
}

QIcon MainWindow::createOpenIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 黄色文件夹图标
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 179, 71));

    QPolygon folder;
    folder << QPoint(5, 7) << QPoint(10, 7) << QPoint(12, 9) << QPoint(18, 9) << QPoint(18, 18) << QPoint(5, 18);
    painter.drawPolygon(folder);

    return QIcon(pixmap);
}

QIcon MainWindow::createSaveIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 蓝色软盘图标
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(66, 133, 244));
    painter.drawRect(4, 4, 16, 16);

    // 白色标签区域
    painter.setBrush(QColor(255, 255, 255));
    painter.drawRect(6, 6, 12, 7);

    // 金属滑盖
    painter.setBrush(QColor(180, 180, 180));
    painter.drawRect(6, 14, 12, 4);

    return QIcon(pixmap);
}

QIcon MainWindow::createListIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 蓝色列表图标
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(66, 133, 244));

    // 列表项
    for (int i = 0; i < 4; i++) {
        painter.drawRect(5, 5 + i * 4, 14, 2);
    }

    return QIcon(pixmap);
}

QIcon MainWindow::createPlayIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 绿色播放三角形
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(52, 168, 83));

    QPolygon triangle;
    triangle << QPoint(8, 6) << QPoint(8, 18) << QPoint(17, 12);
    painter.drawPolygon(triangle);

    return QIcon(pixmap);
}

QIcon MainWindow::createPauseIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(251, 188, 5));

    painter.drawRect(6, 5, 5, 14);
    painter.drawRect(13, 5, 5, 14);

    return QIcon(pixmap);
}

QIcon MainWindow::createStopIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 红色八角形停止标志
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(234, 67, 53));

    // 八角形
    QPainterPath stopPath;
    stopPath.moveTo(7, 4);
    stopPath.lineTo(17, 4);
    stopPath.lineTo(20, 7);
    stopPath.lineTo(20, 17);
    stopPath.lineTo(17, 20);
    stopPath.lineTo(7, 20);
    stopPath.lineTo(4, 17);
    stopPath.lineTo(4, 7);
    stopPath.closeSubpath();
    painter.drawPath(stopPath);

    // 白色横杠
    painter.setBrush(QColor(255, 255, 255));
    QPainterPath barPath;
    barPath.addRoundedRect(7, 10, 10, 4, 1, 1);
    painter.drawPath(barPath);

    return QIcon(pixmap);
}

QIcon MainWindow::createCycleIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(QPen(QColor(26, 115, 232), 2));
    painter.setBrush(Qt::NoBrush);

    // 循环箭头
    painter.drawArc(4, 4, 16, 16, 0 * 16, 270 * 16);

    // 箭头头部
    painter.drawLine(20, 4, 20, 8);
    painter.drawLine(20, 4, 16, 4);

    return QIcon(pixmap);
}

QIcon MainWindow::createQuickModeIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 闪电图标
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 200, 0));

    QPolygon lightning;
    lightning << QPoint(10, 3) << QPoint(6, 10) << QPoint(9, 10) << QPoint(7, 17) << QPoint(14, 8) << QPoint(10, 8);
    painter.drawPolygon(lightning);

    return QIcon(pixmap);
}

QIcon MainWindow::createUserIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(60, 120, 200));

    // 头部
    painter.drawEllipse(7, 3, 6, 6);

    // 身体
    painter.drawEllipse(4, 10, 12, 7);

    return QIcon(pixmap);
}

QIcon MainWindow::createVariableIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(QPen(QColor(180, 100, 50), 2));
    painter.setBrush(QColor(220, 150, 80));

    // x 符号
    painter.drawLine(4, 4, 16, 16);
    painter.drawLine(16, 4, 4, 16);

    return QIcon(pixmap);
}

QIcon MainWindow::createCameraIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(80, 80, 80));

    // 相机主体
    painter.drawRoundedRect(3, 6, 14, 10, 2, 2);

    // 镜头
    painter.setBrush(QColor(40, 40, 40));
    painter.drawEllipse(7, 8, 6, 6);

    // 闪光灯
    painter.setBrush(QColor(100, 100, 100));
    painter.drawRect(14, 7, 2, 3);

    return QIcon(pixmap);
}

QIcon MainWindow::createCommIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(50, 150, 50));

    // 信号波形
    painter.drawEllipse(6, 6, 8, 8);
    painter.drawEllipse(8, 8, 4, 4);
    painter.setBrush(QColor(255, 255, 255));
    painter.drawEllipse(10, 10, 2, 2);

    return QIcon(pixmap);
}

QIcon MainWindow::createHardwareIcon() {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 灰色芯片主体
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(154, 160, 166));

    // 芯片主体圆角矩形
    QPainterPath chipPath;
    chipPath.addRoundedRect(7, 7, 10, 10, 2, 2);
    painter.drawPath(chipPath);

    // 引脚 - 左侧
    painter.setBrush(QColor(189, 195, 199));
    painter.drawRect(4, 8, 3, 2);
    painter.drawRect(4, 11, 3, 2);
    painter.drawRect(4, 14, 3, 2);

    // 引脚 - 右侧
    painter.drawRect(17, 8, 3, 2);
    painter.drawRect(17, 11, 3, 2);
    painter.drawRect(17, 14, 3, 2);

    return QIcon(pixmap);
}

QIcon MainWindow::createReportIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(Qt::NoPen);
    painter.setBrush(m_isDarkTheme ? QColor(200, 200, 200) : QColor(255, 255, 255));
    painter.drawRect(4, 3, 12, 14);

    QColor borderColor = m_isDarkTheme ? QColor(200, 200, 200) : QColor(40, 40, 40);
    painter.setPen(borderColor);
    painter.drawRect(4, 3, 12, 14);

    // 表格线
    painter.drawLine(6, 7, 14, 7);
    painter.drawLine(6, 11, 14, 11);
    painter.drawLine(9, 7, 9, 11);
    painter.drawLine(12, 7, 12, 11);

    return QIcon(pixmap);
}

QIcon MainWindow::createHomeIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(100, 150, 100));

    // 房子形状
    QPolygon house;
    house << QPoint(10, 3) << QPoint(17, 8) << QPoint(17, 16) << QPoint(13, 16) << QPoint(13, 11) << QPoint(7, 11)
          << QPoint(7, 16) << QPoint(3, 16) << QPoint(3, 8);
    painter.drawPolygon(house);

    return QIcon(pixmap);
}

QIcon MainWindow::createUiDesignIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(150, 100, 200));

    // 调色板形状
    painter.drawEllipse(4, 4, 12, 10);

    // 颜色孔
    painter.setBrush(QColor(255, 100, 100));
    painter.drawEllipse(7, 6, 2, 2);
    painter.setBrush(QColor(100, 255, 100));
    painter.drawEllipse(11, 6, 2, 2);
    painter.setBrush(QColor(100, 100, 255));
    painter.drawEllipse(9, 10, 2, 2);

    return QIcon(pixmap);
}

QIcon MainWindow::createLaserIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 0, 0));

    // 激光束
    painter.drawLine(3, 10, 17, 10);

    // 激光源
    painter.setBrush(QColor(100, 100, 100));
    painter.drawRect(2, 7, 3, 6);

    return QIcon(pixmap);
}

QIcon MainWindow::createToggleThemeIcon() {
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 一半太阳（浅色）一半月亮（深色）
    if (m_isDarkTheme) {
        // 深色主题时显示太阳图标
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 200, 0));
        painter.drawEllipse(5, 5, 10, 10);

        // 太阳光芒
        painter.setBrush(QColor(255, 180, 0));
        for (int i = 0; i < 8; i++) {
            double angle = i * 45 * 3.14159 / 180.0;
            int x = static_cast<int>(10 + 14 * cos(angle));
            int y = static_cast<int>(10 + 14 * sin(angle));
            painter.drawRect(x - 1, y - 1, 2, 2);
        }
    } else {
        // 浅色主题时显示月亮图标
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(100, 100, 150));
        painter.drawEllipse(6, 6, 10, 10);
        painter.setBrush(QColor(240, 240, 240));
        painter.drawEllipse(8, 8, 7, 7);
    }

    return QIcon(pixmap);
}

void MainWindow::loadAgentSettings()
{
    ConfigManager& cfg = ConfigManager::instance();
    if (!cfg.isInitialized()) return;

    AgentController& ctrl = AgentController::instance();

    // Load permission level
    int permLevel = cfg.groupInt("agent", "permissionLevel", 1);
    ctrl.setPermissionLevel(static_cast<AgentController::PermissionLevel>(permLevel));

    // Load and configure LLM client if API key is present
    QString apiKey = cfg.groupString("agent", "apiKey", "");
    if (!apiKey.isEmpty()) {
        OpenAILLMClient* client = new OpenAILLMClient(&ctrl);
        client->setEndpoint(cfg.groupString("agent", "endpoint", "https://api.openai.com/v1/chat/completions"));
        client->setApiKey(apiKey);
        client->setModel(cfg.groupString("agent", "model", "gpt-4o"));
        client->setTemperature(cfg.groupDouble("agent", "temperature", 0.3));
        client->setMaxTokens(cfg.groupInt("agent", "maxTokens", 4096));
        client->setToolsEnabled(cfg.groupBool("agent", "toolsEnabled", true));
        ctrl.setLLMClient(client);

        // 流式输出：LLM stream chunk → Chat Panel 实时显示
        connect(client, &ILLMClient::streamChunkReceived,
                m_agentChatPanel, &AgentChatPanel::streamAppend);
    }

    updateAgentPermissionDisplay();

    // Connect permission change to update display
    connect(&ctrl, &AgentController::permissionLevelChanged,
            this, &MainWindow::updateAgentPermissionDisplay);
}

void MainWindow::updateAgentPermissionDisplay()
{
    AgentController::PermissionLevel level = AgentController::instance().permissionLevel();
    QString text;
    QString color;
    switch (level) {
    case AgentController::PermissionLevel::Observer:
        text = "Agent: Observer";
        color = "#3498db";
        break;
    case AgentController::PermissionLevel::Advisor:
        text = "Agent: Advisor";
        color = "#f39c12";
        break;
    case AgentController::PermissionLevel::Autopilot:
        text = "Agent: Autopilot";
        color = "#e74c3c";
        break;
    }

    qDebug() << "Agent permission display:" << text;
}

} // namespace DeepLux
