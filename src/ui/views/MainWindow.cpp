#include "MainWindow.h"

#include "../ThemeManager.h"
#include "../controllers/ProcessTreeController.h"

#include "../bridge/TerminalBridge.h"
#include "../dialogs/AgentSettingsDialog.h"
#include "../dialogs/LoginDialog.h"
#include "../display/3d/Viewport3DContent.h"
#include "../display/DisplayManager.h"
#include "../panels/DataSourcePanel.h"
#include "../widgets/AgentActionLogWidget.h"
#include "../widgets/AgentChatPanel.h"
#include "../widgets/AppIconProvider.h"
#include "../widgets/FlowCanvas.h"
#include "../widgets/HImageWidget.h"
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
#include <QTextEdit>
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

QString measurementInputModeForConsumer(const QString& moduleId) {
    if (moduleId == QStringLiteral("com.deeplux.plugin.distancepp") ||
        moduleId == QStringLiteral("com.deeplux.plugin.measuregap")) {
        return QStringLiteral("point_pair");
    }
    if (moduleId == QStringLiteral("com.deeplux.plugin.distancepl")) {
        return QStringLiteral("point_line");
    }
    if (moduleId == QStringLiteral("com.deeplux.plugin.linesdistance")) {
        return QStringLiteral("line_pair");
    }
    if (moduleId == QStringLiteral("com.deeplux.plugin.pointsurfacedistance")) {
        return QStringLiteral("point_plane");
    }
    return QString();
}

QString measurementInputModeText(const QString& mode) {
    if (mode == QStringLiteral("point_pair")) {
        return QStringLiteral("点1、点2");
    }
    if (mode == QStringLiteral("point_line")) {
        return QStringLiteral("点、线段两端点");
    }
    if (mode == QStringLiteral("line_pair")) {
        return QStringLiteral("线1两端点、线2两端点");
    }
    if (mode == QStringLiteral("point_plane")) {
        return QStringLiteral("点、平面三点");
    }
    return mode;
}

QString measurementSummary(const ImageData& output) {
    QStringList parts;
    const QMap<QString, QVariant> all = output.allData();

    auto add = [&](const QString& key, const QString& label) {
        if (all.contains(key)) {
            double v = all[key].toDouble();
            parts.append(QString("%1=%2").arg(label).arg(v, 0, 'f', 3));
        }
    };

    add("distance", QString::fromUtf8("距离"));
    add("gap_distance", QString::fromUtf8("间隙距离"));
    add("gap_delta_z", QString::fromUtf8("ΔZ"));
    add("line_length", QString::fromUtf8("线段长度"));
    add("rect_width", QString::fromUtf8("宽度"));
    add("rect_height", QString::fromUtf8("高度"));
    add("surface_roughness", QString::fromUtf8("粗糙度"));

    if (all.contains("point_count")) {
        parts.append(QString::fromUtf8("点数=%1").arg(all["point_count"].toInt()));
    }

    return parts.isEmpty() ? QString() : QString::fromUtf8("测量: ") + parts.join(QString::fromUtf8(", "));
}

QJsonArray pointArray2D(const QPointF& point) {
    QJsonArray arr;
    arr.append(point.x());
    arr.append(point.y());
    return arr;
}

QJsonArray pointArray3D(double x, double y, double z) {
    QJsonArray arr;
    arr.append(x);
    arr.append(y);
    arr.append(z);
    return arr;
}

QString pointText2D(const QPointF& point) {
    return QString("%1,%2").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2);
}

QString pointText3D(const QVector3D& point) {
    return QString("%1,%2,%3").arg(point.x(), 0, 'f', 3).arg(point.y(), 0, 'f', 3).arg(point.z(), 0, 'f', 3);
}

QPointF pointFromArray2D(const QJsonArray& arr, int offset = 0) {
    return QPointF(arr.at(offset).toDouble(), arr.at(offset + 1).toDouble());
}

QVector3D pointFromArray3D(const QJsonArray& arr, int offset = 0) {
    const double z = arr.size() > offset + 2 ? arr.at(offset + 2).toDouble() : 0.0;
    return QVector3D(static_cast<float>(arr.at(offset).toDouble()), static_cast<float>(arr.at(offset + 1).toDouble()),
                     static_cast<float>(z));
}

QVector3D linePointFromArray3D(const QJsonArray& arr, int offset = 0) {
    return QVector3D(static_cast<float>(arr.at(offset).toDouble()), static_cast<float>(arr.at(offset + 1).toDouble()),
                     0.0f);
}

QJsonArray updatedLineArray(const QJsonArray& existing, const QPointF& point, bool firstPoint) {
    QJsonArray line = existing;
    while (line.size() < 4) {
        line.append(0.0);
    }

    const int offset = firstPoint ? 0 : 2;
    line[offset] = point.x();
    line[offset + 1] = point.y();
    return line;
}

QJsonArray updatedPlaneArray(const QJsonArray& existing, const QJsonArray& point, int planePointIndex) {
    QJsonArray plane = existing;
    while (plane.size() < 9) {
        plane.append(0.0);
    }

    const int offset = qBound(0, planePointIndex, 2) * 3;
    for (int i = 0; i < 3; ++i) {
        plane[offset + i] = point[i];
    }
    return plane;
}

QString pluginConfigDialogStyle(bool isDark) {
    const QString bg = isDark ? QStringLiteral("#1e1e1e") : QStringLiteral("#f5f5f5");
    const QString surface = isDark ? QStringLiteral("#252525") : QStringLiteral("#ffffff");
    const QString border = isDark ? QStringLiteral("#3b4148") : QStringLiteral("#dce2e8");
    const QString text = isDark ? QStringLiteral("#ffffff") : QStringLiteral("#212121");
    const QString secondary = isDark ? QStringLiteral("#3a3a3a") : QStringLiteral("#f3f4f6");
    const QString secondaryHover = isDark ? QStringLiteral("#4a4a4a") : QStringLiteral("#e5e7eb");

    return QStringLiteral(
               "QDialog#PluginConfigDialog { background-color: %1; color: %4; }"
               "QDialog#PluginConfigDialog QScrollArea#PluginConfigScrollArea { background-color: %1; border: none; }"
               "QDialog#PluginConfigDialog QScrollArea#PluginConfigScrollArea QWidget { background-color: %2; }"
               "QDialog#PluginConfigDialog QPushButton#PluginConfigOkButton { background-color: #0078d7; color: "
               "#ffffff; "
               "border: 1px solid #005a9e; border-radius: 4px; padding: 5px 14px; min-height: 28px; }"
               "QDialog#PluginConfigDialog QPushButton#PluginConfigOkButton:hover { background-color: #1e8ad6; }"
               "QDialog#PluginConfigDialog QPushButton#PluginConfigCancelButton { background-color: %5; color: %4; "
               "border: 1px solid %3; border-radius: 4px; padding: 5px 14px; min-height: 28px; }"
               "QDialog#PluginConfigDialog QPushButton#PluginConfigCancelButton:hover { background-color: %6; }")
        .arg(bg, surface, border, text, secondary, secondaryHover);
}

void applyPluginConfigTheme(QWidget* root, bool isDark) {
    if (!root) {
        return;
    }

    const QString surface = isDark ? QStringLiteral("#252525") : QStringLiteral("#ffffff");
    const QString input = isDark ? QStringLiteral("#333333") : QStringLiteral("#ffffff");
    const QString border = isDark ? QStringLiteral("#555555") : QStringLiteral("#cccccc");
    const QString text = isDark ? QStringLiteral("#ffffff") : QStringLiteral("#212121");
    const QString accent = QStringLiteral("#0078d7");
    const QString hover = isDark ? QStringLiteral("#1e8ad6") : QStringLiteral("#1e8ad6");
    const QString spinButton = isDark ? QStringLiteral("#444444") : QStringLiteral("#e5e7eb");
    const QString spinHover = isDark ? QStringLiteral("#555555") : QStringLiteral("#d1d5db");

    const QString containerStyle = QStringLiteral("background-color: %1; color: %2;").arg(surface, text);
    const QString labelStyle = QStringLiteral("color: %1; background-color: transparent; border: none;").arg(text);
    const QString inputStyle =
        QStringLiteral("QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QTextEdit { background-color: %1; color: %2; "
                       "border: 1px solid %3; border-radius: 4px; padding: 5px 8px; min-height: 26px; }"
                       "QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QTextEdit:focus { "
                       "border: 1px solid %4; }"
                       "QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, "
                       "QDoubleSpinBox::down-button { background-color: %5; border-radius: 2px; }"
                       "QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, "
                       "QDoubleSpinBox::down-button:hover { background-color: %6; }"
                       "QComboBox::drop-down { border: none; width: 20px; }"
                       "QComboBox::down-arrow { image: none; border-left: 5px solid transparent; "
                       "border-right: 5px solid transparent; border-top: 5px solid %2; }")
            .arg(input, text, border, accent, spinButton, spinHover);
    const QString buttonStyle =
        QStringLiteral("QPushButton { background-color: %1; color: #ffffff; border: 1px solid #005a9e; "
                       "border-radius: 4px; padding: 5px 14px; min-height: 28px; }"
                       "QPushButton:hover { background-color: %2; }"
                       "QPushButton:disabled { background-color: %3; color: %4; border-color: %3; }")
            .arg(accent, hover, spinButton, isDark ? QStringLiteral("#9ca3af") : QStringLiteral("#777777"));
    const QString optionStyle =
        QStringLiteral("QCheckBox { color: %1; background-color: transparent; spacing: 6px; }").arg(text);
    const QString groupStyle =
        QStringLiteral("QGroupBox { background-color: %1; color: %2; border: 1px solid %3; border-radius: 6px; "
                       "font-weight: bold; margin-top: 12px; padding-top: 12px; }"
                       "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; "
                       "background-color: %1; }")
            .arg(surface, text, border);

    QList<QWidget*> widgets{root};
    widgets.append(root->findChildren<QWidget*>());
    for (QWidget* widget : widgets) {
        if (qobject_cast<QLineEdit*>(widget) || qobject_cast<QSpinBox*>(widget) ||
            qobject_cast<QDoubleSpinBox*>(widget) || qobject_cast<QComboBox*>(widget) ||
            qobject_cast<QTextEdit*>(widget)) {
            widget->setStyleSheet(inputStyle);
        } else if (qobject_cast<QPushButton*>(widget)) {
            widget->setStyleSheet(buttonStyle);
        } else if (qobject_cast<QLabel*>(widget)) {
            widget->setStyleSheet(labelStyle);
        } else if (qobject_cast<QCheckBox*>(widget)) {
            widget->setStyleSheet(optionStyle);
        } else if (qobject_cast<QGroupBox*>(widget)) {
            widget->setStyleSheet(groupStyle);
        } else {
            widget->setStyleSheet(containerStyle);
        }
    }
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), m_displayManager(new DisplayManager(this)) {

    setupUi();
    applyTheme();

    // RunEngine 信号连接 — 统一执行入口，MainWindow 只做 UI 高亮/状态更新
    // 使用 instanceName → item 映射实现 O(1) 查找
    connect(&RunEngine::instance(), &RunEngine::moduleStarted, this, [this](const QString& moduleName) {
        QTreeWidgetItem* item = m_processTreeController ? m_processTreeController->instanceItem(moduleName) : nullptr;
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

        // 追加测量结果摘要到日志
        const ImageData& lastOut = RunEngine::instance().lastOutput();
        if (lastOut.isValid()) {
            QString summary = measurementSummary(lastOut);
            if (!summary.isEmpty()) {
                Logger::instance().info(summary, "Measurement");
            }
        }

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

    // Connect viewport creating to forward picking signals
    connect(m_displayManager, &DisplayManager::viewportCreated, this, &MainWindow::onViewportCreated);
    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        onViewportCreated(viewport->viewportId(), viewport);
    }

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
        Qt::QueuedConnection);

    connect(
        &PluginManager::instance(), &PluginManager::pluginLoadFailed, this,
        [this](const QString& name, const QString& error) {
            Q_UNUSED(error);
            m_failedPlugins.append(name);
            m_splashScreen->appendLog(QString("<span style='color: #e94560;'>✗</span> %1").arg(name));
        },
        Qt::QueuedConnection);

    connect(
        &PluginManager::instance(), &PluginManager::pluginLoadProgress, this,
        [this, totalPlugins](int current, int total, const QString& name) {
            const int progressTotal = qMax(1, total);
            int progress = 30 + (current * 65) / progressTotal;
            m_splashScreen->setProgress(progress, tr("加载: %1 (%2/%3)").arg(name).arg(current).arg(total));
        },
        Qt::QueuedConnection);

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
        Qt::QueuedConnection);

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
    fileMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::NewFile, 24, QColor("#2563EB")), tr("新建方案"), this, &MainWindow::onNewSolution);
    fileMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::List, 24, QColor("#2563EB")), tr("方案列表"), this, &MainWindow::onSolutionList);
    fileMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::OpenFolder, 24, QColor("#D97706")), tr("打开"), this, &MainWindow::onOpenProject);
    fileMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Save, 24, QColor("#2563EB")), tr("保存"), this, &MainWindow::onSaveProject);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("退出"), qApp, &QApplication::quit);

    // 参数菜单
    QMenu* paramMenu = menuBar()->addMenu(tr("参数 (&P)"));
    paramMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Variable, 20, QColor("#7C3AED")), tr("全局变量"), this, &MainWindow::onGlobalVar);
    paramMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::User, 20, QColor("#2563EB")), tr("用户登录"), this, &MainWindow::onUserLogin);

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
    viewMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::QuickMode, 20, QColor("#D97706")), tr("快捷模式"), this, &MainWindow::onQuickMode);
    viewMenu->addSeparator();
    viewMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Theme, 20, QColor("#F59E0B")), tr("切换主题"), this, &MainWindow::onToggleTheme);

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
    toolMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Camera, 20, QColor("#374151")), tr("相机设置"), this, &MainWindow::onCameraSettings);
    toolMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Communication, 20, QColor("#0891B2")), tr("通讯设置"), this, &MainWindow::onCommSettings);
    toolMenu->addAction(AppIconProvider::icon(AppIconProvider::Icon::Hardware, 24, QColor("#4B5563")), tr("硬件配置"), this, &MainWindow::onHardwareConfig);
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
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::NewFile, 24, QColor("#2563EB")), tr("新建方案"), this, &MainWindow::onNewSolution);
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::List, 24, QColor("#2563EB")), tr("方案列表"), this, &MainWindow::onSolutionList);
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::OpenFolder, 24, QColor("#D97706")), tr("打开"), this, &MainWindow::onOpenProject);
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Save, 24, QColor("#2563EB")), tr("保存"), this, &MainWindow::onSaveProject);
    mainToolbar->addSeparator();

    // 运行控制
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Play, 24, QColor("#16A34A")), tr("单次运行"), this, &MainWindow::onRunOnce);
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Cycle, 24, QColor("#2563EB")), tr("循环运行"), this, &MainWindow::onRunCycle);
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Stop, 24, QColor("#DC2626")), tr("停止"), this, &MainWindow::onStop);
    mainToolbar->addSeparator();

    // 主题切换按钮
    mainToolbar->addAction(AppIconProvider::icon(AppIconProvider::Icon::Theme, 20, QColor("#F59E0B")), tr("切换主题"), this, &MainWindow::onToggleTheme);

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
    addToolBoxItem(imgProcItem, tr("图像采集"), "GrabImage");
    addToolBoxItem(imgProcItem, tr("保存图像"), "SaveImage");
    addToolBoxItem(imgProcItem, tr("显示图像"), "ShowImage");
    addToolBoxItem(imgProcItem, tr("图像预处理"), "PerProcessing");
    addToolBoxItem(imgProcItem, tr("颜色识别"), "ColorRecognition");
    addToolBoxItem(imgProcItem, tr("斑点分析"), "Blob");

    QTreeWidgetItem* detectItem = createCategoryItem(m_toolBoxTree, tr("02 - 检测识别"));
    detectItem->setExpanded(false);
    addToolBoxItem(detectItem, tr("模板匹配"), "Matching");
    addToolBoxItem(detectItem, tr("二维码识别"), "QRCode");

    QTreeWidgetItem* geometryItem = createCategoryItem(m_toolBoxTree, tr("03 - 几何测量"));
    geometryItem->setExpanded(false);
    geometryItem->setData(0, Qt::UserRole, "category");
    addToolBoxItem(geometryItem, tr("距离测量 (点到点)"), "DistancePP");
    addToolBoxItem(geometryItem, tr("距离测量 (点到线)"), "DistancePL");
    addToolBoxItem(geometryItem, tr("线段距离"), "LinesDistance");
    addToolBoxItem(geometryItem, tr("测量矩形"), "MeasureRect");
    addToolBoxItem(geometryItem, tr("测量直线"), "MeasureLine");
    addToolBoxItem(geometryItem, tr("测量间隙"), "MeasureGap");
    addToolBoxItem(geometryItem, tr("测量输入"), "MeasurementInput");

    QTreeWidgetItem* geoRelationItem = createCategoryItem(m_toolBoxTree, tr("04 - 几何关系"));
    geoRelationItem->setExpanded(false);
    addToolBoxItem(geoRelationItem, tr("找圆"), "FindCircle");
    addToolBoxItem(geoRelationItem, tr("圆拟合"), "FitCircle");
    addToolBoxItem(geoRelationItem, tr("直线拟合"), "FitLine");

    QTreeWidgetItem* calibItem = createCategoryItem(m_toolBoxTree, tr("05 - 坐标标定"));
    calibItem->setExpanded(false);
    addToolBoxItem(calibItem, tr("N 点标定"), "NPointCalibration");

    QTreeWidgetItem* alignItem = createCategoryItem(m_toolBoxTree, tr("06 - 对位工具"));
    alignItem->setExpanded(false);

    QTreeWidgetItem* logicItem = createCategoryItem(m_toolBoxTree, tr("07 - 逻辑工具"));
    logicItem->setExpanded(false);
    addToolBoxItem(logicItem, tr("如果"), "If");
    addToolBoxItem(logicItem, tr("循环"), "Loop");
    addToolBoxItem(logicItem, tr("While 循环"), "While");
    addToolBoxItem(logicItem, tr("停止循环"), "StopWhile");
    addToolBoxItem(logicItem, tr("条件判断"), "Condition");

    QTreeWidgetItem* systemItem = createCategoryItem(m_toolBoxTree, tr("08 - 系统工具"));
    systemItem->setExpanded(false);
    addToolBoxItem(systemItem, tr("系统时间"), "SystemTime");
    addToolBoxItem(systemItem, tr("文件夹操作"), "Folder");

    QTreeWidgetItem* varItem = createCategoryItem(m_toolBoxTree, tr("09 - 变量工具"));
    varItem->setExpanded(false);
    addToolBoxItem(varItem, tr("变量定义"), "VarDefine");
    addToolBoxItem(varItem, tr("变量设置"), "VarSet");
    addToolBoxItem(varItem, tr("数学运算"), "Math");
    addToolBoxItem(varItem, tr("数据检查"), "DataCheck");
    addToolBoxItem(varItem, tr("显示数据"), "DisplayData");

    QTreeWidgetItem* fileCommItem = createCategoryItem(m_toolBoxTree, tr("10 - 文件通讯"));
    fileCommItem->setExpanded(false);
    addToolBoxItem(fileCommItem, tr("保存数据"), "SaveData");
    addToolBoxItem(fileCommItem, tr("表格输出"), "TableOutPut");
    addToolBoxItem(fileCommItem, tr("写入文本"), "WriteText");

    QTreeWidgetItem* tool3DItem = createCategoryItem(m_toolBoxTree, tr("11 - 3D 工具"));
    tool3DItem->setExpanded(false);
    addToolBoxItem(tool3DItem, tr("加载点云"), "LoadPointCloud");

    QTreeWidgetItem* dlItem = createCategoryItem(m_toolBoxTree, tr("12 - 深度学习"));
    dlItem->setExpanded(false);

    QTreeWidgetItem* strItem = createCategoryItem(m_toolBoxTree, tr("13 - 字符串处理"));
    strItem->setExpanded(false);
    addToolBoxItem(strItem, tr("分割字符串"), "SplitString");
    addToolBoxItem(strItem, tr("字符串格式化"), "StrFormat");
    addToolBoxItem(strItem, tr("创建字符串"), "CreateString");

    QTreeWidgetItem* commItem = createCategoryItem(m_toolBoxTree, tr("14 - 通信"));
    commItem->setExpanded(false);
    addToolBoxItem(commItem, tr("PLC 通信"), "PLCCommunicate");
    addToolBoxItem(commItem, tr("PLC 读取"), "PLCRead");
    addToolBoxItem(commItem, tr("PLC 写入"), "PLCWrite");
    addToolBoxItem(commItem, tr("TCP 客户端"), "TCPClient");
    addToolBoxItem(commItem, tr("TCP 服务器"), "TCPServer");
    addToolBoxItem(commItem, tr("串口通信"), "SerialPort");

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
    m_btnStartPause->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Play, 24, QColor("#16A34A")));
    m_btnStartPause->setIconSize(QSize(24, 24));
    m_btnStartPause->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnStartPause->setObjectName("ProcessStartPauseBtn");

    m_btnStop = new QToolButton();
    m_btnStop->setToolTip(tr("停止"));
    m_btnStop->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_btnStop->setMinimumHeight(36);
    m_btnStop->setMaximumHeight(36);
    m_btnStop->setAutoRaise(true);
    m_btnStop->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Stop, 24, QColor("#DC2626")));
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
    runCycleBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Cycle, 24, QColor("#2563EB")));
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

    // 创建流程树控制器
    m_processTreeController = new ProcessTreeController(m_processTree, this);
    m_processTreeController->setFlowModules(&m_flowModules);
    m_processTreeController->setModulesNeedSyncFlag(&m_modulesNeedSync);
    m_processTreeController->setMeasurementPickMaps(&m_measurementPickCursor, &m_measurementPickCount);
    m_processTreeController->setToolDisplayNameCallback(
        [this](const QString& pluginName, const QString& fallback) {
            return toolDisplayName(pluginName, fallback);
        });
    m_processTreeController->setClearMeasurementOverlaysCallback(
        [this]() { clearMeasurementOverlays(); });
    connect(m_processTreeController, &ProcessTreeController::moduleAdded, this,
            [this](const ModuleInstance& module) {
                if (m_flowCanvas && !m_flowCanvas->nodeItem(module.id)) {
                    m_flowCanvas->addNode(module.moduleId,
                                          toolDisplayName(module.moduleId, module.name),
                                          QPointF(module.posX, module.posY), module.id);
                }
            });
    connect(m_processTreeController, &ProcessTreeController::moduleRemoved, this,
            [this](const QString& instanceId) {
                if (m_flowCanvas && m_flowCanvas->nodeItem(instanceId)) {
                    m_flowCanvas->removeNode(instanceId);
                }
            });

    flowLayout->addWidget(m_processTree);
    flowLayout->setStretchFactor(m_processTree, 1);

    // 提示标签（由控制器管理）
    m_processTreeController->showHintLabel();

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

void MainWindow::onProjectOpened(Project* project) {
    if (!project)
        return;

    // 清空现有流程树
    m_processTreeController->clear();
    if (m_flowCanvas) {
        m_flowCanvas->loadFromProject(nullptr);
    }

    // 加载项目中已有的模块
    for (const ModuleInstance& inst : project->modules()) {
        m_processTreeController->addModule(inst);
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
    disconnect(project, nullptr, m_processTreeController, nullptr);
    connect(project, &Project::moduleAdded, m_processTreeController, &ProcessTreeController::onModuleAddedFromProject);
    connect(project, &Project::moduleRemoved, m_processTreeController, &ProcessTreeController::onModuleRemovedFromProject);
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
    m_processTreeController->clear();
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

void MainWindow::onViewportCreated(const QString& viewportId, ViewportWidget* viewport) {
    Q_UNUSED(viewportId)
    if (!viewport)
        return;
    connect(viewport, &ViewportWidget::point2DClicked, this, &MainWindow::onPoint2DPicked, Qt::UniqueConnection);
    connect(viewport, &ViewportWidget::point3DClicked, this, &MainWindow::onPoint3DPicked, Qt::UniqueConnection);
}

void MainWindow::onPoint2DPicked(const QPointF& point) {
    QTreeWidgetItem* item = m_processTree->currentItem();
    if (!item || item->data(0, Qt::UserRole).toString() != "flow_item") {
        QGuiApplication::clipboard()->setText(pointText2D(point));
        Logger::instance().info(QString("2D pick: (%1, %2) — no MeasurementInput selected")
                                    .arg(point.x(), 0, 'f', 2)
                                    .arg(point.y(), 0, 'f', 2),
                                "Picking");
        return;
    }

    QString instanceId = item->data(0, Qt::UserRole + 1).toString();
    IModule* mod = m_flowModules.value(instanceId, nullptr);
    if (!mod) {
        QGuiApplication::clipboard()->setText(pointText2D(point));
        return;
    }

    if (mod->moduleId() != "com.deeplux.plugin.measurementinput") {
        QGuiApplication::clipboard()->setText(pointText2D(point));
        Logger::instance().info(QString("2D pick: (%1, %2) — selected module is not MeasurementInput")
                                    .arg(point.x(), 0, 'f', 2)
                                    .arg(point.y(), 0, 'f', 2),
                                "Picking");
        return;
    }

    QJsonObject params = mod->currentParams();
    const QString mode = params["mode"].toString("point_pair");
    const int cursor = m_measurementPickCursor.value(instanceId, 0);
    Project* project = ProjectManager::instance().currentProject();

    const QJsonArray newPoint2D = pointArray2D(point);
    const QJsonArray newPoint3D = pointArray3D(point.x(), point.y(), 0.0);
    auto finishPick = [&]() {
        const int count = m_measurementPickCount.value(instanceId, 0) + 1;
        m_measurementPickCount[instanceId] = count;
        refreshMeasurementOverlay(mod->currentParams(), count);
    };

    if (mode == "line_pair") {
        const int step = cursor % 4;
        const QString key = step < 2 ? QStringLiteral("line1") : QStringLiteral("line2");
        const bool firstPoint = (step % 2) == 0;
        QJsonArray line = updatedLineArray(params[key].toArray(), point, firstPoint);
        mod->setParam(key, line);
        if (project)
            project->setModuleParam(instanceId, key, line);
        Logger::instance().info(QString("2D pick: %1 p%2 set to (%3, %4)")
                                    .arg(key)
                                    .arg(firstPoint ? 1 : 2)
                                    .arg(point.x(), 0, 'f', 2)
                                    .arg(point.y(), 0, 'f', 2),
                                "Picking");
        m_measurementPickCursor[instanceId] = (step + 1) % 4;
        finishPick();
        return;
    }

    if (mode == "point_line") {
        const int step = cursor % 3;
        if (step == 0) {
            mod->setParam("point", newPoint3D);
            if (project)
                project->setModuleParam(instanceId, "point", newPoint3D);
            Logger::instance().info(
                QString("2D pick: point set to (%1, %2, 0)").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2),
                "Picking");
        } else {
            QJsonArray line = updatedLineArray(params["line"].toArray(), point, step == 1);
            mod->setParam("line", line);
            if (project)
                project->setModuleParam(instanceId, "line", line);
            Logger::instance().info(QString("2D pick: line p%1 set to (%2, %3)")
                                        .arg(step)
                                        .arg(point.x(), 0, 'f', 2)
                                        .arg(point.y(), 0, 'f', 2),
                                    "Picking");
        }
        m_measurementPickCursor[instanceId] = (step + 1) % 3;
        finishPick();
        return;
    }

    if (mode == "point_plane") {
        const int step = cursor % 4;
        if (step == 0) {
            mod->setParam("point", newPoint3D);
            if (project)
                project->setModuleParam(instanceId, "point", newPoint3D);
            Logger::instance().info(
                QString("2D pick: point set to (%1, %2, 0)").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2),
                "Picking");
        } else {
            QJsonArray plane = updatedPlaneArray(params["plane"].toArray(), newPoint3D, step - 1);
            mod->setParam("plane", plane);
            if (project)
                project->setModuleParam(instanceId, "plane", plane);
            Logger::instance().info(QString("2D pick: plane p%1 set to (%2, %3, 0)")
                                        .arg(step)
                                        .arg(point.x(), 0, 'f', 2)
                                        .arg(point.y(), 0, 'f', 2),
                                    "Picking");
        }
        m_measurementPickCursor[instanceId] = (step + 1) % 4;
        finishPick();
        return;
    }

    const bool setFirst = (cursor % 2) == 0;
    if (setFirst) {
        mod->setParam("point1", newPoint2D);
        if (project)
            project->setModuleParam(instanceId, "point1", newPoint2D);
        Logger::instance().info(
            QString("2D pick: point1 set to (%1, %2)").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2), "Picking");
    } else {
        mod->setParam("point2", newPoint2D);
        if (project)
            project->setModuleParam(instanceId, "point2", newPoint2D);
        Logger::instance().info(
            QString("2D pick: point2 set to (%1, %2)").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2), "Picking");
    }
    m_measurementPickCursor[instanceId] = (cursor + 1) % 2;
    finishPick();
}

void MainWindow::refreshMeasurementOverlay(const QJsonObject& params, int visibleSteps) {
    if (!m_displayManager) {
        return;
    }

    QList<MeasurementOverlayPoint> points;
    QList<MeasurementOverlayLine> lines;
    const QString mode = params["mode"].toString("point_pair");

    auto addPoint = [&](const QJsonArray& arr, const QString& label, int minStep, int offset = 0) {
        if (visibleSteps < minStep || arr.size() < offset + 2) {
            return;
        }
        points.append(MeasurementOverlayPoint{pointFromArray2D(arr, offset), label});
    };

    auto addLine = [&](const QJsonArray& arr, const QString& label, int minStep) {
        if (visibleSteps < minStep || arr.size() < 4) {
            return;
        }
        lines.append(MeasurementOverlayLine{pointFromArray2D(arr, 0), pointFromArray2D(arr, 2), label});
    };

    if (mode == QStringLiteral("point_line")) {
        const QJsonArray point = params["point"].toArray();
        const QJsonArray line = params["line"].toArray();
        addPoint(point, QStringLiteral("P"), 1);
        addPoint(line, QStringLiteral("L1"), 2, 0);
        addPoint(line, QStringLiteral("L2"), 3, 2);
        addLine(line, QStringLiteral("线段"), 3);
    } else if (mode == QStringLiteral("line_pair")) {
        const QJsonArray line1 = params["line1"].toArray();
        const QJsonArray line2 = params["line2"].toArray();
        addPoint(line1, QStringLiteral("L1-1"), 1, 0);
        addPoint(line1, QStringLiteral("L1-2"), 2, 2);
        addLine(line1, QStringLiteral("线1"), 2);
        addPoint(line2, QStringLiteral("L2-1"), 3, 0);
        addPoint(line2, QStringLiteral("L2-2"), 4, 2);
        addLine(line2, QStringLiteral("线2"), 4);
    } else if (mode == QStringLiteral("point_plane")) {
        const QJsonArray point = params["point"].toArray();
        const QJsonArray plane = params["plane"].toArray();
        addPoint(point, QStringLiteral("P"), 1);
        addPoint(plane, QStringLiteral("A"), 2, 0);
        addPoint(plane, QStringLiteral("B"), 3, 3);
        addPoint(plane, QStringLiteral("C"), 4, 6);
        if (visibleSteps >= 4 && plane.size() >= 9) {
            lines.append(MeasurementOverlayLine{pointFromArray2D(plane, 0), pointFromArray2D(plane, 3),
                                                QStringLiteral("平面边")});
            lines.append(MeasurementOverlayLine{pointFromArray2D(plane, 3), pointFromArray2D(plane, 6), QString()});
            lines.append(MeasurementOverlayLine{pointFromArray2D(plane, 6), pointFromArray2D(plane, 0), QString()});
        }
    } else {
        const QJsonArray point1 = params["point1"].toArray();
        const QJsonArray point2 = params["point2"].toArray();
        addPoint(point1, QStringLiteral("P1"), 1);
        addPoint(point2, QStringLiteral("P2"), 2);
        if (visibleSteps >= 2 && point1.size() >= 2 && point2.size() >= 2) {
            lines.append(MeasurementOverlayLine{pointFromArray2D(point1), pointFromArray2D(point2),
                                                QStringLiteral("P1-P2")});
        }
    }

    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        HImageWidget* imageWidget = viewport ? viewport->imageWidget() : nullptr;
        if (imageWidget && imageWidget->hasImage()) {
            imageWidget->setMeasurementOverlay(points, lines);
        }
    }
}

void MainWindow::refreshMeasurementOverlay3D(const QJsonObject& params, int visibleSteps) {
    if (!m_displayManager) {
        return;
    }

    QList<MeasurementOverlayPoint3D> points;
    QList<MeasurementOverlayLine3D> lines;
    const QString mode = params["mode"].toString("point_pair");

    auto addPoint3D = [&](const QJsonArray& arr, const QString& label, int minStep, int offset = 0) {
        if (visibleSteps < minStep || arr.size() < offset + 2) {
            return;
        }
        points.append(MeasurementOverlayPoint3D{pointFromArray3D(arr, offset), label});
    };

    auto addLine2D = [&](const QJsonArray& arr, const QString& label, int minStep) {
        if (visibleSteps < minStep || arr.size() < 4) {
            return;
        }
        lines.append(MeasurementOverlayLine3D{linePointFromArray3D(arr, 0), linePointFromArray3D(arr, 2), label});
    };

    if (mode == QStringLiteral("point_line")) {
        const QJsonArray point = params["point"].toArray();
        const QJsonArray line = params["line"].toArray();
        addPoint3D(point, QStringLiteral("P"), 1);
        if (visibleSteps >= 2 && line.size() >= 2) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line, 0), QStringLiteral("L1")});
        }
        if (visibleSteps >= 3 && line.size() >= 4) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line, 2), QStringLiteral("L2")});
        }
        addLine2D(line, QStringLiteral("线段(z=0)"), 3);
    } else if (mode == QStringLiteral("line_pair")) {
        const QJsonArray line1 = params["line1"].toArray();
        const QJsonArray line2 = params["line2"].toArray();
        if (visibleSteps >= 1 && line1.size() >= 2) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line1, 0), QStringLiteral("L1-1")});
        }
        if (visibleSteps >= 2 && line1.size() >= 4) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line1, 2), QStringLiteral("L1-2")});
        }
        addLine2D(line1, QStringLiteral("线1(z=0)"), 2);
        if (visibleSteps >= 3 && line2.size() >= 2) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line2, 0), QStringLiteral("L2-1")});
        }
        if (visibleSteps >= 4 && line2.size() >= 4) {
            points.append(MeasurementOverlayPoint3D{linePointFromArray3D(line2, 2), QStringLiteral("L2-2")});
        }
        addLine2D(line2, QStringLiteral("线2(z=0)"), 4);
    } else if (mode == QStringLiteral("point_plane")) {
        const QJsonArray point = params["point"].toArray();
        const QJsonArray plane = params["plane"].toArray();
        addPoint3D(point, QStringLiteral("P"), 1);
        addPoint3D(plane, QStringLiteral("A"), 2, 0);
        addPoint3D(plane, QStringLiteral("B"), 3, 3);
        addPoint3D(plane, QStringLiteral("C"), 4, 6);
        if (visibleSteps >= 4 && plane.size() >= 9) {
            lines.append(MeasurementOverlayLine3D{pointFromArray3D(plane, 0), pointFromArray3D(plane, 3),
                                                  QStringLiteral("平面边")});
            lines.append(MeasurementOverlayLine3D{pointFromArray3D(plane, 3), pointFromArray3D(plane, 6), QString()});
            lines.append(MeasurementOverlayLine3D{pointFromArray3D(plane, 6), pointFromArray3D(plane, 0), QString()});
        }
    } else {
        const QJsonArray point1 = params["point1"].toArray();
        const QJsonArray point2 = params["point2"].toArray();
        addPoint3D(point1, QStringLiteral("P1"), 1);
        addPoint3D(point2, QStringLiteral("P2"), 2);
        if (visibleSteps >= 2 && point1.size() >= 2 && point2.size() >= 2) {
            lines.append(
                MeasurementOverlayLine3D{pointFromArray3D(point1), pointFromArray3D(point2), QStringLiteral("P1-P2")});
        }
    }

    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        Viewport3DContent* content = viewport ? viewport->viewport3D() : nullptr;
        if (content) {
            content->setMeasurementOverlay(points, lines);
        }
    }
}

void MainWindow::clearMeasurementOverlays() {
    if (!m_displayManager) {
        return;
    }

    for (ViewportWidget* viewport : m_displayManager->allViewports()) {
        if (!viewport) {
            continue;
        }
        if (HImageWidget* imageWidget = viewport->imageWidget()) {
            imageWidget->clearMeasurementOverlay();
        }
        if (Viewport3DContent* content = viewport->viewport3D()) {
            content->clearMeasurementOverlay();
        }
    }
}

void MainWindow::onPoint3DPicked(const QVector3D& point) {
    QTreeWidgetItem* item = m_processTree->currentItem();
    if (!item || item->data(0, Qt::UserRole).toString() != "flow_item") {
        QGuiApplication::clipboard()->setText(pointText3D(point));
        Logger::instance().info(QString("3D pick: (%1, %2, %3) — no MeasurementInput selected")
                                    .arg(point.x(), 0, 'f', 3)
                                    .arg(point.y(), 0, 'f', 3)
                                    .arg(point.z(), 0, 'f', 3),
                                "Picking");
        return;
    }

    QString instanceId = item->data(0, Qt::UserRole + 1).toString();
    IModule* mod = m_flowModules.value(instanceId, nullptr);
    if (!mod) {
        QGuiApplication::clipboard()->setText(pointText3D(point));
        return;
    }

    if (mod->moduleId() != "com.deeplux.plugin.measurementinput") {
        QGuiApplication::clipboard()->setText(pointText3D(point));
        Logger::instance().info(QString("3D pick: (%1, %2, %3) — selected module is not MeasurementInput")
                                    .arg(point.x(), 0, 'f', 3)
                                    .arg(point.y(), 0, 'f', 3)
                                    .arg(point.z(), 0, 'f', 3),
                                "Picking");
        return;
    }

    QJsonObject params = mod->currentParams();
    const QString mode = params["mode"].toString("point_pair");
    const int cursor = m_measurementPickCursor.value(instanceId, 0);
    Project* project = ProjectManager::instance().currentProject();
    const QJsonArray newPoint = pointArray3D(point.x(), point.y(), point.z());
    auto finishPick = [&]() {
        const int count = m_measurementPickCount.value(instanceId, 0) + 1;
        m_measurementPickCount[instanceId] = count;
        refreshMeasurementOverlay3D(mod->currentParams(), count);
    };

    if (mode == "point_plane") {
        const int step = cursor % 4;
        if (step == 0) {
            mod->setParam("point", newPoint);
            if (project)
                project->setModuleParam(instanceId, "point", newPoint);
            Logger::instance().info(QString("3D pick: point set to (%1, %2, %3)")
                                        .arg(point.x(), 0, 'f', 3)
                                        .arg(point.y(), 0, 'f', 3)
                                        .arg(point.z(), 0, 'f', 3),
                                    "Picking");
        } else {
            QJsonArray plane = updatedPlaneArray(params["plane"].toArray(), newPoint, step - 1);
            mod->setParam("plane", plane);
            if (project)
                project->setModuleParam(instanceId, "plane", plane);
            Logger::instance().info(QString("3D pick: plane p%1 set to (%2, %3, %4)")
                                        .arg(step)
                                        .arg(point.x(), 0, 'f', 3)
                                        .arg(point.y(), 0, 'f', 3)
                                        .arg(point.z(), 0, 'f', 3),
                                    "Picking");
        }
        m_measurementPickCursor[instanceId] = (step + 1) % 4;
        finishPick();
        return;
    }

    if (mode == "line_pair") {
        const int step = cursor % 4;
        const QString key = step < 2 ? QStringLiteral("line1") : QStringLiteral("line2");
        const bool firstPoint = (step % 2) == 0;
        QJsonArray line = updatedLineArray(params[key].toArray(), QPointF(point.x(), point.y()), firstPoint);
        mod->setParam(key, line);
        if (project)
            project->setModuleParam(instanceId, key, line);
        Logger::instance().info(QString("3D pick: %1 p%2 set to (%3, %4, z=0; input z %5 ignored)")
                                    .arg(key)
                                    .arg(firstPoint ? 1 : 2)
                                    .arg(point.x(), 0, 'f', 3)
                                    .arg(point.y(), 0, 'f', 3)
                                    .arg(point.z(), 0, 'f', 3),
                                "Picking");
        m_measurementPickCursor[instanceId] = (step + 1) % 4;
        finishPick();
        return;
    }

    if (mode == "point_line") {
        const int step = cursor % 3;
        if (step == 0) {
            mod->setParam("point", newPoint);
            if (project)
                project->setModuleParam(instanceId, "point", newPoint);
            Logger::instance().info(QString("3D pick: point set to (%1, %2, %3)")
                                        .arg(point.x(), 0, 'f', 3)
                                        .arg(point.y(), 0, 'f', 3)
                                        .arg(point.z(), 0, 'f', 3),
                                    "Picking");
        } else {
            QJsonArray line = updatedLineArray(params["line"].toArray(), QPointF(point.x(), point.y()), step == 1);
            mod->setParam("line", line);
            if (project)
                project->setModuleParam(instanceId, "line", line);
            Logger::instance().info(QString("3D pick: line p%1 set to (%2, %3, z=0; input z %4 ignored)")
                                        .arg(step)
                                        .arg(point.x(), 0, 'f', 3)
                                        .arg(point.y(), 0, 'f', 3)
                                        .arg(point.z(), 0, 'f', 3),
                                    "Picking");
        }
        m_measurementPickCursor[instanceId] = (step + 1) % 3;
        finishPick();
        return;
    }

    const bool setFirst = (cursor % 2) == 0;
    if (setFirst) {
        mod->setParam("point1", newPoint);
        if (project)
            project->setModuleParam(instanceId, "point1", newPoint);
        Logger::instance().info(QString("3D pick: point1 set to (%1, %2, %3)")
                                    .arg(point.x(), 0, 'f', 3)
                                    .arg(point.y(), 0, 'f', 3)
                                    .arg(point.z(), 0, 'f', 3),
                                "Picking");
    } else {
        mod->setParam("point2", newPoint);
        if (project)
            project->setModuleParam(instanceId, "point2", newPoint);
        Logger::instance().info(QString("3D pick: point2 set to (%1, %2, %3)")
                                    .arg(point.x(), 0, 'f', 3)
                                    .arg(point.y(), 0, 'f', 3)
                                    .arg(point.z(), 0, 'f', 3),
                                "Picking");
    }
    m_measurementPickCursor[instanceId] = (cursor + 1) % 2;
    finishPick();
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

void MainWindow::addMeasurementConfigAction(QVBoxLayout* layout, const QString& consumerModuleId,
                                            const QString& consumerInstanceId, QDialog* dialog) {
    const QString mode = measurementInputModeForConsumer(consumerModuleId);
    if (mode.isEmpty() || !layout || !dialog) {
        return;
    }

    QGroupBox* group = new QGroupBox(tr("测量元素"));
    group->setObjectName("MeasurementInputActionGroup");
    QVBoxLayout* groupLayout = new QVBoxLayout(group);
    groupLayout->setContentsMargins(10, 8, 10, 10);
    groupLayout->setSpacing(8);

    QLabel* label = new QLabel(tr("需要输入：%1").arg(measurementInputModeText(mode)), group);
    label->setWordWrap(true);
    groupLayout->addWidget(label);

    QPushButton* button = new QPushButton(tr("添加测量输入并开始拾取"), group);
    button->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Add, 16, QColor("#2563EB")));
    button->setObjectName("MeasurementInputSetupButton");
    button->setProperty("measurementInputMode", mode);
    button->setMinimumHeight(qMax(30, button->fontMetrics().height() + 10));
    groupLayout->addWidget(button);

    connect(button, &QPushButton::clicked, this, [this, mode, consumerInstanceId, dialog]() {
        const QString inputId = ensureMeasurementInputForMode(mode, consumerInstanceId);
        if (inputId.isEmpty()) {
            QMessageBox::warning(this, tr("测量输入不可用"), tr("无法创建测量输入插件，请先同步并重启插件。"));
            return;
        }
        Logger::instance().info(tr("已创建测量输入：%1，开始在视图中拾取元素").arg(inputId), "Config");
        dialog->accept();
    });

    layout->addWidget(group);
}

QString MainWindow::ensureMeasurementInputForMode(const QString& mode, const QString& consumerInstanceId) {
    if (mode.isEmpty()) {
        return QString();
    }

    PluginManager& pm = PluginManager::instance();
    if (!pm.isPluginLoaded(QStringLiteral("MeasurementInput")) && !pm.loadPlugin(QStringLiteral("MeasurementInput"))) {
        return QString();
    }

    Project* project = ProjectManager::instance().currentProject();
    if (!project) {
        return QString();
    }

    const QString baseId = consumerInstanceId.isEmpty() ? QStringLiteral("measurement_input")
                                                        : QStringLiteral("%1_input").arg(consumerInstanceId);
    QString instanceId = baseId;
    int counter = 1;
    while (project->findModule(instanceId) || m_processTreeController->containsInstance(instanceId)) {
        instanceId = QStringLiteral("%1_%2").arg(baseId).arg(counter++);
    }

    ModuleInstance inst;
    inst.id = instanceId;
    inst.moduleId = QStringLiteral("MeasurementInput");
    inst.name = toolDisplayName(QStringLiteral("MeasurementInput"), tr("测量输入"));
    inst.params["mode"] = mode;

    int insertRow = m_processTreeController->topLevelItemCount();
    if (m_processTreeController->containsInstance(consumerInstanceId)) {
        const int consumerRow = m_processTreeController->indexOfTopLevelItem(
            m_processTreeController->instanceItem(consumerInstanceId));
        if (consumerRow >= 0) {
            insertRow = consumerRow;
        }
    }

    project->addModule(inst);
    project->moveModule(instanceId, insertRow);

    QTreeWidgetItem* inputItem = m_processTreeController->instanceItem(instanceId);
    if (inputItem) {
        const int currentRow = m_processTreeController->indexOfTopLevelItem(inputItem);
        if (currentRow >= 0 && currentRow != insertRow) {
            m_processTreeController->takeTopLevelItem(currentRow);
            m_processTreeController->insertTopLevelItem(insertRow, inputItem);
        }
        m_processTreeController->setCurrentItem(inputItem);
    }

    m_measurementPickCursor[instanceId] = 0;
    m_measurementPickCount[instanceId] = 0;
    m_modulesNeedSync = true;
    return instanceId;
}

void MainWindow::applyTheme() {
    // 首先更新 ConfigWidgetHelper 的全局主题状态
    ConfigWidgetHelper::setGlobalDarkTheme(m_isDarkTheme);

    // 更新所有子控件的 ConfigWidgetHelper 样式（包括对话框中的控件）
    ConfigWidgetHelper::updateAllWidgetsStyle(this, m_isDarkTheme);

    setStyleSheet(ThemeManager::styleSheet(m_isDarkTheme));

    // 更新自定义标题栏样式
    const ThemePalette pal = ThemeManager::palette(m_isDarkTheme);

    if (m_toolBoxDock && m_toolBoxDock->titleBarWidget()) {
        m_toolBoxDock->titleBarWidget()->setStyleSheet(
            QString("background-color: %1; border: none; border-bottom: 1px solid %2;").arg(pal.bgColor, pal.borderColor));
        QLabel* label = m_toolBoxDock->titleBarWidget()->findChild<QLabel*>();
        if (label)
            label->setStyleSheet(QString("QLabel { color: %1; font-weight: 600; font-size: 13px; }").arg(pal.textColor));
        QToolButton* btn = m_toolBoxDock->titleBarWidget()->findChild<QToolButton*>();
        if (btn)
            btn->setStyleSheet(
                QString("QToolButton { background-color: transparent; color: %1; font-size: 18px; border: none; }"
                        "QToolButton:hover { background-color: #e74c3c; }")
                    .arg(pal.btnColor));
    }
    if (m_logDock && m_logDock->titleBarWidget()) {
        m_logDock->titleBarWidget()->setStyleSheet(
            QString("background-color: %1; border: none; border-bottom: 1px solid %2;").arg(pal.bgColor, pal.borderColor));
        QLabel* label = m_logDock->titleBarWidget()->findChild<QLabel*>();
        if (label)
            label->setStyleSheet(QString("QLabel { color: %1; font-weight: 600; font-size: 13px; }").arg(pal.textColor));
    }

    if (m_toolBoxTree) {
        m_toolBoxTree->setStyleSheet(
            QString("QTreeWidget { background-color: %1; color: %2; border: none; font-size: 13px; }"
                    "QTreeWidget::item { height: 28px; padding-left: 2px; padding-right: 2px; }"
                    "QTreeWidget::item:hover { background-color: %3; }"
                    "QTreeWidget::item:selected { background-color: #0078d7; color: #ffffff; }")
                .arg(pal.treeBgColor, pal.treeTextColor, pal.treeHoverColor));
    }
    if (m_processTree) {
        m_processTree->setStyleSheet(
            QString("QTreeWidget { background-color: %1; color: %2; border: none; font-size: 13px; }"
                    "QTreeWidget::item { height: 28px; padding-left: 2px; padding-right: 2px; }"
                    "QTreeWidget::item:hover { background-color: %3; }"
                    "QTreeWidget::item:selected { background-color: #0078d7; color: #ffffff; }")
                .arg(pal.treeBgColor, pal.treeTextColor, pal.treeHoverColor));
    }

    QScrollArea* toolCategoryScroll = findChild<QScrollArea*>("ToolCategoryScroll");
    if (toolCategoryScroll) {
        toolCategoryScroll->setStyleSheet(QString("QScrollArea { background-color: %1; border: none; }"
                                                  "QScrollArea > QWidget > QWidget { background-color: %1; }")
                                              .arg(pal.scrollBgColor));
    }

    if (m_processStatusWidget) {
        m_processStatusWidget->setStyleSheet(QString("background-color: %1;").arg(pal.bgColor));
        if (m_processTimeLabel) {
            m_processTimeLabel->setStyleSheet(QString("QLabel { color: %1; }").arg(pal.textColor));
        }
    }

    const QString processToolButtonStyle =
        QString("QToolButton { background-color: %1; color: %2; border: 1px solid %3; border-radius: 3px; "
                "padding: 4px 6px; }"
                "QToolButton:hover { background-color: %4; }"
                "QToolButton:disabled { background-color: %1; color: #999999; }")
            .arg(pal.btnBgColor, pal.btnTextColor, pal.toolBtnBorderColor, pal.btnHoverColor);
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

    QWidget* procToolBar = findChild<QWidget*>("ProcessToolBar");
    if (procToolBar) {
        procToolBar->setStyleSheet(QString("background-color: %1;").arg(pal.scrollBgColor));
    }

    QWidget* viewToggleWidget = findChild<QWidget*>("ViewToggleWidget");
    if (viewToggleWidget) {
        viewToggleWidget->setStyleSheet(QString("background-color: %1;").arg(pal.scrollBgColor));
    }

    QWidget* toolPanelWidget = findChild<QWidget*>("ToolPanelWidget");
    if (toolPanelWidget) {
        toolPanelWidget->setStyleSheet(QString("background-color: %1;").arg(pal.scrollBgColor));
    }
    QWidget* processPanelWidget = findChild<QWidget*>("ProcessPanelWidget");
    if (processPanelWidget) {
        processPanelWidget->setStyleSheet(
            QString("background-color: %1; border-right: 1px solid %2;").arg(pal.scrollBgColor, pal.panelBorderColor));
    }
    QWidget* imageDisplayWidget = findChild<QWidget*>("ImageDisplayWidget");
    if (imageDisplayWidget) {
        imageDisplayWidget->setStyleSheet(QString("background-color: %1; border-left: 1px solid %2; "
                                                  "border-bottom: 1px solid %2;")
                                              .arg(pal.imageDisplayBg, pal.panelBorderColor));
    }
    if (m_logDock) {
        m_logDock->setStyleSheet(QString("QDockWidget#LogDock { border-top: 1px solid %1; }").arg(pal.panelBorderColor));
    }
    if (m_logTable) {
        m_logTable->setFrameShape(QFrame::NoFrame);
        m_logTable->setStyleSheet(
            QString("QTableWidget#LogTable { background-color: %1; color: %2; border: none; outline: none; "
                    "gridline-color: %3; font-size: 13px; }"
                    "QTableWidget#LogTable::item { border-bottom: 1px solid %3; }"
                    "QTableWidget#LogTable::item:selected { background-color: #0078d7; color: #ffffff; }"
                    "QHeaderView::section { background-color: %4; color: %2; padding: 5px; border: none; "
                    "border-bottom: 1px solid %3; font-size: 13px; }"
                    "QTableCornerButton::section { background-color: %4; border: none; }")
                .arg(pal.logTableBg, pal.logTextColor, pal.logLineColor, pal.logHeaderBg));
    }

    if (m_processTabWidget) {
        m_processTabWidget->setStyleSheet(
            QString("QTabWidget::pane { border: none; border-top: 2px solid %1; background-color: %2; }"
                    "QTabBar::tab { background-color: transparent; color: %3; font-size: 13px; font-weight: 500;"
                    "  min-height: 26px; padding: 3px 10px; border: none; border-bottom: 2px solid transparent;"
                    "  margin-right: 2px; }"
                    "QTabBar::tab:selected { color: #0078d7; border-bottom: 2px solid #0078d7; }"
                    "QTabBar::tab:hover:!selected { color: %3; background-color: %1; }")
                .arg(pal.processTabBorder, pal.processTabBg, pal.processTabFg));
    }
    if (m_logTerminalTabs) {
        m_logTerminalTabs->setStyleSheet(
            QString("QTabWidget::pane { border: none; background-color: %1; }"
                    "QTabBar::tab { background-color: %2; color: %3; font-size: 13px; font-weight: 500;"
                    "  min-height: 26px; padding: 3px 10px; border: none; margin-right: 1px; }"
                    "QTabBar::tab:selected { background-color: %1; color: %4; }"
                    "QTabBar::tab:hover:!selected { background-color: %5; }")
                .arg(pal.logTabPaneBg, pal.logTabBg, pal.logTabFg, pal.logTabSelFg, pal.logTabHoverBg));
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
                    dialog->setObjectName("PluginConfigDialog");
                    dialog->setWindowTitle(tr("配置 - %1").arg(item->text(0)));
                    dialog->setMinimumSize(460, 340);
                    dialog->setStyleSheet(pluginConfigDialogStyle(m_isDarkTheme));
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    QVBoxLayout* layout = new QVBoxLayout(dialog);
                    layout->setContentsMargins(14, 12, 14, 12);
                    layout->setSpacing(10);
                    addMeasurementConfigAction(layout, module->moduleId(), instanceName, dialog);
                    configWidget->setObjectName("PluginConfigContent");
                    applyPluginConfigTheme(configWidget, m_isDarkTheme);
                    QScrollArea* scrollArea = new QScrollArea(dialog);
                    scrollArea->setObjectName("PluginConfigScrollArea");
                    scrollArea->setWidgetResizable(true);
                    scrollArea->setFrameShape(QFrame::NoFrame);
                    scrollArea->setWidget(configWidget);
                    layout->addWidget(scrollArea, 1);
                    QHBoxLayout* btnLayout = new QHBoxLayout();
                    QPushButton* okBtn = new QPushButton(tr("确定"));
                    okBtn->setObjectName("PluginConfigOkButton");
                    okBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Confirm, 16, QColor("#16A34A")));
                    QPushButton* cancelBtn = new QPushButton(tr("取消"));
                    cancelBtn->setObjectName("PluginConfigCancelButton");
                    cancelBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Cancel, 16, QColor("#DC2626")));
                    btnLayout->addStretch();
                    btnLayout->addWidget(okBtn);
                    btnLayout->addWidget(cancelBtn);
                    layout->addLayout(btnLayout);

                    // dialog 关闭时把 configWidget 从 dialog 分离，防止被 WA_DeleteOnClose 销毁
                    // 插件可能缓存了 configWidget 指针，销毁会导致下次 createConfigWidget crash
                    connect(dialog, &QDialog::finished, dialog, [scrollArea, configWidget]() {
                        if (scrollArea->widget() == configWidget) {
                            scrollArea->takeWidget();
                        }
                        configWidget->setParent(nullptr);
                    });

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
    if (m_processTreeController && (watched == m_processTree || watched == m_processTree->viewport()) &&
        event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Delete) {
            QList<QTreeWidgetItem*> selected = m_processTreeController->selectedItems();
            if (!selected.isEmpty()) {
                QTreeWidgetItem* item = selected.first();
                QString instanceName = item->data(0, Qt::UserRole + 1).toString();
                m_processTreeController->removeFlowModule(instanceName);
            }
            return true;
        }
    }

    // 处理拖放到流程树
    if (m_processTreeController) {
        QTreeWidget* tree = m_processTreeController->tree();
        QWidget* processViewport = tree->viewport();
        if (processViewport && watched == processViewport &&
            (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
             event->type() == QEvent::Drop)) {
            QDropEvent* dropEvent = static_cast<QDropEvent*>(event);

            // 获取拖放的数据
            const QMimeData* mimeData = dropEvent->mimeData();
            if (!mimeData) {
                return QMainWindow::eventFilter(watched, event);
            }

            auto insertRowForDrop = [this, tree, dropEvent]() {
                QTreeWidgetItem* hoverItem = tree->itemAt(dropEvent->pos());
                if (!hoverItem) {
                    return tree->topLevelItemCount();
                }

                const QRect itemRect = tree->visualItemRect(hoverItem);
                const int itemMiddle = itemRect.top() + itemRect.height() / 2;
                const int hoverRow = tree->indexOfTopLevelItem(hoverItem);
                return dropEvent->pos().y() < itemMiddle ? hoverRow : hoverRow + 1;
            };

            const bool isToolBoxDrop =
                dropEvent->source() == m_toolBoxTree || dropEvent->source() == m_toolBoxTree->viewport();
            const bool isProcessTreeDrop =
                dropEvent->source() == tree || dropEvent->source() == tree->viewport();
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
                QTreeWidgetItem* sourceItem = m_processTreeController->currentItem();
                if (!sourceItem || sourceItem->data(0, Qt::UserRole).toString() != "flow_item") {
                    dropEvent->ignore();
                    return true;
                }

                const int sourceRow = m_processTreeController->indexOfTopLevelItem(sourceItem);
                int insertRow = insertRowForDrop();
                if (sourceRow < 0 || insertRow == sourceRow || insertRow == sourceRow + 1) {
                    dropEvent->setDropAction(Qt::MoveAction);
                    dropEvent->accept();
                    return true;
                }

                QTreeWidgetItem* movedItem = m_processTreeController->takeTopLevelItem(sourceRow);
                if (insertRow > sourceRow) {
                    --insertRow;
                }
                insertRow = qMax(0, qMin(insertRow, m_processTreeController->topLevelItemCount()));
                m_processTreeController->insertTopLevelItem(insertRow, movedItem);
                m_processTreeController->setCurrentItem(movedItem);

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
                        if (!ProjectManager::instance().currentProject() && !ProjectManager::instance().newProject()) {
                            Logger::instance().error(tr("无法创建工程，不能添加插件：%1").arg(pluginName), "Flow");
                            dropEvent->ignore();
                            return true;
                        }

                        // 隐藏提示标签
                        m_processTreeController->hideHintLabel();

                        const int insertRow = insertRowForDrop();

                        // 先添加树节点（即时视觉反馈），再异步创建模块实例
                        QTreeWidgetItem* newItem = new QTreeWidgetItem();
                        newItem->setFlags((newItem->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
                        m_processTreeController->insertTopLevelItem(insertRow, newItem);

                        QString instanceName = pluginName;
                        int counter = 1;
                        while (m_processTreeController->isUsedName(instanceName)) {
                            instanceName = QString("%1_%2").arg(pluginName).arg(counter++);
                        }
                        m_processTreeController->insertUsedName(instanceName);
                        newItem->setData(0, Qt::UserRole, "flow_item");
                        newItem->setData(0, Qt::UserRole + 1, instanceName);
                        newItem->setData(0, Qt::UserRole + 2, pluginName);
                        m_processTreeController->insertInstanceItem(instanceName, newItem); // 防 Project 信号重复创建

                        // 推迟模块创建到事件循环 — 避免在拖放嵌套循环中阻塞
                        QTimer::singleShot(0, this, [this, pluginName, instanceName, newItem]() {
                            DeepLux::PluginManager& pm = DeepLux::PluginManager::instance();
                            IModule* module = pm.createModule(pluginName);
                            if (!module) {
                                Logger::instance().error(tr("无法创建插件：%1").arg(pluginName), "Flow");
                                m_processTreeController->removeModule(instanceName);
                                return;
                            }
                            if (!module->initialize()) {
                                Logger::instance().error(tr("插件初始化失败：%1").arg(pluginName), "Flow");
                                delete module;
                                m_processTreeController->removeModule(instanceName);
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
                                proj->moveModule(instanceName, m_processTreeController->indexOfTopLevelItem(newItem));
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
        m_btnStartPause->setIcon(running ? AppIconProvider::icon(AppIconProvider::Icon::Pause, 24, QColor("#D97706")) : AppIconProvider::icon(AppIconProvider::Icon::Play, 24, QColor("#16A34A")));
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
    m_processTreeController->clearSelection();
    m_processTreeController->setCurrentItem(nullptr);
    for (int i = 0; i < m_processTreeController->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_processTreeController->topLevelItem(i);
        item->setBackground(0, QBrush());
        item->setForeground(0, QBrush());
        item->setBackground(1, QBrush());
        item->setForeground(1, QBrush());
    }
    m_currentExecutingItem = nullptr;

    // 如果没有模块，直接返回
    if (m_processTreeController->topLevelItemCount() == 0) {
        if (m_processTimeLabel) {
            m_processTimeLabel->setText(tr("总耗时：0 ms"));
        }
        return;
    }

    RunEngine& engine = RunEngine::instance();

    // 仅在模块变更后重新同步（避免循环模式下每轮重复 addModule）
    if (m_modulesNeedSync) {
        m_processTreeController->clearInstanceItemMap();
        engine.clearModules();
        for (int i = 0; i < m_processTreeController->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = m_processTreeController->topLevelItem(i);
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
                m_processTreeController->setInstanceItem(instanceName, item);
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
    if (name == QStringLiteral("delete")) {
        return AppIconProvider::icon(AppIconProvider::Icon::Delete, 24, QColor("#DC2626"));
    }
    return AppIconProvider::icon(AppIconProvider::Icon::Settings, 24, QColor("#374151"));
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
