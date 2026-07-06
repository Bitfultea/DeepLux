#include "MainWindow.h"

#include "../bridge/TerminalBridge.h"
#include "../dialogs/AgentSettingsDialog.h"
#include "../dialogs/LoginDialog.h"
#include "../display/DisplayManager.h"
#include "../panels/DataSourcePanel.h"
#include "../widgets/AgentActionLogWidget.h"
#include "../widgets/AgentChatPanel.h"
#include "../widgets/FlowCanvas.h"
#include "../widgets/TerminalWidget.h"
#include "../widgets/ViewportWidget.h"
#include "CameraSetView.h"
#include "CommunicationSetView.h"
#include "GlobalVarView.h"
#include "SplashScreen.h"
#include "SystemParamView.h"
#include "core/agent/AgentController.h"
#include "core/agent/OpenAILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/base/ModuleBase.h"
#include "core/common/ConfigWidgetHelper.h"
#include "core/common/Logger.h"
#include "core/common/ModuleIconProvider.h"
#include "core/display/DisplayData.h"
#include "core/display/IDisplayPort.h"
#include "core/engine/RunEngine.h"
#include "core/interface/IModule.h"
#include "core/io/PlyLoader.h"
#include "core/io/TiffLoader.h"
#include "core/manager/ConfigManager.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/DataSource.h"
#include "core/model/ImageData.h"
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
#include <QJsonValue>
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
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace DeepLux {

// 前向声明
class FlowCanvas;

namespace {
QString cleanToolDisplayName(const QString& displayName) {
    const QString text = displayName.trimmed();
    const int firstSpace = text.indexOf(' ');
    if (firstSpace > 0 && !text.at(0).isLetterOrNumber()) {
        return text.mid(firstSpace + 1).trimmed();
    }
    return text;
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), m_displayManager(new DisplayManager(this)) {

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
        } else {
            setUiRunningState(false, false);
        }
    });

    // Initialize AgentController (Phase 1)
    AgentController::instance().initialize();

    // Load Agent settings from ConfigManager (Phase 3)
    loadAgentSettings();

    // Connect ProjectManager signals to sync process tree with Project model
    connect(&ProjectManager::instance(), &ProjectManager::projectCreated, this, &MainWindow::onProjectOpened);
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, this, &MainWindow::onProjectOpened);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this, &MainWindow::onProjectClosed);

    // Connect Agent action log to UI (will be set after m_agentActionLogWidget is created)
}

MainWindow::~MainWindow() {
    TerminalBridge::instance().shutdown();
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

    // 3D 渲染模式（菜单项 + 横线分隔）
    viewMenu->addSeparator();
    QActionGroup* renderGroup = new QActionGroup(this);
    renderGroup->setExclusive(true);

    m_renderActions[0] = viewMenu->addAction(tr("统一色"));
    m_renderActions[1] = viewMenu->addAction(tr("RGB"));
    m_renderActions[2] = viewMenu->addAction(tr("高度"));
    m_renderActions[3] = viewMenu->addAction(tr("强度"));
    m_renderActions[4] = viewMenu->addAction(tr("法向"));
    m_renderActions[5] = viewMenu->addAction(tr("BlinnPhong"));
    for (int i = 0; i < 6; ++i) {
        m_renderActions[i]->setCheckable(true);
        m_renderActions[i]->setData(i);
        renderGroup->addAction(m_renderActions[i]);
    }
    m_renderActions[5]->setChecked(true);
    connect(renderGroup, &QActionGroup::triggered, this,
            [this](QAction* action) { onRenderModeChanged(action->data().toInt()); });

    // 工具菜单
    QMenu* toolMenu = menuBar()->addMenu(tr("工具 (&T)"));
    toolMenu->addAction(createCameraIcon(), tr("相机设置"), this, &MainWindow::onCameraSettings);
    toolMenu->addAction(createCommIcon(), tr("通讯设置"), this, &MainWindow::onCommSettings);
    toolMenu->addAction(createHardwareIcon(), tr("硬件配置"), this, &MainWindow::onHardwareConfig);
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

    // Debug 菜单
    QMenu* debugMenu = menuBar()->addMenu(tr("Debug"));
    debugMenu->addAction(tr("Test 3D Render"), this, &MainWindow::onTest3DRender);

    // 帮助菜单
    QMenu* helpMenu = menuBar()->addMenu(tr("帮助 (&H)"));
    helpMenu->addAction(tr("关于"), this, &MainWindow::onAbout);
}

void MainWindow::setupToolBar() {
    QToolBar* mainToolbar = addToolBar(tr("主工具栏"));
    mainToolbar->setObjectName("MainToolBar");
    mainToolbar->setMovable(false);
    mainToolbar->setIconSize(QSize(24, 24));
    mainToolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

    // 文件操作
    mainToolbar->addAction(createNewIcon(), tr("新建方案"), this, &MainWindow::onNewSolution);
    mainToolbar->addAction(createListIcon(), tr("方案列表"), this, &MainWindow::onSolutionList);
    mainToolbar->addAction(createOpenIcon(), tr("打开"), this, &MainWindow::onOpenProject);
    mainToolbar->addAction(createSaveIcon(), tr("保存"), this, &MainWindow::onSaveProject);
    mainToolbar->addSeparator();

    // 运行控制
    mainToolbar->addAction(createPlayIcon(), tr("单次运行"), this, &MainWindow::onRunOnce);
    mainToolbar->addAction(createCycleIcon(), tr("循环运行"), this, &MainWindow::onRunCycle);
    mainToolbar->addAction(createStopIcon(), tr("停止"), this, &MainWindow::onStop);
    mainToolbar->addSeparator();

    // 主题切换按钮
    mainToolbar->addAction(createToggleThemeIcon(), tr("切换主题"), this, &MainWindow::onToggleTheme);

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
}

void MainWindow::setupMainLayout() {
    // 创建主 Splitter - 水平分割左右
    QSplitter* mainSplitter = new QSplitter(Qt::Horizontal);
    mainSplitter->setObjectName("MainSplitter");
    mainSplitter->setHandleWidth(7);
    mainSplitter->setChildrenCollapsible(false);
    mainSplitter->setOpaqueResize(true);

    QWidget* mainContentWidget = new QWidget();
    mainContentWidget->setObjectName("MainContentWidget");
    QVBoxLayout* mainContentLayout = new QVBoxLayout(mainContentWidget);
    mainContentLayout->setContentsMargins(mainSplitter->handleWidth(), 0, 0, 0);
    mainContentLayout->setSpacing(0);

    // ========== 左侧：工具面板（贯穿整个高度）==========
    QWidget* toolPanelWidget = new QWidget();
    toolPanelWidget->setObjectName("ToolPanelWidget");
    QVBoxLayout* toolPanelLayout = new QVBoxLayout(toolPanelWidget);
    toolPanelLayout->setContentsMargins(0, 0, 0, 0);
    toolPanelLayout->setSpacing(0);

    // 工具分类区域（带滚动）
    QScrollArea* toolCategoryScroll = new QScrollArea();
    toolCategoryScroll->setWidgetResizable(true);
    toolCategoryScroll->setObjectName("ToolCategoryScroll");

    QWidget* toolCategoryWidget = new QWidget();
    QVBoxLayout* toolCategoryLayout = new QVBoxLayout(toolCategoryWidget);
    toolCategoryLayout->setContentsMargins(6, 6, 6, 6);
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

    connect(&PluginManager::instance(), &PluginManager::pluginLoaded, this, &MainWindow::updateToolBoxPluginItem);

    toolCategoryLayout->addWidget(m_toolBoxTree);
    toolCategoryScroll->setWidget(toolCategoryWidget);
    toolPanelLayout->addWidget(toolCategoryScroll);
    toolPanelLayout->setStretchFactor(toolCategoryScroll, 10);

    // 创建工具 DockWidget
    m_toolBoxDock = new QDockWidget(this);
    m_toolBoxDock->setObjectName("ToolPanelDock");
    m_toolBoxDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    m_toolBoxDock->setWidget(toolPanelWidget);
    m_toolBoxDock->setMinimumWidth(240);
    m_toolBoxDock->setMaximumWidth(340);
    // 自定义标题栏
    QWidget* toolTitleWidget = new QWidget();
    QHBoxLayout* toolTitleLayout = new QHBoxLayout(toolTitleWidget);
    toolTitleLayout->setContentsMargins(10, 0, 6, 0);
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
    rightSplitter->setHandleWidth(7);
    rightSplitter->setChildrenCollapsible(false);
    rightSplitter->setOpaqueResize(true);

    // 右上方区域：水平 Splitter（流程面板 + 显示区域）
    QSplitter* rightTopSplitter = new QSplitter(Qt::Horizontal);
    rightTopSplitter->setObjectName("RightTopSplitter");
    rightTopSplitter->setHandleWidth(7);
    rightTopSplitter->setChildrenCollapsible(false);
    rightTopSplitter->setOpaqueResize(true);

    // ========== 流程面板（QTabWidget：流程 + 画布）==========
    QWidget* processPanelWidget = new QWidget();
    processPanelWidget->setObjectName("ProcessPanelWidget");
    QVBoxLayout* processPanelLayout = new QVBoxLayout(processPanelWidget);
    processPanelLayout->setContentsMargins(0, 0, 0, 0);
    processPanelLayout->setSpacing(0);

    m_processTabWidget = new QTabWidget(this);
    m_processTabWidget->setObjectName("ProcessTabWidget");
    m_processTabWidget->setDocumentMode(true);
    m_processTabWidget->tabBar()->setUsesScrollButtons(false);
    m_processTabWidget->tabBar()->setExpanding(false);
    m_processTabWidget->tabBar()->setElideMode(Qt::ElideNone);

    // ---- Tab 1: 流程 ----
    m_processTabContent = new QWidget();
    m_processTabContent->setObjectName("ProcessTabContent");
    QVBoxLayout* flowLayout = new QVBoxLayout(m_processTabContent);
    flowLayout->setContentsMargins(6, 6, 6, 6);
    flowLayout->setSpacing(6);

    // 流程面板工具栏
    QWidget* processToolBar = new QWidget();
    QHBoxLayout* processToolBarLayout = new QHBoxLayout(processToolBar);
    processToolBarLayout->setContentsMargins(6, 6, 6, 6);
    processToolBarLayout->setSpacing(4);
    processToolBar->setObjectName("ProcessToolBar");
    processToolBar->setMinimumHeight(48);

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
    flowLayout->addWidget(processToolBar);

    // 模块树
    m_processTree = new QTreeWidget();
    m_processTree->setHeaderHidden(true);
    m_processTree->setAcceptDrops(true);
    m_processTree->setDragEnabled(true);
    m_processTree->setDragDropMode(QAbstractItemView::DragDrop);
    m_processTree->setAnimated(true);
    m_processTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_processTree->setDefaultDropAction(Qt::CopyAction);
    m_processTree->setDragDropOverwriteMode(false);
    m_processTree->setDropIndicatorShown(true);
    m_processTree->setObjectName("ProcessTree");
    m_processTree->setColumnCount(2);
    m_processTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_processTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_processTree->setColumnHidden(1, true); // 耗时列默认隐藏，运行后显示
    m_processTree->header()->setHidden(true);
    m_processTree->viewport()->installEventFilter(this);
    m_processTree->installEventFilter(this);
    connect(m_processTree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onProcessTreeContextMenu);

    // 提示标签
    m_hintLabel = new QLabel(tr("← 从左侧拖拽工具"));
    m_hintLabel->setAlignment(Qt::AlignCenter);
    m_hintLabel->setStyleSheet("color: #808080; padding: 10px;");
    m_hintLabel->setObjectName("ProcessTreeHintLabel");
    flowLayout->addWidget(m_hintLabel);

    flowLayout->addWidget(m_processTree);
    flowLayout->setStretchFactor(m_processTree, 1);

    // 流程状态栏
    m_processStatusWidget = new QWidget();
    QHBoxLayout* processStatusLayout = new QHBoxLayout(m_processStatusWidget);
    processStatusLayout->setContentsMargins(5, 5, 5, 5);
    m_processTimeLabel = new QLabel(tr("总耗时：0 ms"));
    processStatusLayout->addWidget(m_processTimeLabel);
    processStatusLayout->addStretch();
    flowLayout->addWidget(m_processStatusWidget);
    flowLayout->setStretchFactor(m_processStatusWidget, 0);

    m_processTabWidget->addTab(m_processTabContent, tr("流程"));

    // ---- Tab 2: 画布 ----
    m_flowCanvas = new FlowCanvas(this);
    m_flowCanvas->setObjectName("FlowCanvas");
    m_flowCanvas->setStyleSheet("background-color: #000000;");
    m_processTabWidget->addTab(m_flowCanvas, tr("画布"));

    // ---- Tab 3: 数据源 ----
    m_dataSourcePanel = new DataSourcePanel(this);
    m_dataSourcePanel->setObjectName("DataSourcePanel");
    m_processTabWidget->addTab(m_dataSourcePanel, tr("数据源"));

    // 连接 DataSourcePanel 信号
    connect(m_dataSourcePanel, &DataSourcePanel::requestDisplay, this, &MainWindow::onDisplayDataSource);
    connect(m_dataSourcePanel, &DataSourcePanel::requestRemove, this, &MainWindow::onRemoveDataSource);
    connect(m_dataSourcePanel, &DataSourcePanel::requestShowInFolder, this, &MainWindow::onShowDataSourceInFolder);
    connect(m_dataSourcePanel, &DataSourcePanel::requestCopyPath, this, &MainWindow::onCopyDataSourcePath);

    processPanelLayout->addWidget(m_processTabWidget);

    // 图像显示区域 - 使用 DisplayManager 的中央视口
    QWidget* imageDisplayWidget = m_displayManager->centralDisplay();
    imageDisplayWidget->setObjectName("ImageDisplayWidget");
    imageDisplayWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    imageDisplayWidget->setMinimumSize(0, 0);
    imageDisplayWidget->setAcceptDrops(true);     // 接收文件拖放
    imageDisplayWidget->installEventFilter(this); // route drops to eventFilter

    rightTopSplitter->addWidget(processPanelWidget);

    processPanelWidget->setMinimumWidth(220);
    processPanelWidget->setMaximumWidth(320);

    rightTopSplitter->addWidget(imageDisplayWidget);
    rightTopSplitter->setStretchFactor(0, 1);
    rightTopSplitter->setStretchFactor(1, 8);

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
    m_logTerminalTabs->tabBar()->setMinimumHeight(qMax(30, m_logTerminalTabs->tabBar()->fontMetrics().height() + 10));

    // 隐藏 dock 标题栏，节省空间
    m_logDock->setTitleBarWidget(new QWidget());

    // ===== Tab 1: 日志面板 =====
    QWidget* logWidget = new QWidget();
    QVBoxLayout* logLayout = new QVBoxLayout(logWidget);
    logLayout->setContentsMargins(6, 6, 6, 6);
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
    const int logTextHeight = m_logTable->fontMetrics().height();
    m_logTable->horizontalHeader()->setFixedHeight(qMax(30, logTextHeight + 10));
    m_logTable->verticalHeader()->setDefaultSectionSize(qMax(26, logTextHeight + 6));
    m_logTable->verticalHeader()->setMinimumWidth(40);
    m_logTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_logTable->setObjectName("LogTable");
    m_logTable->setFrameShape(QFrame::NoFrame);

    logLayout->addWidget(m_logTable);

    // 连接日志信号到表格
    connect(&Logger::instance(), &Logger::logAdded, this, &MainWindow::onLogAdded);

    // 点击表头"级别"列弹出筛选菜单
    connect(m_logTable->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int logicalIndex) {
        if (logicalIndex == 1)
            showLogLevelMenu();
    });

    int logTabIndex = m_logTerminalTabs->addTab(logWidget, tr("日志"));

    // Tab "日志" 再次点击时打开日志文件（第一次点击切换，第二次点击打开）
    connect(m_logTerminalTabs, &QTabWidget::tabBarClicked, this, [this, logTabIndex](int index) {
        if (index == logTabIndex && m_logTerminalTabs->currentIndex() == logTabIndex) {
            QString logPath = Logger::instance().logFilePath();
            QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
        }
    });

    // ===== Tab 2: 终端 =====
    m_terminalWidget = new TerminalWidget();
    TerminalBridge::instance().initialize(m_terminalWidget);
    m_logTerminalTabs->addTab(m_terminalWidget, tr("终端"));

    // ===== Tab 3: Agent Chat =====
    m_agentChatPanel = new AgentChatPanel();
    m_agentChatTabIndex = m_logTerminalTabs->addTab(m_agentChatPanel, tr("Agent 对话"));
    m_logTerminalTabs->setTabToolTip(m_agentChatTabIndex, tr("Agent 对话"));
    auto setAgentTabStatus = [this](const QString& tooltip, bool focus) {
        if (!m_logTerminalTabs || m_agentChatTabIndex < 0)
            return;
        m_logTerminalTabs->setTabToolTip(m_agentChatTabIndex, tooltip);
        if (focus) {
            m_logTerminalTabs->setCurrentIndex(m_agentChatTabIndex);
        }
    };
    connect(m_agentChatPanel, &AgentChatPanel::userMessageSent, this, [this, setAgentTabStatus](const QString& msg) {
        setAgentTabStatus(tr("Agent 正在思考"), false);
        m_agentChatPanel->setThinking(true);
        AgentController::instance().sendUserMessage(msg);
    });
    connect(m_agentChatPanel, &AgentChatPanel::userMessageWithImagesSent, this,
            [this, setAgentTabStatus](const QString& msg, const QList<QPixmap>& images) {
                setAgentTabStatus(tr("Agent 正在思考"), false);
                m_agentChatPanel->setThinking(true);
                AgentController::instance().sendUserMessageWithImages(msg, images);
            });
    connect(&AgentController::instance(), &AgentController::llmResponseReceived, this,
            [this, setAgentTabStatus](const QString& content, const QJsonArray& toolCalls) {
                Q_UNUSED(toolCalls);
                setAgentTabStatus(tr("Agent 对话"), false);
                m_agentChatPanel->setThinking(false);
                m_agentChatPanel->clearToolPreview();
                m_agentChatPanel->addMessage(AgentMessageBubble::Sender::Agent, content);
            });
    connect(&AgentController::instance(), &AgentController::llmErrorOccurred, this,
            [this, setAgentTabStatus](const QString& error) {
                setAgentTabStatus(tr("Agent 出错"), false);
                m_agentChatPanel->setThinking(false);
                m_agentChatPanel->clearToolPreview();
                m_agentChatPanel->addMessage(AgentMessageBubble::Sender::System, QString("Error: %1").arg(error));
                AgentActionLogEntry entry;
                entry.timestamp = QDateTime::currentDateTime();
                entry.actor = "System";
                entry.action = "LLM Error";
                entry.params = error;
                entry.result = "error";
                entry.undoable = false;
                m_agentActionLogWidget->addEntry(entry);
            });
    connect(&AgentController::instance(), &AgentController::toolsPendingConfirmation, this,
            [this, setAgentTabStatus](const QJsonArray& toolCalls) {
                setAgentTabStatus(tr("等待确认"), true);
                m_agentChatPanel->setThinking(false);
                QList<AgentToolPreviewCard::ToolItem> items;
                QStringList toolSummary;
                for (const QJsonValue& v : toolCalls) {
                    QJsonObject tc = v.toObject();
                    AgentToolPreviewCard::ToolItem item;
                    item.name = tc["name"].toString();
                    if (item.name.isEmpty()) {
                        QJsonObject func = tc["function"].toObject();
                        item.name = func["name"].toString();
                        QJsonValue argsVal = func["arguments"];
                        if (argsVal.isString()) {
                            item.params = QJsonDocument::fromJson(argsVal.toString().toUtf8()).object();
                        } else if (argsVal.isObject()) {
                            item.params = argsVal.toObject();
                        }
                    } else {
                        item.params = tc["arguments"].toObject();
                    }
                    const bool dangerous = ToolSchema::instance().findTool(item.name).dangerous;
                    toolSummary.append(QString("%1%2").arg(item.name, dangerous ? tr(" [高风险]") : QString()));
                    items.append(item);
                }
                if (!toolSummary.isEmpty()) {
                    m_agentChatPanel->addMessage(AgentMessageBubble::Sender::Tool,
                                                 tr("待确认工具：%1").arg(toolSummary.join(", ")));
                }
                m_agentChatPanel->showToolPreview(items);
            });
    connect(m_agentChatPanel, &AgentChatPanel::toolPreviewConfirmed, &AgentController::instance(),
            &AgentController::confirmPendingTools);
    connect(m_agentChatPanel, &AgentChatPanel::toolPreviewCancelled, &AgentController::instance(),
            &AgentController::rejectPendingTools);

    // ===== Tab 4: Agent Action Log =====
    m_agentActionLogWidget = new AgentActionLogWidget();
    m_logTerminalTabs->addTab(m_agentActionLogWidget, tr("Agent 日志"));
    connect(&AgentController::instance(), &AgentController::actionLogEntryAdded, m_agentActionLogWidget,
            &AgentActionLogWidget::addEntry);
    connect(m_agentActionLogWidget, &AgentActionLogWidget::undoRequested, this,
            [](int) { AgentController::instance().undoLastAgentAction(); });
    connect(&AgentController::instance(), &AgentController::agentLoopIterating, this, [this, setAgentTabStatus]() {
        setAgentTabStatus(tr("Agent 正在思考"), false);
        m_agentChatPanel->setThinking(true);
    });

    m_logDock->setWidget(m_logTerminalTabs);
    m_logDock->setMinimumHeight(220);

    rightSplitter->addWidget(m_logDock);
    rightSplitter->setStretchFactor(0, 7);
    rightSplitter->setStretchFactor(1, 3);
    rightSplitter->setSizes(QList<int>() << 620 << 260);

    // 将右侧 Splitter 添加到主 Splitter
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 4);

    // 设置中央 widget
    mainContentLayout->addWidget(mainSplitter);
    setCentralWidget(mainContentWidget);

    // 连接 DockWidget 关闭信号以同步菜单勾选状态
    connect(m_toolBoxDock, &QDockWidget::topLevelChanged, this,
            [this](bool) { m_viewToolPanelAction->setChecked(!m_toolBoxDock->isHidden()); });

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
    const QString text = cleanToolDisplayName(displayName);
    const PluginInfo info = PluginManager::instance().pluginInfo(pluginName);
    QTreeWidgetItem* item = new QTreeWidgetItem(parent, QStringList(text));
    item->setIcon(0, ModuleIconProvider::instance().iconFor(pluginName, info.category));
    item->setToolTip(0, pluginName);
    item->setData(0, Qt::UserRole, "plugin");
    item->setData(0, Qt::UserRole + 1, pluginName);
    m_toolDisplayNames.insert(pluginName, text);
}

QString MainWindow::toolDisplayName(const QString& pluginName, const QString& fallback) const {
    const QString cached = m_toolDisplayNames.value(pluginName).trimmed();
    if (!cached.isEmpty()) {
        return cached;
    }

    const QString managerName = PluginManager::instance().moduleDisplayName(pluginName).trimmed();
    if (!managerName.isEmpty()) {
        return managerName;
    }
    return fallback.isEmpty() ? pluginName : fallback;
}

void MainWindow::updateToolBoxPluginItem(const QString& pluginName) {
    if (!m_toolBoxTree) {
        return;
    }

    const QString displayName = PluginManager::instance().moduleDisplayName(pluginName);
    const PluginInfo info = PluginManager::instance().pluginInfo(pluginName);
    const QIcon icon = ModuleIconProvider::instance().iconFor(pluginName, info.category);

    for (QTreeWidgetItemIterator it(m_toolBoxTree); *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (item->data(0, Qt::UserRole).toString() == "plugin" &&
            item->data(0, Qt::UserRole + 1).toString() == pluginName) {
            item->setText(0, displayName);
            item->setIcon(0, icon);
            m_toolDisplayNames.insert(pluginName, displayName);
            return;
        }
    }
}

void MainWindow::onToggleToolPanel(bool checked) {
    if (m_toolBoxDock) {
        m_toolBoxDock->setVisible(checked);
    }
}

void MainWindow::onToggleProcessPanel(bool checked) {
    if (m_processTabWidget) {
        m_processTabWidget->setVisible(checked);
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
        removeFlowModuleByInstanceId(instanceName);
    }
}

void MainWindow::onProjectOpened(Project* project) {
    if (!project)
        return;

    // 清空现有流程树
    clearProcessTree();
    if (m_flowCanvas) {
        m_flowCanvas->loadFromProject(nullptr);
    }

    // 加载项目中已有的模块
    for (const ModuleInstance& inst : project->modules()) {
        addModuleToProcessTree(inst);
    }
    if (m_flowCanvas) {
        m_flowCanvas->loadFromProject(project);
    }

    // 刷新数据源面板
    if (m_dataSourcePanel) {
        m_dataSourcePanel->refreshFromProject(project);
    }
    if (m_projectLabel) {
        m_projectLabel->setText(tr("当前工程：%1").arg(project->name()));
    }

    // 连接 Project 信号
    disconnect(project, nullptr, this, nullptr);
    connect(project, &Project::moduleAdded, this, &MainWindow::onModuleAdded);
    connect(project, &Project::moduleRemoved, this, &MainWindow::onModuleRemoved);
    connect(project, &Project::connectionAdded, this, [this](const ModuleConnection& conn) {
        if (m_flowCanvas && m_flowCanvas->nodeItem(conn.fromModuleId) && m_flowCanvas->nodeItem(conn.toModuleId)) {
            m_flowCanvas->addConnection(conn.fromModuleId, conn.fromOutput, conn.toModuleId, conn.toInput);
        }
    });
    connect(project, &Project::connectionRemoved, this, [this](const QString& fromId, const QString& toId) {
        if (m_flowCanvas) {
            m_flowCanvas->removeConnection(fromId, toId);
        }
    });
    connect(project, &Project::dataSourceAdded, this, &MainWindow::onDataSourceAdded);
    connect(project, &Project::dataSourceRemoved, this, &MainWindow::onDataSourceRemoved);
}

void MainWindow::onProjectClosed() {
    clearProcessTree();
    if (m_flowCanvas) {
        m_flowCanvas->loadFromProject(nullptr);
    }
    if (m_dataSourcePanel) {
        m_dataSourcePanel->refreshFromProject(nullptr);
    }
    if (m_projectLabel) {
        m_projectLabel->setText(tr("当前工程：无"));
    }
}

void MainWindow::onModuleAdded(const ModuleInstance& module) {
    addModuleToProcessTree(module);
    if (m_flowCanvas && !m_flowCanvas->nodeItem(module.id)) {
        m_flowCanvas->addNode(module.moduleId, toolDisplayName(module.moduleId, module.name),
                              QPointF(module.posX, module.posY), module.id);
    }
}

void MainWindow::onModuleRemoved(const QString& instanceId) {
    removeModuleFromProcessTree(instanceId);
    if (m_flowCanvas && m_flowCanvas->nodeItem(instanceId)) {
        m_flowCanvas->removeNode(instanceId);
    }
}

void MainWindow::onDataSourceAdded(const DataSource& ds) {
    if (m_dataSourcePanel) {
        m_dataSourcePanel->addDataSource(ds);
    }
}

void MainWindow::onDataSourceRemoved(const QString& id) {
    if (m_dataSourcePanel) {
        m_dataSourcePanel->removeDataSource(id);
    }
}

void MainWindow::onDisplayDataSource(const QString& dataSourceId) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return;

    auto ds = project->findDataSource(dataSourceId);
    if (!ds || !ds->isValid())
        return;

    QFileInfo fileInfo(ds->filePath);
    if (!fileInfo.exists()) {
        Logger::instance().warning(tr("数据源文件已丢失：%1").arg(ds->filePath), "System");
        return;
    }

    if (ds->isPointCloud()) {
        importPointCloudFile(ds->filePath);
    } else if (ds->isImage()) {
        importImageFile(ds->filePath);
        // 图像导入后需要实际显示到视口
        QImage image(ds->filePath);
        if (!image.isNull() && m_displayManager) {
            DisplayData data;
            data.variant() = ImageData(image);
            data.setMetadata(QVariantMap{{"dataSourceId", ds->id}, {"dataSourceName", ds->name}});
            m_displayManager->displayData(data);
        }
    }
}

void MainWindow::onRemoveDataSource(const QString& dataSourceId) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return;
    project->removeDataSource(dataSourceId);
}

void MainWindow::onShowDataSourceInFolder(const QString& dataSourceId) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return;
    auto ds = project->findDataSource(dataSourceId);
    if (!ds || ds->filePath.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(ds->filePath).absolutePath()));
}

void MainWindow::onCopyDataSourcePath(const QString& dataSourceId) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return;
    auto ds = project->findDataSource(dataSourceId);
    if (!ds || ds->filePath.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(ds->filePath);
}

void MainWindow::addModuleToProcessTree(const ModuleInstance& inst) {
    if (m_instanceItemMap.contains(inst.id))
        return; // 已存在，防止重复

    // 隐藏提示标签
    if (m_hintLabel) {
        m_hintLabel->setVisible(false);
        m_hintLabel->deleteLater();
        m_hintLabel = nullptr;
    }

    // 创建树节点
    QString displayName = toolDisplayName(inst.moduleId, inst.name);
    QTreeWidgetItem* newItem = new QTreeWidgetItem();
    newItem->setFlags((newItem->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    newItem->setText(0, displayName);
    m_processTree->addTopLevelItem(newItem);

    // 更新 used names
    m_usedPluginNames.insert(inst.id);

    // 创建插件运行时实例（clone 可能返回 null — 取决于插件是否实现了 cloneImpl）
    DeepLux::PluginManager& pm = DeepLux::PluginManager::instance();
    IModule* module = pm.createModule(inst.moduleId);
    if (module) {
        newItem->setIcon(0, module->icon());
        if (!inst.params.isEmpty()) {
            module->setParams(inst.params);
        }
        if (module->initialize()) {
            m_flowModules.insert(inst.id, module);
            if (!module->icon().isNull()) {
                newItem->setIcon(0, module->icon());
            }
        } else {
            delete module;
            module = nullptr;
            Logger::instance().warning(tr("模块初始化失败：%1").arg(inst.moduleId), "Flow");
        }
    } else {
        Logger::instance().warning(tr("模块不支持克隆，无法创建运行时实例：%1").arg(inst.moduleId), "Flow");
    }

    // 更新 item 数据
    newItem->setData(0, Qt::UserRole, "flow_item");
    newItem->setData(0, Qt::UserRole + 1, inst.id);
    newItem->setData(0, Qt::UserRole + 2, inst.moduleId);

    m_instanceItemMap.insert(inst.id, newItem);
    m_modulesNeedSync = true;
}

void MainWindow::removeFlowModuleByInstanceId(const QString& instanceId) {
    if (instanceId.isEmpty()) {
        return;
    }

    Project* project = ProjectManager::instance().currentProject();
    if (project && project->findModule(instanceId)) {
        project->removeModule(instanceId);
        return;
    }

    removeModuleFromProcessTree(instanceId);
}

void MainWindow::removeModuleFromProcessTree(const QString& instanceId) {
    QTreeWidgetItem* item = m_instanceItemMap.value(instanceId);
    if (item) {
        int idx = m_processTree->indexOfTopLevelItem(item);
        if (idx >= 0) {
            m_processTree->takeTopLevelItem(idx);
        }
        delete item;
        m_instanceItemMap.remove(instanceId);
    }

    if (m_flowModules.contains(instanceId)) {
        IModule* module = m_flowModules.take(instanceId);
        if (module) {
            module->shutdown();
            delete module;
        }
    }

    m_usedPluginNames.remove(instanceId);
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

void MainWindow::clearProcessTree() {
    for (IModule* module : m_flowModules) {
        if (module) {
            module->shutdown();
            delete module;
        }
    }
    m_flowModules.clear();
    m_usedPluginNames.clear();
    m_instanceItemMap.clear();
    m_processTree->clear();

    if (!m_hintLabel) {
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
    m_modulesNeedSync = true;
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
            "QWidget#MainContentWidget { background-color: #1e1e1e; }"
            "QSplitter#MainSplitter { background-color: #1e1e1e; }"
            "QDockWidget { background-color: #252525; color: #ffffff; border: none; }"
            "QDockWidget::title { "
            "    background-color: #2d2d2d; "
            "    color: #ffffff; "
            "    font-weight: bold; "
            "    font-size: 13px; "
            "    padding: 8px 10px; "
            "    border-bottom: 1px solid #444444; }"
            "QDockWidget::title:hover { background-color: #333333; }"
            "QTreeWidget { background-color: #252525; color: #ffffff; border: none; font-size: 13px; }"
            "QTreeWidget::item { height: 28px; padding-left: 2px; padding-right: 2px; }"
            "QTreeWidget::item:hover { background-color: #3a3a3a; }"
            "QTreeWidget::item:selected { background-color: #0078d7; }"
            "QTableWidget { background-color: #252525; color: #ffffff; border: none; font-size: 13px; }"
            "QTableWidget::item { border-bottom: 1px solid #333; }"
            "QTableWidget::item:selected { background-color: #0078d7; }"
            "QHeaderView::section { background-color: #333333; padding: 5px; border: none; font-size: 13px; }"
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
            "QToolBar QToolButton { background-color: transparent; color: #ffffff; padding: 5px; border: 1px solid "
            "transparent; }"
            "QToolBar QToolButton:hover { background-color: #3a3a3a; border: 1px solid #555555; }"
            "QToolBar QToolButton:pressed { background-color: #444444; border: 1px solid #6b7280; }"
            "QMenuBar { background-color: #252525; color: #ffffff; }"
            "QMenuBar::item:selected { background-color: #3a3a3a; }"
            "QMenu { background-color: #252525; color: #ffffff; border: 1px solid #333; }"
            "QMenu::item:selected { background-color: #0078d7; }"
            "QStatusBar { background-color: #252525; color: #ffffff; font-size: 13px; }"
            "QStatusBar QLabel { padding: 0 8px; }"
            "QPushButton { background-color: #0078d7; color: white; padding: 5px 15px; border: none; }"
            "QPushButton:hover { background-color: #1e8ad6; }"
            "QPushButton:disabled { background-color: #555555; }"
            "QLineEdit { background-color: #333333; color: white; border: 1px solid #555; padding: 5px; }"
            "QComboBox { background-color: #333333; color: white; border: 1px solid #555; padding: 5px; }"
            "QSpinBox { background-color: #333333; color: white; border: 1px solid #555; padding: 5px; }"
            "QSplitter#MainSplitter::handle { background-color: #1e1e1e; border: none; }"
            "QSplitter#RightTopSplitter::handle:horizontal { "
            "    background-color: #30363d; border-left: 1px solid #3f4750; border-right: 1px solid #252b31; }"
            "QSplitter#RightSplitter::handle:vertical { "
            "    background-color: #30363d; border-top: 1px solid #3f4750; border-bottom: 1px solid #252b31; }"
            "QWidget#ProcessPanelWidget { background-color: #252525; border-right: 1px solid #3b4148; }"
            "QWidget#ImageDisplayWidget { background-color: #1e1e1e; border-left: 1px solid #3b4148; "
            "border-bottom: 1px solid #3b4148; }"
            "QDockWidget#LogDock { border-top: 1px solid #3b4148; }"
            "QLabel { color: #ffffff; }"
            "QScrollArea { background-color: #252525; }"
            "QFrame { background-color: #444444; }"
            "QTabWidget::pane { border: none; background-color: #252525; }"
            "QTabBar { background-color: #252525; }"
            "QTabBar::tab { background-color: #333333; color: #ffffff; font-size: 13px; font-weight: 500; "
            "min-height: 26px; padding: 3px 10px; border: none; }"
            "QTabBar::tab:selected { background-color: #444444; }"
            "QTabBar::tab:hover:!selected { background-color: #3a3a3a; }");
    } else {
        // 白色主题 - 适合亮光环境
        setStyleSheet(
            "QMainWindow { background-color: #f5f5f5; color: #212121; }"
            "QWidget#MainContentWidget { background-color: #f5f5f5; }"
            "QSplitter#MainSplitter { background-color: #f5f5f5; }"
            "QDockWidget { background-color: #ffffff; color: #212121; border: none; }"
            "QDockWidget::title { "
            "    background-color: #e8e8e8; "
            "    color: #212121; "
            "    font-weight: bold; "
            "    font-size: 13px; "
            "    padding: 8px 10px; "
            "    border-bottom: 1px solid #cccccc; }"
            "QDockWidget::title:hover { background-color: #d0d0d0; }"
            "QTreeWidget { background-color: #ffffff; color: #212121; border: 1px solid #dddddd; font-size: 13px; }"
            "QTreeWidget::item { height: 28px; padding-left: 2px; padding-right: 2px; }"
            "QTreeWidget::item:hover { background-color: #e5f3ff; }"
            "QTreeWidget::item:selected { background-color: #0078d7; color: #ffffff; }"
            "QTableWidget { background-color: #ffffff; color: #212121; border: 1px solid #dddddd; font-size: 13px; }"
            "QTableWidget::item { border-bottom: 1px solid #eeeeee; }"
            "QTableWidget::item:selected { background-color: #0078d7; color: #ffffff; }"
            "QHeaderView::section { background-color: #f0f0f0; color: #212121; padding: 5px; border: 1px solid "
            "#dddddd; font-size: 13px; }"
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
            "QStatusBar { background-color: #f8f8f8; color: #212121; font-size: 13px; }"
            "QStatusBar QLabel { padding: 0 8px; }"
            "QPushButton { background-color: #0078d7; color: white; padding: 5px 15px; border: 1px solid #005a9e; }"
            "QPushButton:hover { background-color: #1e8ad6; }"
            "QPushButton:disabled { background-color: #cccccc; color: #999999; }"
            "QLineEdit { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; padding: 5px; }"
            "QLineEdit:focus { border: 1px solid #0078d7; }"
            "QComboBox { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; padding: 5px; }"
            "QComboBox:focus { border: 1px solid #0078d7; }"
            "QSpinBox { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; padding: 5px; }"
            "QSpinBox:focus { border: 1px solid #0078d7; }"
            "QSplitter#MainSplitter::handle { background-color: #f5f5f5; border: none; }"
            "QSplitter#RightTopSplitter::handle:horizontal { "
            "    background-color: #e4e8ed; border-left: 1px solid #d2d8e0; border-right: 1px solid #f7f8fa; }"
            "QSplitter#RightSplitter::handle:vertical { "
            "    background-color: #e4e8ed; border-top: 1px solid #d2d8e0; border-bottom: 1px solid #f7f8fa; }"
            "QWidget#ProcessPanelWidget { background-color: #ffffff; border-right: 1px solid #dce2e8; }"
            "QWidget#ImageDisplayWidget { background-color: #ffffff; border-left: 1px solid #dce2e8; "
            "border-bottom: 1px solid #dce2e8; }"
            "QDockWidget#LogDock { border-top: 1px solid #dce2e8; }"
            "QLabel { color: #212121; }"
            "QScrollArea { background-color: #ffffff; }"
            "QFrame { background-color: #dddddd; }"
            "QTabWidget::pane { border: none; background-color: #ffffff; }"
            "QTabBar { background-color: #ffffff; }"
            "QTabBar::tab { background-color: #e8e8e8; color: #212121; font-size: 13px; font-weight: 500; "
            "min-height: 26px; padding: 3px 10px; border: none; }"
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
    QString panelBorderColor = m_isDarkTheme ? "#3b4148" : "#dce2e8";

    if (m_toolBoxDock && m_toolBoxDock->titleBarWidget()) {
        m_toolBoxDock->titleBarWidget()->setStyleSheet(
            QString("background-color: %1; border: none; border-bottom: 1px solid %2;").arg(bgColor, borderColor));
        QLabel* label = m_toolBoxDock->titleBarWidget()->findChild<QLabel*>();
        if (label)
            label->setStyleSheet(QString("QLabel { color: %1; font-weight: 600; font-size: 13px; }").arg(textColor));
        QToolButton* btn = m_toolBoxDock->titleBarWidget()->findChild<QToolButton*>();
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
            label->setStyleSheet(QString("QLabel { color: %1; font-weight: 600; font-size: 13px; }").arg(textColor));
    }

    // 更新 TreeWidget 样式
    if (m_toolBoxTree) {
        m_toolBoxTree->setStyleSheet(
            QString("QTreeWidget { background-color: %1; color: %2; border: none; font-size: 13px; }"
                    "QTreeWidget::item { height: 28px; padding-left: 2px; padding-right: 2px; }"
                    "QTreeWidget::item:hover { background-color: %3; }"
                    "QTreeWidget::item:selected { background-color: #0078d7; color: #ffffff; }")
                .arg(treeBgColor, treeTextColor, treeHoverColor));
    }
    if (m_processTree) {
        m_processTree->setStyleSheet(
            QString("QTreeWidget { background-color: %1; color: %2; border: none; font-size: 13px; }"
                    "QTreeWidget::item { height: 28px; padding-left: 2px; padding-right: 2px; }"
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

    // 更新流程状态栏样式
    if (m_processStatusWidget) {
        m_processStatusWidget->setStyleSheet(QString("background-color: %1;").arg(bgColor));
        if (m_processTimeLabel) {
            m_processTimeLabel->setStyleSheet(QString("QLabel { color: %1; }").arg(textColor));
        }
    }

    // 更新流程工具栏按钮样式
    QString btnBgColor = m_isDarkTheme ? "#3a3a3a" : "#e0e0e0";
    QString btnTextColor = m_isDarkTheme ? "#ffffff" : "#212121";
    QString btnHoverColor = m_isDarkTheme ? "#4a4a4a" : "#d0d0d0";
    QString toolBtnBorderColor = m_isDarkTheme ? "#555555" : "#cccccc";
    const QString processToolButtonStyle =
        QString("QToolButton { background-color: %1; color: %2; border: 1px solid %3; border-radius: 3px; "
                "padding: 4px 6px; }"
                "QToolButton:hover { background-color: %4; }"
                "QToolButton:disabled { background-color: %1; color: #999999; }")
            .arg(btnBgColor, btnTextColor, toolBtnBorderColor, btnHoverColor);
    QToolButton* startPauseBtn = findChild<QToolButton*>("ProcessStartPauseBtn");
    if (startPauseBtn) {
        startPauseBtn->setStyleSheet(processToolButtonStyle);
    }
    QToolButton* stopBtn = findChild<QToolButton*>("ProcessStopBtn");
    if (stopBtn) {
        stopBtn->setStyleSheet(processToolButtonStyle);
    }
    QToolButton* cycleBtn = findChild<QToolButton*>("ProcessCycleBtn");
    if (cycleBtn) {
        cycleBtn->setStyleSheet(processToolButtonStyle);
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
        processPanelWidget->setStyleSheet(
            QString("background-color: %1; border-right: 1px solid %2;").arg(scrollBgColor, panelBorderColor));
    }
    QWidget* imageDisplayWidget = findChild<QWidget*>("ImageDisplayWidget");
    if (imageDisplayWidget) {
        imageDisplayWidget->setStyleSheet(QString("background-color: %1; border-left: 1px solid %2; "
                                                  "border-bottom: 1px solid %2;")
                                              .arg(m_isDarkTheme ? "#1e1e1e" : "#ffffff", panelBorderColor));
    }
    if (m_logDock) {
        m_logDock->setStyleSheet(QString("QDockWidget#LogDock { border-top: 1px solid %1; }").arg(panelBorderColor));
    }
    if (m_logTable) {
        const QString logTableBg = m_isDarkTheme ? "#252525" : "#ffffff";
        const QString logHeaderBg = m_isDarkTheme ? "#333333" : "#f0f0f0";
        const QString logTextColor = m_isDarkTheme ? "#ffffff" : "#212121";
        const QString logLineColor = m_isDarkTheme ? "#3b4148" : "#dce2e8";
        m_logTable->setFrameShape(QFrame::NoFrame);
        m_logTable->setStyleSheet(
            QString("QTableWidget#LogTable { background-color: %1; color: %2; border: none; outline: none; "
                    "gridline-color: %3; font-size: 13px; }"
                    "QTableWidget#LogTable::item { border-bottom: 1px solid %3; }"
                    "QTableWidget#LogTable::item:selected { background-color: #0078d7; color: #ffffff; }"
                    "QHeaderView::section { background-color: %4; color: %2; padding: 5px; border: none; "
                    "border-bottom: 1px solid %3; font-size: 13px; }"
                    "QTableCornerButton::section { background-color: %4; border: none; }")
                .arg(logTableBg, logTextColor, logLineColor, logHeaderBg));
    }

    if (m_processTabWidget) {
        m_processTabWidget->setStyleSheet(
            QString("QTabWidget::pane { border: none; border-top: 2px solid %1; background-color: %2; }"
                    "QTabBar::tab { background-color: transparent; color: %3; font-size: 13px; font-weight: 500;"
                    "  min-height: 26px; padding: 3px 10px; border: none; border-bottom: 2px solid transparent;"
                    "  margin-right: 2px; }"
                    "QTabBar::tab:selected { color: #0078d7; border-bottom: 2px solid #0078d7; }"
                    "QTabBar::tab:hover:!selected { color: %3; background-color: %1; }")
                .arg(m_isDarkTheme ? "#3a3a3a" : "#e0e0e0", m_isDarkTheme ? "#252525" : "#ffffff",
                     m_isDarkTheme ? "#a0a0a0" : "#666666"));
    }
    if (m_logTerminalTabs) {
        m_logTerminalTabs->setStyleSheet(
            QString("QTabWidget::pane { border: none; background-color: %1; }"
                    "QTabBar::tab { background-color: %2; color: %3; font-size: 13px; font-weight: 500;"
                    "  min-height: 26px; padding: 3px 10px; border: none; margin-right: 1px; }"
                    "QTabBar::tab:selected { background-color: %1; color: %4; }"
                    "QTabBar::tab:hover:!selected { background-color: %5; }")
                .arg(m_isDarkTheme ? "#252525" : "#ffffff", m_isDarkTheme ? "#333333" : "#e8e8e8",
                     m_isDarkTheme ? "#d1d5db" : "#4b5563", m_isDarkTheme ? "#ffffff" : "#212121",
                     m_isDarkTheme ? "#3a3a3a" : "#dce2e8"));
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
        m_agentActionLogWidget->applyTheme(m_isDarkTheme);
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
            qDebug() << "[DIAG-UI] Double-click module:" << instanceName;
            if (!instanceName.isEmpty() && m_flowModules.contains(instanceName)) {
                IModule* module = m_flowModules.value(instanceName);
                qDebug() << "[DIAG-UI] Creating config widget for" << instanceName << "moduleId=" << module->moduleId();
                QWidget* configWidget = module->createConfigWidget();
                qDebug() << "[DIAG-UI] createConfigWidget returned" << configWidget;
                if (configWidget) {
                    QDialog* dialog = new QDialog(this);
                    dialog->setWindowTitle(tr("配置 - %1").arg(item->text(0)));
                    dialog->setMinimumSize(400, 300);
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    QVBoxLayout* layout = new QVBoxLayout(dialog);
                    layout->addWidget(configWidget); // addWidget 会 reparent 到 dialog
                    QHBoxLayout* btnLayout = new QHBoxLayout();
                    QPushButton* okBtn = new QPushButton(tr("确定"));
                    QPushButton* cancelBtn = new QPushButton(tr("取消"));
                    btnLayout->addStretch();
                    btnLayout->addWidget(okBtn);
                    btnLayout->addWidget(cancelBtn);
                    layout->addLayout(btnLayout);

                    // dialog 关闭时把 configWidget 从 dialog 分离，防止被 WA_DeleteOnClose 销毁
                    // 插件可能缓存了 configWidget 指针，销毁会导致下次 createConfigWidget crash
                    connect(dialog, &QDialog::finished, configWidget,
                            [configWidget]() { configWidget->setParent(nullptr); });

                    connect(okBtn, &QPushButton::clicked, dialog, &QDialog::accept);
                    connect(cancelBtn, &QPushButton::clicked, dialog, &QDialog::reject);

                    if (dialog->exec() == QDialog::Accepted) {
                        Logger::instance().info(tr("模块参数已更新：%1").arg(item->text(0)), "Config");
                    }
                } else {
                    Logger::instance().warning(tr("该模块没有配置选项：%1").arg(item->text(0)), "Config");
                }
            }
            return true;
        }
    }

    // 图像显示区域的拖放处理
    QWidget* imgWidget = m_displayManager ? m_displayManager->centralDisplay() : nullptr;
    if (imgWidget && watched == imgWidget) {
        if (event->type() == QEvent::DragEnter) {
            QDragEnterEvent* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                QList<QUrl> urls = de->mimeData()->urls();
                if (!urls.isEmpty()) {
                    QString ext = QFileInfo(urls.first().toLocalFile()).suffix().toLower();
                    QStringList supported = {"png", "jpg", "jpeg", "bmp", "tif", "tiff", "ply"};
                    if (supported.contains(ext)) {
                        de->acceptProposedAction();
                        return true;
                    }
                }
            }
        }
        if (event->type() == QEvent::Drop) {
            QDropEvent* de = static_cast<QDropEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                QList<QUrl> urls = de->mimeData()->urls();
                if (!urls.isEmpty()) {
                    QString filePath = urls.first().toLocalFile();
                    importFile(filePath);
                    de->acceptProposedAction();
                    return true;
                }
            }
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
                removeFlowModuleByInstanceId(instanceName);
            }
            return true;
        }
    }

    // 处理拖放到流程树
    if (m_processTree) {
        QWidget* processViewport = m_processTree->viewport();
        if (processViewport && watched == processViewport &&
            (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
             event->type() == QEvent::Drop)) {
            QDropEvent* dropEvent = static_cast<QDropEvent*>(event);

            // 获取拖放的数据
            const QMimeData* mimeData = dropEvent->mimeData();
            if (!mimeData) {
                return QMainWindow::eventFilter(watched, event);
            }

            auto insertRowForDrop = [this, dropEvent]() {
                QTreeWidgetItem* hoverItem = m_processTree->itemAt(dropEvent->pos());
                if (!hoverItem) {
                    return m_processTree->topLevelItemCount();
                }

                const QRect itemRect = m_processTree->visualItemRect(hoverItem);
                const int itemMiddle = itemRect.top() + itemRect.height() / 2;
                const int hoverRow = m_processTree->indexOfTopLevelItem(hoverItem);
                return dropEvent->pos().y() < itemMiddle ? hoverRow : hoverRow + 1;
            };

            const bool isToolBoxDrop =
                dropEvent->source() == m_toolBoxTree || dropEvent->source() == m_toolBoxTree->viewport();
            const bool isProcessTreeDrop =
                dropEvent->source() == m_processTree || dropEvent->source() == m_processTree->viewport();
            if (!isToolBoxDrop && !isProcessTreeDrop) {
                dropEvent->ignore();
                return true;
            }

            if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
                if (mimeData->hasFormat("application/x-qabstractitemmodeldatalist")) {
                    dropEvent->setDropAction(isProcessTreeDrop ? Qt::MoveAction : Qt::CopyAction);
                    dropEvent->accept();
                    return true;
                }
                dropEvent->ignore();
                return true;
            }

            if (isProcessTreeDrop && mimeData->hasFormat("application/x-qabstractitemmodeldatalist")) {
                QTreeWidgetItem* sourceItem = m_processTree->currentItem();
                if (!sourceItem || sourceItem->data(0, Qt::UserRole).toString() != "flow_item") {
                    dropEvent->ignore();
                    return true;
                }

                const int sourceRow = m_processTree->indexOfTopLevelItem(sourceItem);
                int insertRow = insertRowForDrop();
                if (sourceRow < 0 || insertRow == sourceRow || insertRow == sourceRow + 1) {
                    dropEvent->setDropAction(Qt::MoveAction);
                    dropEvent->accept();
                    return true;
                }

                QTreeWidgetItem* movedItem = m_processTree->takeTopLevelItem(sourceRow);
                if (insertRow > sourceRow) {
                    --insertRow;
                }
                insertRow = qMax(0, qMin(insertRow, m_processTree->topLevelItemCount()));
                m_processTree->insertTopLevelItem(insertRow, movedItem);
                m_processTree->setCurrentItem(movedItem);

                const QString instanceId = movedItem->data(0, Qt::UserRole + 1).toString();
                if (Project* project = ProjectManager::instance().currentProject()) {
                    project->moveModule(instanceId, insertRow);
                }
                m_modulesNeedSync = true;

                dropEvent->setDropAction(Qt::MoveAction);
                dropEvent->accept();
                return true;
            }

            // 检查是否有来自工具箱的拖放
            if (isToolBoxDrop && mimeData->hasFormat("application/x-qabstractitemmodeldatalist")) {
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

                        const int insertRow = insertRowForDrop();

                        // 先添加树节点（即时视觉反馈），再异步创建模块实例
                        QTreeWidgetItem* newItem = new QTreeWidgetItem();
                        newItem->setFlags((newItem->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
                        m_processTree->insertTopLevelItem(insertRow, newItem);

                        QString instanceName = pluginName;
                        int counter = 1;
                        while (m_usedPluginNames.contains(instanceName)) {
                            instanceName = QString("%1_%2").arg(pluginName).arg(counter++);
                        }
                        m_usedPluginNames.insert(instanceName);
                        newItem->setData(0, Qt::UserRole, "flow_item");
                        newItem->setData(0, Qt::UserRole + 1, instanceName);
                        newItem->setData(0, Qt::UserRole + 2, pluginName);
                        m_instanceItemMap.insert(instanceName, newItem); // 防 Project 信号重复创建

                        // 推迟模块创建到事件循环 — 避免在拖放嵌套循环中阻塞
                        QTimer::singleShot(0, this, [this, pluginName, instanceName, newItem]() {
                            DeepLux::PluginManager& pm = DeepLux::PluginManager::instance();
                            IModule* module = pm.createModule(pluginName);
                            if (!module) {
                                Logger::instance().error(tr("无法创建插件：%1").arg(pluginName), "Flow");
                                removeModuleFromProcessTree(instanceName);
                                return;
                            }
                            if (!module->initialize()) {
                                Logger::instance().error(tr("插件初始化失败：%1").arg(pluginName), "Flow");
                                delete module;
                                removeModuleFromProcessTree(instanceName);
                                return;
                            }
                            // 通过 Project 模型添加 — moduleAdded 信号同步 processTree + FlowCanvas
                            const QString displayName = toolDisplayName(pluginName, module->name());
                            newItem->setText(0, displayName);
                            newItem->setIcon(0, module->icon());
                            m_flowModules.insert(instanceName, module);
                            m_modulesNeedSync = true;
                            ModuleInstance inst;
                            inst.id = instanceName;
                            inst.moduleId = pluginName;
                            inst.name = displayName;
                            Project* proj = ProjectManager::instance().currentProject();
                            if (proj) {
                                proj->addModule(inst);
                                proj->moveModule(instanceName, m_processTree->indexOfTopLevelItem(newItem));
                            }
                            Logger::instance().info(tr("已添加插件到流程：%1 (%2)").arg(displayName).arg(instanceName),
                                                    "Flow");
                        });

                        dropEvent->setDropAction(Qt::CopyAction);
                        dropEvent->accept();
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
            if (importFile(filePath)) {
                Logger::instance().info(tr("拖放导入文件：%1").arg(QFileInfo(filePath).fileName()), "System");
            }
        }
    }
    event->acceptProposedAction();
}

// TIFF 大文件阈值与跳采样步长（避免 UI 卡顿）
static constexpr qint64 LARGE_TIFF_THRESHOLD = 100 * 1024 * 1024; // 100MB
static constexpr int TIFF_STEP_LARGE = 2;
static constexpr int TIFF_STEP_NORMAL = 1;

bool MainWindow::importFile(const QString& filePath) {
    if (filePath.isEmpty())
        return false;

    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        Logger::instance().error(tr("文件不存在：%1").arg(filePath), "System");
        return false;
    }

    if (!ProjectManager::instance().hasProject()) {
        ProjectManager::instance().newProject();
        Logger::instance().info(tr("自动创建项目"), "System");
    }

    m_lastImportedImagePath = filePath;
    QString ext = fileInfo.suffix().toLower();
    Logger::instance().info(QString("[importFile] %1 ext=%2").arg(fileInfo.fileName()).arg(ext), "3D");

    if (ext == "ply" || ext == "tif" || ext == "tiff") {
        Logger::instance().info("[importFile] → importPointCloudFile", "3D");
        return importPointCloudFile(filePath);
    }
    Logger::instance().info("[importFile] → importImageFile", "3D");
    return importImageFile(filePath);
}

bool MainWindow::importPointCloudFile(const QString& filePath) {
    QFileInfo fileInfo(filePath);
    QString ext = fileInfo.suffix().toLower();
    Logger::instance().info(
        QString("[imp3D] file=%1 size=%2 ext=%3").arg(fileInfo.fileName()).arg(fileInfo.size()).arg(ext), "3D");
    PointCloudData pc;
    QString error;
    bool ok = false;

    if (ext == "ply") {
        ok = PlyLoader::load(filePath, pc, error);
    } else if (ext == "tif" || ext == "tiff") {
        TiffLoader::Config cfg;
        cfg.step = (fileInfo.size() > LARGE_TIFF_THRESHOLD) ? TIFF_STEP_LARGE : TIFF_STEP_NORMAL;
        ok = TiffLoader::load(filePath, pc, error, cfg);
    } else {
        Logger::instance().error(tr("不支持的点云格式：%1").arg(ext), "System");
        return false;
    }

    if (!ok) {
        QString reason = error.isEmpty() ? tr("未知错误") : error;
        Logger::instance().error(tr("3D 文件加载失败：%1 (%2)").arg(fileInfo.fileName()).arg(reason), "3D");
        return false;
    }

    // 创建 DataSource 并注册到 Project
    DataSource ds;
    ds.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ds.name = fileInfo.fileName();
    ds.filePath = fileInfo.absoluteFilePath();
    ds.type = "pointcloud";
    ds.metadata["pointCount"] = static_cast<qint64>(pc.size());
    ds.metadata["fileSize"] = fileInfo.size();
    ds.importTime = QDateTime::currentMSecsSinceEpoch();

    Project* project = ProjectManager::instance().currentProject();
    if (project) {
        project->addDataSource(ds);
    }

    Logger::instance().info(QString("[imp3D] loaded %1 points, sending to DisplayManager").arg(pc.size()), "3D");

    size_t pointCount = pc.size();
    updateRenderModeComboForData(pc);

    DisplayData data;
    data.variant() = std::move(pc);
    data.setTimestamp(QDateTime::currentMSecsSinceEpoch());
    data.setMetadata(QVariantMap{{"dataSourceId", ds.id}, {"dataSourceName", ds.name}});
    if (m_displayManager) {
        m_displayManager->displayData(data);
    }
    Logger::instance().info(tr("导入 3D 点云：%1 (%2 点)").arg(fileInfo.fileName()).arg(pointCount), "System");

    if (m_processTabWidget && m_dataSourcePanel) {
        m_processTabWidget->setCurrentWidget(m_dataSourcePanel);
    }

    return true;
}

bool MainWindow::importImageFile(const QString& filePath) {
    Logger::instance().info(tr("导入图像：%1").arg(filePath), "System");

    QFileInfo fileInfo(filePath);
    DataSource ds;
    ds.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ds.name = fileInfo.fileName();
    ds.filePath = fileInfo.absoluteFilePath();
    ds.type = "image";
    ds.metadata["fileSize"] = fileInfo.size();
    ds.importTime = QDateTime::currentMSecsSinceEpoch();

    Project* project = ProjectManager::instance().currentProject();
    if (project) {
        project->addDataSource(ds);
    }

    autoConfigureGrabImage(filePath);

    QImage image(filePath);
    if (!image.isNull() && m_displayManager) {
        DisplayData data;
        data.variant() = ImageData(image);
        data.setMetadata(QVariantMap{{"dataSourceId", ds.id}, {"dataSourceName", ds.name}});
        m_displayManager->displayData(data);
    }

    if (m_processTabWidget && m_dataSourcePanel) {
        m_processTabWidget->setCurrentWidget(m_dataSourcePanel);
    }

    return true;
}

void MainWindow::onRenderModeChanged(int index) {
    auto viewports = m_displayManager->allViewports();
    for (auto* vp : viewports) {
        if (vp)
            vp->setRenderMode(index);
    }
}

void MainWindow::updateRenderModeCombo() {
    // 菜单项不需要预填充（已在 setupMenuBar 中创建）
}

void MainWindow::updateRenderModeComboForData(const PointCloudData& pc) {
    m_renderActions[1]->setEnabled(pc.hasColors());
    m_renderActions[3]->setEnabled(!pc.intensities.empty() && pc.intensities.size() == pc.points.size());
    m_renderActions[4]->setEnabled(pc.hasNormals());
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
    QStringList recent = ProjectManager::instance().recentProjects();
    if (recent.isEmpty()) {
        QMessageBox::information(this, tr("方案列表"), tr("没有最近的工程"));
        return;
    }

    QStringList items;
    for (const QString& path : recent) {
        items.append(QFileInfo(path).fileName() + " - " + path);
    }

    bool ok = false;
    const QString selected = QInputDialog::getItem(this, tr("方案列表"), tr("选择工程:"), items, 0, false, &ok);
    if (!ok || selected.isEmpty()) {
        return;
    }

    const int selectedIndex = items.indexOf(selected);
    if (selectedIndex < 0 || selectedIndex >= recent.size()) {
        return;
    }

    const QString path = recent.at(selectedIndex);
    TerminalBridge::instance().onGuiAction("open-project", path);
    Project* project = ProjectManager::instance().openProject(path);
    if (!project) {
        QMessageBox::warning(this, tr("方案列表"), tr("无法打开工程：%1").arg(path));
        Logger::instance().error(tr("无法打开最近工程：%1").arg(path), "System");
        return;
    }

    TerminalBridge::instance().onGuiAction("open-project-done", path);
    if (m_projectLabel) {
        const QString projectName = project->name().isEmpty() ? QFileInfo(path).fileName() : project->name();
        m_projectLabel->setText(tr("当前工程：%1").arg(projectName));
    }
    Logger::instance().info(tr("打开最近工程：%1").arg(path), "System");
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
        QMessageBox::warning(this, tr("保存工程"), tr("没有打开的工程"));
        return;
    }

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj) {
        return;
    }

    QString filePath = proj->filePath();
    if (filePath.isEmpty()) {
        filePath = QFileDialog::getSaveFileName(this, tr("保存工程"), QString(), tr("工程文件 (*.dproj)"));
        if (filePath.isEmpty()) {
            return;
        }
        TerminalBridge::instance().onGuiAction("save-project", filePath);
        ProjectManager::instance().saveAsProject(filePath);
    } else {
        TerminalBridge::instance().onGuiAction("save-project", filePath);
        ProjectManager::instance().saveProject();
    }
}

void MainWindow::onQuickMode() {
    // 快速模式：直接运行当前流程
    executeFlowOnce();
}

void MainWindow::setUiRunningState(bool running, bool cycleMode) {
    m_isRunning = running;
    m_isCycleMode = running && cycleMode;

    if (m_btnStartPause) {
        m_btnStartPause->setIcon(running ? createPauseIcon() : createPlayIcon());
        m_btnStartPause->setToolTip(running ? tr("暂停") : tr("单次运行"));
    }
    if (m_btnStop) {
        m_btnStop->setEnabled(running);
    }
}

void MainWindow::onRunOnce() {
    if (!m_isRunning) {
        if (m_processTree->topLevelItemCount() == 0) {
            if (m_processTimeLabel) {
                m_processTimeLabel->setText(tr("总耗时：0 ms"));
            }
            Logger::instance().warning(tr("流程为空，无法运行"), "System");
            return;
        }
        setUiRunningState(true, false);
        Logger::instance().info(tr("运行一次"), "System");
        executeFlowOnce();
    } else {
        onStop();
    }
}

void MainWindow::onRunCycle() {
    if (!m_isRunning) {
        if (m_processTree->topLevelItemCount() == 0) {
            if (m_processTimeLabel) {
                m_processTimeLabel->setText(tr("总耗时：0 ms"));
            }
            Logger::instance().warning(tr("流程为空，无法循环运行"), "System");
            return;
        }
        setUiRunningState(true, true);
        Logger::instance().info(tr("循环运行"), "System");
        executeFlowOnce();
    }
    // 已经在运行时，不做操作，保持循环模式
}

void MainWindow::onStop() {
    setUiRunningState(false, false);
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
            if (instanceName.isEmpty() || !m_flowModules.contains(instanceName))
                continue;
            IModule* im = m_flowModules.value(instanceName);
            if (!im || !im->isInitialized())
                continue;
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

void MainWindow::onHome() {
    clearCentralDisplay();
    if (m_processTabWidget && m_flowCanvas) {
        m_processTabWidget->setCurrentWidget(m_flowCanvas);
    }
    if (m_processTree) {
        m_processTree->clearSelection();
    }
    if (m_processTimeLabel) {
        m_processTimeLabel->setText(tr("总耗时：0 ms"));
    }
    Logger::instance().info(tr("主页"), "System");
}

void MainWindow::onUIDesign() {
    // 显示/激活 FlowCanvas（图形化节点编辑器）
    if (m_flowCanvas) {
        if (m_processTabWidget) {
            m_processTabWidget->setCurrentWidget(m_flowCanvas);
        }
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

void MainWindow::onTest3DRender() {
    // Create a synthetic point cloud (sphere + cube)
    PointCloudData pc;
    const int N = 50;
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            double theta = i * 2.0 * M_PI / N;
            double phi = j * M_PI / N;
            double r = 2.0;
            pc.points.push_back(Eigen::Vector3d(r * sin(phi) * cos(theta), r * sin(phi) * sin(theta), r * cos(phi)));
            pc.colors.push_back(Eigen::Vector3d(sin(theta), cos(phi), 0.5));
            pc.normals.push_back(Eigen::Vector3d(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi)));
        }
    }

    Logger::instance().info(tr("Test 3D: %1 points").arg(pc.size()), "Debug");
    qDebug() << "[Test3D] Creating DisplayData with" << pc.size() << "points";

    DisplayData data;
    data.variant() = std::move(pc);
    data.setTimestamp(QDateTime::currentMSecsSinceEpoch());

    qDebug() << "[Test3D] pointCloudData ptr:" << data.pointCloudData() << "isValid:" << data.isValid();

    if (m_displayManager) {
        qDebug() << "[Test3D] Calling displayManager->displayData, viewports=" << m_displayManager->viewportCount();
        m_displayManager->displayData(data);
        qDebug() << "[Test3D] displayData returned";
    } else {
        qDebug() << "[Test3D] m_displayManager is NULL!";
    }
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

void MainWindow::showLogLevelMenu() {
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
                            ? QRect(header->sectionViewportPosition(1), header->height(), header->sectionSize(1), 0)
                            : QRect();
    QPoint pos = m_logTable->mapToGlobal(QPoint(sectionRect.x(), sectionRect.y()));
    menu.exec(pos);
}

void MainWindow::onImportImage() {
    QString filePath = QFileDialog::getOpenFileName(this, tr("导入图像"), QString(),
                                                    tr("图像文件 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
    if (!filePath.isEmpty()) {
        if (importFile(filePath)) {
            Logger::instance().info(tr("导入文件：%1").arg(QFileInfo(filePath).fileName()), "System");
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

void MainWindow::loadAgentSettings() {
    ConfigManager& cfg = ConfigManager::instance();
    if (!cfg.isInitialized())
        return;

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
        client->setReasoningEffort(cfg.groupString("agent", "reasoningEffort", ""));
        client->setThinkingEnabled(cfg.groupBool("agent", "thinkingEnabled", true));
        ctrl.setLLMClient(client);
    }

    updateAgentPermissionDisplay();

    // Connect permission change to update display
    connect(&ctrl, &AgentController::permissionLevelChanged, this, &MainWindow::updateAgentPermissionDisplay);
}

void MainWindow::updateAgentPermissionDisplay() {
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
