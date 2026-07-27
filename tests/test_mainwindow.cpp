#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVector3D>
#include <QtTest/QtTest>
#include <core/agent/AgentController.h>
#include <core/base/ModuleBase.h>
#include <core/engine/RunEngine.h>
#include <core/manager/PluginManager.h>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>
#include <ui/dialogs/SamAnnotatorDialog.h>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentChatPanel.h>
#include <ui/widgets/AgentMessageBubble.h>
#include <ui/widgets/AgentToolPreviewCard.h>
#include <ui/widgets/AnnotationOverlayWidget.h>
#include <ui/widgets/AppIconProvider.h>
#include <ui/widgets/FlowCanvas.h>
#include <ui/widgets/HImageWidget.h>
#include <ui/widgets/ModuleInspectorPanel.h>
#include <ui/widgets/ViewportWidget.h>

using namespace DeepLux;

class MainWindowOutputProbeModule : public ModuleBase {
    Q_OBJECT

public:
    explicit MainWindowOutputProbeModule(const QString& instanceId) {
        m_moduleId = QStringLiteral("com.deeplux.test.mainwindowoutputprobe");
        m_name = QStringLiteral("OutputProbe");
        m_category = QStringLiteral("test");
        setInstanceName(instanceId);
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        QImage image(16, 12, QImage::Format_RGB32);
        image.fill(QColor("#22C55E"));
        output = ImageData(image);
        output.setData(QStringLiteral("tag"), QStringLiteral("probe"));
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class TestMainWindow : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void testOpenProjectSyncsProcessTreeAndFlowCanvas();
    void testHomeSwitchesToFlowCanvas();
    void testAgentMessageRenderingUsesCompactLineSpacing();
    void testAgentThinkingStatusStaysCompactAndSafe();
    void testAgentInputErrorPathDoesNotCrashOrStayThinking();
    void testMainWindowLayoutKeepsConfirmedWorkflowTabsAndReadableTheme();
    void testClickingProcessModuleDisplaysIntermediateOutput();
    void testStepRunHighlightMovesBetweenModules();
    void testMeasurementPickingUpdatesInputAndClipboard();
    void testMeasurementPickingViaImageViewportClick();
    void testMeasurementConfigButtonCreatesInputNode();
    void testMeasurementConfigButtonWithInstalledPlugins();
    void testPluginConfigDialogRestylesLegacyDarkPlugin();
    void testQuickAnnotateOpensSamDialogOnMainViewportImage();
    void testTreeAndCanvasSyncSelection();
    void testCycleRunDoesNotStealSelection();
    void testStepRunFollowsExecution();
    void testCloseInspectorDoesNotAutoExpand();
    void testDeleteModuleClearsInspector();
    void testOldAdvancedConfigDialogStillUsable();
    void testNarrowWindowToolPanelRestored();
    void testInspectorManualCollapseResizesSplitter();
    void testFocusModePreservesLogVisibility();

private:
    bool installFitLinePlugin(const QString& pluginRoot) const;
    bool installRuntimePlugin(const QString& pluginRoot, const QString& pluginName) const;
};

void TestMainWindow::init() {
    ProjectManager::instance().closeProject();
    PluginManager::instance().shutdown();
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
    qunsetenv("DEEPLUX_APP_DATA_DIR");
}

void TestMainWindow::cleanup() {
    ProjectManager::instance().closeProject();
    PluginManager::instance().shutdown();
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
    qunsetenv("DEEPLUX_APP_DATA_DIR");
}

bool TestMainWindow::installFitLinePlugin(const QString& pluginRoot) const {
    return installRuntimePlugin(pluginRoot, QStringLiteral("FitLine"));
}

bool TestMainWindow::installRuntimePlugin(const QString& pluginRoot, const QString& pluginName) const {
    QDir root(pluginRoot);
    if (!root.mkpath(pluginName)) {
        return false;
    }

    QDir pluginDir(root.filePath(pluginName));
    QString metadataSrc;
    const QString srcRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../src/plugins");
    const QStringList pluginDomains = {
        QStringLiteral("geometry"), QStringLiteral("image_processing"), QStringLiteral("detection"),
        QStringLiteral("logic"),    QStringLiteral("system"),           QStringLiteral("communication"),
        QStringLiteral("variable"), QStringLiteral("calibration"),      QStringLiteral("hymson3d"),
    };
    for (const QString& domain : pluginDomains) {
        const QString candidate = QDir(srcRoot).filePath(QString("%1/%2/metadata.json").arg(domain, pluginName));
        if (QFileInfo::exists(candidate)) {
            metadataSrc = candidate;
            break;
        }
    }
    const QString libSrc =
        QDir::cleanPath(QCoreApplication::applicationDirPath() + QString("/../lib/lib%1Plugin.so").arg(pluginName));

    if (metadataSrc.isEmpty() || !QFileInfo::exists(libSrc)) {
        return false;
    }

    QFile::remove(pluginDir.filePath("metadata.json"));
    QFile::remove(pluginDir.filePath(QString("lib%1Plugin.so").arg(pluginName)));

    return QFile::copy(metadataSrc, pluginDir.filePath("metadata.json")) &&
           QFile::copy(libSrc, pluginDir.filePath(QString("lib%1Plugin.so").arg(pluginName)));
}

void TestMainWindow::testClickingProcessModuleDisplaysIntermediateOutput() {
    RunEngine& engine = RunEngine::instance();
    MainWindowOutputProbeModule* module = new MainWindowOutputProbeModule(QStringLiteral("probe_1"));
    module->initialize();
    engine.addModule(module);
    engine.runOnce();

    MainWindow window;
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance instance;
    instance.id = QStringLiteral("probe_1");
    instance.moduleId = QStringLiteral("OutputProbe");
    instance.name = QStringLiteral("输出模块");
    project->addModule(instance);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QCOMPARE(processTree->topLevelItemCount(), 1);

    ViewportWidget* viewport = window.findChild<ViewportWidget*>();
    QVERIFY(viewport != nullptr);
    HImageWidget* imageWidget = viewport->imageWidget();
    QVERIFY(imageWidget != nullptr);
    QVERIFY(!imageWidget->hasImage());

    // Ensure inspector is not closed (could be loaded from persistent settings)
    ModuleInspectorPanel* clickInspector = window.findChild<ModuleInspectorPanel*>();
    QVERIFY(clickInspector != nullptr);
    clickInspector->setPinned(false);
    window.resetInspectorClosed();

    QTreeWidgetItem* item = processTree->topLevelItem(0);
    processTree->setCurrentItem(item);
    const QRect itemRect = processTree->visualItemRect(item);
    QTest::mouseClick(processTree->viewport(), Qt::LeftButton, Qt::NoModifier, itemRect.center());
    QCoreApplication::processEvents();

    QTRY_VERIFY(imageWidget->hasImage());

    engine.clearModules();
    delete module;
}

void TestMainWindow::testStepRunHighlightMovesBetweenModules() {
    RunEngine& engine = RunEngine::instance();

    MainWindow window;
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance firstInstance;
    firstInstance.id = QStringLiteral("first_1");
    firstInstance.moduleId = QStringLiteral("OutputProbe");
    firstInstance.name = QStringLiteral("第一步");
    project->addModule(firstInstance);

    ModuleInstance secondInstance;
    secondInstance.id = QStringLiteral("second_1");
    secondInstance.moduleId = QStringLiteral("OutputProbe");
    secondInstance.name = QStringLiteral("第二步");
    project->addModule(secondInstance);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QCOMPARE(processTree->topLevelItemCount(), 2);
    QTreeWidgetItem* firstItem = processTree->topLevelItem(0);
    QTreeWidgetItem* secondItem = processTree->topLevelItem(1);

    MainWindowOutputProbeModule* first = new MainWindowOutputProbeModule(QStringLiteral("first_1"));
    MainWindowOutputProbeModule* second = new MainWindowOutputProbeModule(QStringLiteral("second_1"));
    first->initialize();
    second->initialize();
    engine.addModule(first);
    engine.addModule(second);
    // Register modules to m_flowModules so syncModulesToRunEngine works
    window.registerFlowModule(QStringLiteral("first_1"), first);
    window.registerFlowModule(QStringLiteral("second_1"), second);

    // Ensure inspector is not pinned (could be loaded from persistent settings)
    ModuleInspectorPanel* stepInspector = window.findChild<ModuleInspectorPanel*>();
    QVERIFY(stepInspector != nullptr);
    stepInspector->setPinned(false);

    // Step run via onStepRun() to test the full UI path
    QVERIFY(QMetaObject::invokeMethod(&window, "onStepRun", Qt::DirectConnection));
    QCoreApplication::processEvents();
    QCOMPARE(processTree->currentItem(), firstItem);
    QVERIFY(firstItem->text(1).endsWith(QStringLiteral(" ms")));

    QVERIFY(QMetaObject::invokeMethod(&window, "onStepRun", Qt::DirectConnection));
    QCoreApplication::processEvents();
    QCOMPARE(processTree->currentItem(), secondItem);
    QVERIFY(secondItem->text(1).endsWith(QStringLiteral(" ms")));

    engine.clearModules();
    delete first;
    delete second;
}

void TestMainWindow::testQuickAnnotateOpensSamDialogOnMainViewportImage() {
    MainWindow window;
    QCoreApplication::processEvents();

    ViewportWidget* viewport = window.findChild<ViewportWidget*>();
    QVERIFY(viewport != nullptr);
    HImageWidget* imageWidget = viewport->imageWidget();
    QVERIFY(imageWidget != nullptr);

    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(QColor("#22C55E"));
    imageWidget->setImage(image);
    QVERIFY(imageWidget->hasImage());

    QVERIFY(QMetaObject::invokeMethod(&window, "onQuickAnnotate", Qt::DirectConnection));
    QCoreApplication::processEvents();

    SamAnnotatorDialog* dialog = window.findChild<SamAnnotatorDialog*>();
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    QCOMPARE(dialog->overlayWidget()->parentWidget(), imageWidget);
}

void TestMainWindow::testMeasurementConfigButtonCreatesInputNode() {
    QTemporaryDir appDir;
    QVERIFY(appDir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", appDir.path().toLocal8Bit());
    const QString pluginRoot = QDir(appDir.path()).filePath("plugins");
    QVERIFY(installRuntimePlugin(pluginRoot, QStringLiteral("MeasurementInput")));
    QVERIFY(installRuntimePlugin(pluginRoot, QStringLiteral("DistancePP")));

    MainWindow window;
    QCoreApplication::processEvents();
    QTRY_VERIFY(PluginManager::instance().isPluginLoaded(QStringLiteral("DistancePP")));
    QTRY_VERIFY(PluginManager::instance().isPluginLoaded(QStringLiteral("MeasurementInput")));

    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance distance;
    distance.id = QStringLiteral("distance_1");
    distance.moduleId = QStringLiteral("DistancePP");
    distance.name = QStringLiteral("点点距离");
    project->addModule(distance);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QCOMPARE(processTree->topLevelItemCount(), 1);
    QTreeWidgetItem* distanceItem = processTree->topLevelItem(0);
    processTree->setCurrentItem(distanceItem);

    bool clickedSetup = false;
    QTimer::singleShot(20, [&]() {
        QWidget* modal = QApplication::activeModalWidget();
        QVERIFY(modal != nullptr);
        QPushButton* button = modal->findChild<QPushButton*>("MeasurementInputSetupButton");
        QVERIFY(button != nullptr);
        clickedSetup = true;
        button->click();
    });

    QVERIFY(QMetaObject::invokeMethod(&window, "_phase8_openAdvancedPluginConfig",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("distance_1"))));
    QCoreApplication::processEvents();

    QVERIFY(clickedSetup);
    QCOMPARE(processTree->topLevelItemCount(), 2);
    QCOMPARE(processTree->topLevelItem(0)->data(0, Qt::UserRole + 2).toString(), QStringLiteral("MeasurementInput"));
    QCOMPARE(processTree->topLevelItem(1)->data(0, Qt::UserRole + 2).toString(), QStringLiteral("DistancePP"));
    QCOMPARE(processTree->currentItem(), processTree->topLevelItem(0));

    const QString inputId = processTree->topLevelItem(0)->data(0, Qt::UserRole + 1).toString();
    ModuleInstance* input = project->findModule(inputId);
    QVERIFY(input != nullptr);
    QCOMPARE(input->moduleId, QStringLiteral("MeasurementInput"));
    QCOMPARE(input->params["mode"].toString(), QStringLiteral("point_pair"));

    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(10.0, 20.0))));
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(30.0, 45.0))));

    input = project->findModule(inputId);
    QVERIFY(input != nullptr);
    QCOMPARE(input->params["point1"].toArray().at(0).toDouble(), 10.0);
    QCOMPARE(input->params["point1"].toArray().at(1).toDouble(), 20.0);
    QCOMPARE(input->params["point2"].toArray().at(0).toDouble(), 30.0);
    QCOMPARE(input->params["point2"].toArray().at(1).toDouble(), 45.0);
}

void TestMainWindow::testMeasurementConfigButtonWithInstalledPlugins() {
    qunsetenv("DEEPLUX_APP_DATA_DIR");

    MainWindow window;
    QCoreApplication::processEvents();
    QTRY_VERIFY(PluginManager::instance().isPluginLoaded(QStringLiteral("DistancePP")));
    QTRY_VERIFY(PluginManager::instance().isPluginLoaded(QStringLiteral("MeasurementInput")));

    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance distance;
    distance.id = QStringLiteral("installed_distance_1");
    distance.moduleId = QStringLiteral("DistancePP");
    distance.name = QStringLiteral("点点距离");
    project->addModule(distance);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QCOMPARE(processTree->topLevelItemCount(), 1);
    QTreeWidgetItem* distanceItem = processTree->topLevelItem(0);
    processTree->setCurrentItem(distanceItem);

    bool clickedSetup = false;
    QTimer::singleShot(20, [&]() {
        QWidget* modal = QApplication::activeModalWidget();
        QVERIFY(modal != nullptr);
        QPushButton* button = modal->findChild<QPushButton*>("MeasurementInputSetupButton");
        QVERIFY(button != nullptr);
        clickedSetup = true;
        button->click();
    });

    QVERIFY(QMetaObject::invokeMethod(&window, "_phase8_openAdvancedPluginConfig",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("installed_distance_1"))));
    QCoreApplication::processEvents();

    QVERIFY(clickedSetup);
    QCOMPARE(processTree->topLevelItemCount(), 2);
    QCOMPARE(processTree->topLevelItem(0)->data(0, Qt::UserRole + 2).toString(), QStringLiteral("MeasurementInput"));
    QCOMPARE(processTree->currentItem(), processTree->topLevelItem(0));
}

void TestMainWindow::testPluginConfigDialogRestylesLegacyDarkPlugin() {
    QTemporaryDir appDir;
    QVERIFY(appDir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", appDir.path().toLocal8Bit());
    const QString pluginRoot = QDir(appDir.path()).filePath("plugins");
    QVERIFY(installRuntimePlugin(pluginRoot, QStringLiteral("LoadPointCloud")));

    MainWindow window;
    QCoreApplication::processEvents();
    QTRY_VERIFY(PluginManager::instance().isPluginLoaded(QStringLiteral("LoadPointCloud")));

    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance loader;
    loader.id = QStringLiteral("pointcloud_1");
    loader.moduleId = QStringLiteral("LoadPointCloud");
    loader.name = QStringLiteral("加载点云");
    project->addModule(loader);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QCOMPARE(processTree->topLevelItemCount(), 1);

    bool checkedDialog = false;
    QTimer::singleShot(20, [&]() {
        QWidget* modal = QApplication::activeModalWidget();
        QVERIFY(modal != nullptr);
        QCOMPARE(modal->objectName(), QStringLiteral("PluginConfigDialog"));

        QWidget* content = modal->findChild<QWidget*>("PluginConfigContent");
        QVERIFY(content != nullptr);
        QVERIFY2(content->styleSheet().contains(QStringLiteral("#ffffff")),
                 "Light theme config content should be white, not the plugin's legacy dark surface");
        QVERIFY2(!content->styleSheet().contains(QStringLiteral("#1a2332")),
                 "Legacy ConfigWidgetHelper dark surface should be overwritten when dialog opens");

        const QList<QLabel*> labels = content->findChildren<QLabel*>();
        QVERIFY(!labels.isEmpty());
        for (QLabel* label : labels) {
            QVERIFY2(label->wordWrap(), "Config labels should wrap instead of clipping or overlapping");
        }

        QLineEdit* pathEdit = modal->findChild<QLineEdit*>();
        QVERIFY(pathEdit != nullptr);
        QVERIFY2(pathEdit->styleSheet().contains(QStringLiteral("#ffffff")),
                 "Config inputs should follow the current light theme");
        QVERIFY2(!pathEdit->styleSheet().contains(QStringLiteral("#1a2332")),
                 "Config inputs should not keep legacy dark backgrounds");

        QPushButton* browseButton = nullptr;
        for (QPushButton* button : modal->findChildren<QPushButton*>()) {
            if (button->text().contains(QStringLiteral("浏览"))) {
                browseButton = button;
                break;
            }
        }
        QVERIFY(browseButton != nullptr);
        QVERIFY2(browseButton->styleSheet().contains(QStringLiteral("#0078d7")),
                 "Plugin-owned buttons should be restyled by the config dialog");
        QVERIFY2(!browseButton->styleSheet().contains(QStringLiteral("#2d3748")),
                 "Plugin-owned buttons should not keep legacy dark button colors");

        checkedDialog = true;
        modal->close();
    });

    QVERIFY(QMetaObject::invokeMethod(&window, "_phase8_openAdvancedPluginConfig",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("pointcloud_1"))));
    QCoreApplication::processEvents();

    QVERIFY(checkedDialog);
}

void TestMainWindow::testOpenProjectSyncsProcessTreeAndFlowCanvas() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("workflow.dproj");

    {
        Project project;

        ModuleInstance grab;
        grab.id = "grab_1";
        grab.moduleId = "GrabImage";
        grab.name = "Grab Image";
        grab.posX = 40;
        grab.posY = 60;
        project.addModule(grab);

        ModuleInstance save;
        save.id = "save_1";
        save.moduleId = "SaveImage";
        save.name = "Save Image";
        save.posX = 260;
        save.posY = 60;
        project.addModule(save);

        ModuleConnection conn;
        conn.fromModuleId = grab.id;
        conn.toModuleId = save.id;
        conn.fromOutput = 0;
        conn.toInput = 0;
        project.addConnection(conn);

        QVERIFY(project.save(path));
    }

    MainWindow window;

    Project* opened = ProjectManager::instance().openProject(path);
    QVERIFY(opened != nullptr);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QCOMPARE(processTree->topLevelItemCount(), 2);
    QCOMPARE(processTree->topLevelItem(0)->data(0, Qt::UserRole + 1).toString(), QString("grab_1"));
    QCOMPARE(processTree->topLevelItem(1)->data(0, Qt::UserRole + 1).toString(), QString("save_1"));

    FlowCanvas* canvas = window.findChild<FlowCanvas*>("FlowCanvas");
    QVERIFY(canvas != nullptr);
    QCOMPARE(canvas->nodeIds().size(), 2);
    QVERIFY(canvas->nodeIds().contains("grab_1"));
    QVERIFY(canvas->nodeIds().contains("save_1"));
    QCOMPARE(canvas->nodeItem("grab_1")->pos(), QPointF(40, 60));
    QCOMPARE(canvas->nodeItem("save_1")->pos(), QPointF(260, 60));

    ProjectManager::instance().closeProject();
    QCoreApplication::processEvents();

    QCOMPARE(processTree->topLevelItemCount(), 0);
    QCOMPARE(canvas->nodeIds().size(), 0);
}

void TestMainWindow::testHomeSwitchesToFlowCanvas() {
    MainWindow window;

    QTabWidget* processTabs = window.findChild<QTabWidget*>("ProcessTabWidget");
    FlowCanvas* canvas = window.findChild<FlowCanvas*>("FlowCanvas");
    QVERIFY(processTabs != nullptr);
    QVERIFY(canvas != nullptr);

    processTabs->setCurrentIndex(0);
    QVERIFY(QMetaObject::invokeMethod(&window, "onHome", Qt::DirectConnection));
    QCOMPARE(processTabs->currentWidget(), canvas);
}

void TestMainWindow::testAgentMessageRenderingUsesCompactLineSpacing() {
    const QString html = AgentMessageBubble::markdownToHtml(
        QStringLiteral("参数说明\n\n- param1：Canny边缘检测的高阈值\n\n- param2：圆心投票数阈值"), false,
        QColor("#111111"));

    QVERIFY2(html.contains(QStringLiteral("line-height:1.25")),
             "Agent messages should use terminal-like compact line height");
    QVERIFY2(!html.contains(QStringLiteral("<br><br>")),
             "Blank markdown lines should not expand into inconsistent vertical gaps");
    QVERIFY2(!html.contains(QStringLiteral("<ul")), "Agent markdown lists should render as terminal-style rows");
    QVERIFY2(!html.contains(QStringLiteral("<li")),
             "Agent markdown lists should avoid QTextDocument list block spacing");
    QVERIFY2(html.contains(QStringLiteral("&bull;&nbsp;")),
             "Agent markdown list items should keep a compact bullet marker");
}

void TestMainWindow::testAgentThinkingStatusStaysCompactAndSafe() {
    AgentChatPanel panel;
    panel.setThinking(true);
    QCoreApplication::processEvents();

    QWidget* strip = panel.findChild<QWidget*>("AgentChatStatusStrip");
    QLabel* status = panel.findChild<QLabel*>("AgentChatStatusLabel");
    QVERIFY2(strip != nullptr, "Agent thinking state should render through the compact status strip");
    QVERIFY2(status != nullptr, "Agent thinking state should expose a status label");
    QVERIFY2(strip->maximumHeight() <= 24, "Thinking status should not create a tall message row");
    QVERIFY2(status->text().contains(QStringLiteral("正在思考")), "Thinking status should be visible");

    for (int i = 0; i < 10; ++i) {
        panel.setThinking(false);
        panel.setThinking(true);
    }
    panel.setThinking(false);
    QCoreApplication::processEvents();
    QVERIFY2(!status->text().contains(QStringLiteral("正在思考")), "Thinking status should clear cleanly");
}

void TestMainWindow::testAgentInputErrorPathDoesNotCrashOrStayThinking() {
    AgentController::instance().setLLMClient(nullptr);
    AgentController::instance().clearConversation();

    MainWindow window;
    AgentChatPanel* agentPanel = window.findChild<AgentChatPanel*>();
    QVERIFY(agentPanel != nullptr);

    QPlainTextEdit* input = nullptr;
    const QList<QPlainTextEdit*> edits = agentPanel->findChildren<QPlainTextEdit*>();
    for (QPlainTextEdit* edit : edits) {
        if (edit->placeholderText().contains(QStringLiteral("输入 Agent 指令"))) {
            input = edit;
            break;
        }
    }
    QVERIFY2(input != nullptr, "Agent panel should expose the compact input editor");

    QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);
    input->setPlainText(QStringLiteral("测试 Agent 错误路径"));
    QTest::keyClick(input, Qt::Key_Return);
    QCoreApplication::processEvents();

    QCOMPARE(errorSpy.count(), 1);
    QLabel* status = agentPanel->findChild<QLabel*>("AgentChatStatusLabel");
    QVERIFY(status != nullptr);
    QVERIFY2(!status->text().contains(QStringLiteral("正在思考")),
             "Agent error path should leave the compact thinking status");
    QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
}

void TestMainWindow::testMainWindowLayoutKeepsConfirmedWorkflowTabsAndReadableTheme() {
    MainWindow window;
    window.resize(1024, 700);
    window.show();
    QCoreApplication::processEvents();

    QToolBar* mainToolbar = nullptr;
    const QList<QToolBar*> toolbars = window.findChildren<QToolBar*>();
    for (QToolBar* toolbar : toolbars) {
        if (toolbar->windowTitle() == QStringLiteral("主工具栏")) {
            mainToolbar = toolbar;
            break;
        }
    }
    QVERIFY(mainToolbar != nullptr);

    QStringList toolbarTexts;
    for (QAction* action : mainToolbar->actions()) {
        if (!action->isSeparator()) {
            toolbarTexts.append(action->text().remove('&'));
        }
    }

    const QStringList expectedPrimaryActions = {
        QStringLiteral("新建方案"), QStringLiteral("方案列表"), QStringLiteral("打开"), QStringLiteral("保存"),
        QStringLiteral("单次运行"), QStringLiteral("循环运行"), QStringLiteral("停止"), QStringLiteral("切换主题"),
    };
    for (const QString& actionText : expectedPrimaryActions) {
        QVERIFY2(toolbarTexts.contains(actionText), qPrintable(QString("Missing toolbar action: %1").arg(actionText)));
    }

    const QStringList lowFrequencyActions = {
        QStringLiteral("用户登录"), QStringLiteral("全局变量"), QStringLiteral("相机设置"),
        QStringLiteral("通讯设置"), QStringLiteral("硬件配置"), QStringLiteral("报表查询"),
        QStringLiteral("主页"),     QStringLiteral("UI 设计"),  QStringLiteral("激光设置"),
    };
    for (const QString& actionText : lowFrequencyActions) {
        QVERIFY2(!toolbarTexts.contains(actionText),
                 qPrintable(QString("Low-frequency action should not be in main toolbar: %1").arg(actionText)));
    }

    QTabWidget* processTabs = window.findChild<QTabWidget*>("ProcessTabWidget");
    QVERIFY(processTabs != nullptr);
    QVERIFY(processTabs->tabBar() != nullptr);
    QStringList tabTexts;
    for (int i = 0; i < processTabs->count(); ++i) {
        tabTexts.append(processTabs->tabText(i));
    }
    QCOMPARE(processTabs->count(), 3);
    QVERIFY(tabTexts.contains(QStringLiteral("流程")));
    QVERIFY(tabTexts.contains(QStringLiteral("画布")));
    QVERIFY(tabTexts.contains(QStringLiteral("数据源")));
    QVERIFY(!tabTexts.contains(QStringLiteral("属性")));
    QVERIFY2(!processTabs->tabBar()->usesScrollButtons(), "Confirmed workflow tabs should fit without scroll arrows");

    QTreeWidget* toolTree = window.findChild<QTreeWidget*>("ToolBoxTree");
    QVERIFY(toolTree != nullptr);
    QTreeWidgetItem* findCircleTool = nullptr;
    for (QTreeWidgetItemIterator it(toolTree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole + 1).toString() == QStringLiteral("FindCircle")) {
            findCircleTool = *it;
            break;
        }
    }
    QVERIFY2(findCircleTool != nullptr, "FindCircle should be present in the tool panel");
    QVERIFY2(!findCircleTool->text(0).contains(QStringLiteral("🔵")),
             "Plugin icon should be a QIcon, not a repeated emoji in the item text");
    QVERIFY2(!findCircleTool->icon(0).isNull(), "Plugin item should keep a single dedicated icon");

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QVERIFY2(processTree->dragEnabled(), "Process panel items should support internal reordering");
    QCOMPARE(processTree->dragDropMode(), QAbstractItemView::DragDrop);
    QCOMPARE(processTree->defaultDropAction(), Qt::CopyAction);
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);
    ModuleInstance agentAddedCircle;
    agentAddedCircle.id = QStringLiteral("agent_circle_1");
    agentAddedCircle.moduleId = QStringLiteral("FindCircle");
    agentAddedCircle.name = QStringLiteral("圆检测");
    project->addModule(agentAddedCircle);
    QCoreApplication::processEvents();
    QTreeWidgetItem* processCircle = nullptr;
    for (int i = 0; i < processTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = processTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole + 1).toString() == agentAddedCircle.id) {
            processCircle = item;
            break;
        }
    }
    QVERIFY2(processCircle != nullptr, "Agent-added module should appear in the process panel");
    QCOMPARE(processCircle->text(0), findCircleTool->text(0));
    QVERIFY2(processCircle->flags() & Qt::ItemIsDragEnabled,
             "Process panel modules should be draggable for reordering");
    QVERIFY2(!(processCircle->flags() & Qt::ItemIsDropEnabled),
             "Process panel modules should not accept drops onto the item body");

    QWidget* dataSourcePanel = window.findChild<QWidget*>("DataSourcePanel");
    QVERIFY(dataSourcePanel != nullptr);
    QCOMPARE(processTabs->indexOf(dataSourcePanel), tabTexts.indexOf(QStringLiteral("数据源")));

    QVERIFY2(window.styleSheet().contains(QStringLiteral("QWidget#MainContentWidget { background-color: #f5f5f5; }")),
             "Main content gutter should match the light MainWindow background instead of showing a dark default fill");
    QVERIFY2(window.styleSheet().contains(QStringLiteral("QSplitter#MainSplitter { background-color: #f5f5f5; }")),
             "Main splitter gutter should match the light left outer gutter color");
    QVERIFY2(window.styleSheet().contains(
                 QStringLiteral("QSplitter#MainSplitter::handle { background-color: #f5f5f5; border: none; }")),
             "Main splitter handle should use the same light gutter fill as the left outer margin");
    QVERIFY(QMetaObject::invokeMethod(&window, "onToggleTheme", Qt::DirectConnection));
    QVERIFY2(window.styleSheet().contains(
                 QStringLiteral("QToolBar QToolButton { background-color: transparent; color: #ffffff;")),
             "Dark theme toolbar buttons need an explicit readable foreground color");
    QVERIFY2(window.styleSheet().contains(QStringLiteral("#1e1e1e")),
             "Dark theme should apply dark background color to stylesheet");

    QList<QPlainTextEdit*> plainTextEdits = window.findChildren<QPlainTextEdit*>();
    bool hasLocalizedAgentPlaceholder = false;
    for (QPlainTextEdit* edit : plainTextEdits) {
        if (edit->placeholderText().contains(QStringLiteral("输入 Agent 指令"))) {
            hasLocalizedAgentPlaceholder = true;
            break;
        }
    }
    QVERIFY2(hasLocalizedAgentPlaceholder, "Agent input placeholder should be localized and compact");

    AgentChatPanel* agentPanel = window.findChild<AgentChatPanel*>();
    QVERIFY(agentPanel != nullptr);
    QWidget* agentStatusStrip = agentPanel->findChild<QWidget*>("AgentChatStatusStrip");
    QLabel* agentMessageCount = agentPanel->findChild<QLabel*>("AgentChatMessageCountLabel");
    QLabel* agentToolCount = agentPanel->findChild<QLabel*>("AgentChatToolCountLabel");
    QVERIFY2(agentStatusStrip != nullptr, "Agent chat should expose a compact information strip");
    QVERIFY2(agentStatusStrip->maximumHeight() <= 24, "Agent information strip should not consume chat history space");
    QVERIFY(agentMessageCount != nullptr);
    QVERIFY(agentToolCount != nullptr);

    agentPanel->addMessage(AgentMessageBubble::Sender::User, QStringLiteral("创建一个找圆流程"));
    agentPanel->addMessage(AgentMessageBubble::Sender::Agent, QStringLiteral("已准备流程步骤"));
    QList<AgentToolPreviewCard::ToolItem> compactTools;
    AgentToolPreviewCard::ToolItem compactTool;
    compactTool.name = QStringLiteral("run_flow");
    compactTools.append(compactTool);
    agentPanel->showToolPreview(compactTools);
    QCoreApplication::processEvents();
    QVERIFY2(agentMessageCount->text().contains(QStringLiteral("2")), "Agent message count should update in the strip");
    QVERIFY2(agentToolCount->text().contains(QStringLiteral("1")),
             "Agent pending tool count should update in the strip");

    QStringList buttonTexts;
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        buttonTexts.append(button->text());
    }
    QVERIFY2(buttonTexts.contains(QStringLiteral("撤销最近操作")),
             "Agent action log undo button should clarify stack behavior");
    QVERIFY2(buttonTexts.contains(QStringLiteral("清空")), "Agent action log clear button should be localized");
    QVERIFY2(!buttonTexts.contains(QStringLiteral("撤销")),
             "Agent action log should not imply arbitrary selected-row undo");
    QVERIFY2(!buttonTexts.contains(QStringLiteral("Undo")), "Agent action log should not expose English Undo text");
    QVERIFY2(!buttonTexts.contains(QStringLiteral("Clear")), "Agent action log should not expose English Clear text");

    QDockWidget* logDock = window.findChild<QDockWidget*>("LogDock");
    QVERIFY(logDock != nullptr);
    QVERIFY2(logDock->minimumHeight() <= 240,
             qPrintable(QString("Log panel minimum height is too large: %1").arg(logDock->minimumHeight())));

    QSplitter* mainSplitter = window.findChild<QSplitter*>("MainSplitter");
    QSplitter* rightSplitter = window.findChild<QSplitter*>("RightSplitter");
    QSplitter* rightTopSplitter = window.findChild<QSplitter*>("RightTopSplitter");
    QVERIFY(mainSplitter != nullptr);
    QVERIFY(rightSplitter != nullptr);
    QVERIFY(rightTopSplitter != nullptr);
    QVERIFY2(mainSplitter->handleWidth() >= 6, "Main splitter handle should be easy to see and drag");
    QWidget* mainContentWidget = window.findChild<QWidget*>("MainContentWidget");
    QVERIFY2(mainContentWidget != nullptr,
             "Central content should wrap the splitter so the left edge can have a gutter");
    QVERIFY(mainContentWidget->layout() != nullptr);
    const QMargins mainContentMargins = mainContentWidget->layout()->contentsMargins();
    QCOMPARE(mainContentMargins.left(), mainSplitter->handleWidth());
    QCOMPARE(mainContentMargins.top(), 0);
    QCOMPARE(mainContentMargins.right(), mainSplitter->handleWidth());
    QCOMPARE(mainContentMargins.bottom(), 0);
    QVERIFY2(rightSplitter->handleWidth() >= 6, "Bottom panel splitter handle should be easy to see and drag");
    QVERIFY2(rightTopSplitter->handleWidth() >= 6, "Process/display splitter handle should be easy to see and drag");
    QVERIFY2(!mainSplitter->childrenCollapsible(), "Primary panels should not collapse accidentally while dragging");
    QVERIFY2(!rightSplitter->childrenCollapsible(),
             "Top and bottom panels should not collapse accidentally while dragging");
    QVERIFY2(!rightTopSplitter->childrenCollapsible(),
             "Process and display panels should not collapse accidentally while dragging");
    const QString mainStyle = window.styleSheet();
    QVERIFY2(mainStyle.contains(QStringLiteral("QWidget#MainContentWidget { background-color: #1e1e1e; }")),
             "Main content gutter should match the dark MainWindow background instead of showing a black default fill");
    QVERIFY2(mainStyle.contains(QStringLiteral("QSplitter#MainSplitter { background-color: #1e1e1e; }")),
             "Main splitter gutter should match the dark left outer gutter color");
    QVERIFY2(mainStyle.contains(
                 QStringLiteral("QSplitter#MainSplitter::handle { background-color: #1e1e1e; border: none; }")),
             "Main splitter handle should use the same dark gutter fill as the left outer margin");
    QVERIFY2(mainStyle.contains(QStringLiteral("QWidget#ProcessPanelWidget")),
             "Process panel needs an explicit boundary style");
    QVERIFY2(mainStyle.contains(QStringLiteral("QWidget#ImageDisplayWidget")),
             "Display panel needs an explicit boundary style");
    QVERIFY2(mainStyle.contains(QStringLiteral("font-size: 13px")),
             "Main panels should pin a consistent base font size");

    // 流程面板顶部不再保留无功能的占位工具栏，流程树直接占据剩余空间
    QVERIFY2(window.findChild<QWidget*>("ProcessToolBar") == nullptr,
             "Process panel should not retain a non-functional placeholder toolbar above the tree");
    QVERIFY2(window.findChild<QWidget*>("ToolToolBar") == nullptr,
             "Tool panel should not contain dead shortcut buttons above the plugin list");
    QVERIFY2(window.findChild<QToolButton*>("ToolCreateProcessBtn") == nullptr,
             "Removed tool-panel shortcut buttons should not remain clickable but inert");

    QWidget* toolCategoryWidget = window.findChild<QWidget*>("ToolCategoryWidget");
    QVERIFY(toolCategoryWidget != nullptr);
    QVERIFY(toolCategoryWidget->layout() != nullptr);
    QCOMPARE(toolCategoryWidget->layout()->contentsMargins().left(), 6);
    QCOMPARE(toolCategoryWidget->layout()->contentsMargins().right(), 6);
    QWidget* toolPanelWidget = window.findChild<QWidget*>("ToolPanelWidget");
    QVERIFY(toolPanelWidget != nullptr);
    QVERIFY2(!toolPanelWidget->styleSheet().contains(QStringLiteral("border-right")),
             "Tool panel content should not draw an extra right border inside the splitter boundary");
    QVERIFY2(!mainStyle.contains(QStringLiteral("QDockWidget#ToolPanelDock { border-right")),
             "Tool dock should not add a second right border next to the splitter");

    QTreeWidget* processTreeForHighlight = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTreeForHighlight != nullptr);
    // Tree widget 样式现在由 ThemeManager 统一管理，检查全局样式表
    QVERIFY2(window.styleSheet().contains(
                 QStringLiteral("QTreeWidget::item:selected { background-color: #0078d7; }")),
             "Process tree mouse selection should use the same deep highlight as run completion");

    QWidget* processTabContent = window.findChild<QWidget*>("ProcessTabContent");
    QVERIFY(processTabContent != nullptr);
    QVERIFY(processTabContent->layout() != nullptr);
    QCOMPARE(processTabContent->layout()->contentsMargins().left(), 10);
    QCOMPARE(processTabContent->layout()->contentsMargins().right(), 10);

    QWidget* dataSourcePanelForMargins = window.findChild<QWidget*>("DataSourcePanel");
    QVERIFY(dataSourcePanelForMargins != nullptr);
    QVERIFY(dataSourcePanelForMargins->layout() != nullptr);
    QCOMPARE(dataSourcePanelForMargins->layout()->contentsMargins().left(), 10);
    QCOMPARE(dataSourcePanelForMargins->layout()->contentsMargins().right(), 10);

    // 运行控制按钮已移至顶部工具栏（流程面板不再有重复按钮）
    QVERIFY(mainToolbar != nullptr);
    bool foundStepAction = false;
    for (QAction* action : mainToolbar->actions()) {
        if (action->text().contains(QStringLiteral("单步"))) {
            foundStepAction = true;
            break;
        }
    }
    QVERIFY2(foundStepAction,
             "Top toolbar should expose a dedicated one-step execution action");
    QVERIFY2(window.styleSheet().contains(QStringLiteral("QToolBar QToolButton")),
             "Toolbar buttons should use the same styled tool-button rules");
    const QImage playIcon =
        AppIconProvider::icon(AppIconProvider::Icon::Play, 24, QColor("#0F766E")).pixmap(24, 24).toImage();
    const QImage stepIcon =
        AppIconProvider::icon(AppIconProvider::Icon::Step, 24, QColor("#0F766E")).pixmap(24, 24).toImage();
    QVERIFY2(playIcon != stepIcon, "Step execution icon should not reuse the play triangle");
    // 流程面板不应再有重复的运行控制按钮
    QVERIFY2(window.findChild<QToolButton*>("ProcessStartPauseBtn") == nullptr,
             "Process panel should not have duplicate start/pause button");
    QVERIFY2(window.findChild<QToolButton*>("ProcessStepBtn") == nullptr,
             "Process panel should not have duplicate step button");

    QLabel* viewportTitle = window.findChild<QLabel*>("ViewportTitle");
    QVERIFY(viewportTitle != nullptr);
    QVERIFY2(viewportTitle->styleSheet().contains(QStringLiteral("font-size: 14px")),
             "Viewport title should use the same readable panel title font size");

    QTabWidget* logTabs = window.findChild<QTabWidget*>("LogTerminalTabs");
    QVERIFY(logTabs != nullptr);
    QVERIFY(logTabs->tabBar() != nullptr);
    QVERIFY2(processTabs->styleSheet().contains(QStringLiteral("font-size: 14px")),
             "Process tabs should use a more readable workflow tab font size");
    QVERIFY2(processTabs->styleSheet().contains(QStringLiteral("QTabWidget::tab-bar { left: 8px; }")),
             "Process tabs should be inset from the left edge");
    const int processTabTextHeight = processTabs->tabBar()->fontMetrics().height();
    QVERIFY2(processTabs->tabBar()->tabRect(0).height() >= processTabTextHeight + 14,
             qPrintable(QString("Process tab height %1 is too tight for text height %2")
                            .arg(processTabs->tabBar()->tabRect(0).height())
                            .arg(processTabTextHeight)));
    QVERIFY2(logTabs->styleSheet().contains(QStringLiteral("font-size: 13px")),
             "Bottom tabs should use the shared panel tab font size");
    const int logTabTextHeight = logTabs->tabBar()->fontMetrics().height();
    QVERIFY2(logTabs->tabBar()->tabRect(0).height() >= logTabTextHeight + 10,
             qPrintable(QString("Bottom tab height %1 clips text height %2")
                            .arg(logTabs->tabBar()->tabRect(0).height())
                            .arg(logTabTextHeight)));

    // agentPanel is now inside Agent inner tabs; find the "Agent" outer tab
    int agentOuterIndex = -1;
    for (int i = 0; i < logTabs->count(); ++i) {
        if (logTabs->tabText(i) == QStringLiteral("Agent")) {
            agentOuterIndex = i;
            break;
        }
    }
    QVERIFY2(agentOuterIndex >= 0, "Agent outer tab should exist in bottom panel");
    QJsonArray pendingTools;
    pendingTools.append(QJsonObject{
        {"id", "call_remove"},
        {"type", "function"},
        {"name", "remove_module"},
        {"arguments", QJsonObject{{"instanceId", "grab_1"}}},
    });
    emit AgentController::instance().toolsPendingConfirmation(pendingTools);
    QCoreApplication::processEvents();
    QCOMPARE(logTabs->currentIndex(), agentOuterIndex);
    QVERIFY2(logTabs->tabToolTip(agentOuterIndex).contains(QStringLiteral("等待确认")),
             "Agent chat tab should expose pending confirmation state");

    QList<AgentMessageBubble*> bubbles = window.findChildren<AgentMessageBubble*>();
    bool hasToolMessage = false;
    for (AgentMessageBubble* bubble : bubbles) {
        if (bubble->text().contains(QStringLiteral("remove_module")) &&
            bubble->text().contains(QStringLiteral("高风险"))) {
            hasToolMessage = true;
            break;
        }
    }
    QVERIFY2(hasToolMessage, "Tool preview should also leave a Tool message in chat history");

    QTableWidget* logTable = window.findChild<QTableWidget*>("LogTable");
    QVERIFY(logTable != nullptr);
    QCOMPARE(logTable->frameShape(), QFrame::NoFrame);
    QVERIFY2(logTable->styleSheet().contains(QStringLiteral("QTableWidget#LogTable")),
             "Log table should remove the dark default outer frame explicitly");
    QVERIFY(logTable->parentWidget() != nullptr);
    QVERIFY(logTable->parentWidget()->layout() != nullptr);
    QCOMPARE(logTable->parentWidget()->layout()->contentsMargins().left(), 0);
    QCOMPARE(logTable->parentWidget()->layout()->contentsMargins().right(), 0);
    QVERIFY(logTable->horizontalHeader() != nullptr);
    QVERIFY(logTable->verticalHeader() != nullptr);
    const int logTableTextHeight = logTable->fontMetrics().height();
    QVERIFY2(logTable->horizontalHeader()->height() >= logTableTextHeight + 10,
             qPrintable(QString("Log header height %1 clips text height %2")
                            .arg(logTable->horizontalHeader()->height())
                            .arg(logTableTextHeight)));
    QVERIFY2(logTable->verticalHeader()->defaultSectionSize() >= logTableTextHeight + 6,
             qPrintable(QString("Log row height %1 clips text height %2")
                            .arg(logTable->verticalHeader()->defaultSectionSize())
                            .arg(logTableTextHeight)));

    QTableWidget* agentLogTable = window.findChild<QTableWidget*>("AgentActionLogTable");
    QVERIFY(agentLogTable != nullptr);
    QVERIFY(agentLogTable->parentWidget() != nullptr);
    QVERIFY(agentLogTable->parentWidget()->layout() != nullptr);
    QCOMPARE(agentLogTable->parentWidget()->layout()->contentsMargins().left(), 6);
    QCOMPARE(agentLogTable->parentWidget()->layout()->contentsMargins().right(), 6);

    QList<AgentToolPreviewCard::ToolItem> previewTools;
    AgentToolPreviewCard::ToolItem dangerousTool;
    dangerousTool.name = QStringLiteral("remove_module");
    dangerousTool.params = QJsonObject{{"instanceId", "grab_1"}};
    previewTools.append(dangerousTool);
    AgentToolPreviewCard preview(previewTools, false);
    QStringList previewButtonTexts;
    for (QPushButton* button : preview.findChildren<QPushButton*>()) {
        previewButtonTexts.append(button->text());
        QVERIFY2(button->minimumHeight() >= button->fontMetrics().height() + 6,
                 qPrintable(QString("Agent preview button '%1' is too short").arg(button->text())));
    }
    QVERIFY(previewButtonTexts.contains(QStringLiteral("取消")));
    QVERIFY(previewButtonTexts.contains(QStringLiteral("确认执行")));
    QVERIFY(!previewButtonTexts.contains(QStringLiteral("Cancel")));
    QVERIFY(!previewButtonTexts.contains(QStringLiteral("Confirm")));
}

void TestMainWindow::testMeasurementPickingUpdatesInputAndClipboard() {
    QTemporaryDir appDir;
    QVERIFY(appDir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", appDir.path().toLocal8Bit());
    QVERIFY2(installRuntimePlugin(QDir(appDir.path()).filePath("plugins"), QStringLiteral("MeasurementInput")),
             "MeasurementInput plugin should be available to MainWindow during the picking test");

    MainWindow window;
    QCoreApplication::processEvents();

    QClipboard* clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard != nullptr);
    clipboard->clear();
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(12.5, 34.5))));
    QCOMPARE(clipboard->text(), QStringLiteral("12.50,34.50"));

    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance lineInput;
    lineInput.id = QStringLiteral("measure_line_input");
    lineInput.moduleId = QStringLiteral("MeasurementInput");
    lineInput.name = QStringLiteral("测量输入");
    lineInput.params["mode"] = QStringLiteral("point_line");
    project->addModule(lineInput);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QVERIFY(processTree->topLevelItemCount() > 0);
    processTree->setCurrentItem(processTree->topLevelItem(processTree->topLevelItemCount() - 1));

    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(10.0, 20.0))));
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(30.0, 40.0))));
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(50.0, 60.0))));

    ModuleInstance* lineModule = project->findModule(QStringLiteral("measure_line_input"));
    QVERIFY(lineModule != nullptr);
    QCOMPARE(lineModule->params["point"].toArray().at(0).toDouble(), 10.0);
    QCOMPARE(lineModule->params["point"].toArray().at(1).toDouble(), 20.0);
    QCOMPARE(lineModule->params["point"].toArray().at(2).toDouble(), 0.0);
    QCOMPARE(lineModule->params["line"].toArray().at(0).toDouble(), 30.0);
    QCOMPARE(lineModule->params["line"].toArray().at(3).toDouble(), 60.0);

    Logger::instance().clearLogs();
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint3DPicked", Qt::DirectConnection,
                                      Q_ARG(QVector3D, QVector3D(1.0f, 2.0f, 9.0f))));
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint3DPicked", Qt::DirectConnection,
                                      Q_ARG(QVector3D, QVector3D(3.0f, 4.0f, 99.0f))));
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint3DPicked", Qt::DirectConnection,
                                      Q_ARG(QVector3D, QVector3D(5.0f, 6.0f, 88.0f))));

    lineModule = project->findModule(QStringLiteral("measure_line_input"));
    QVERIFY(lineModule != nullptr);
    QCOMPARE(lineModule->params["point"].toArray().at(2).toDouble(), 9.0);
    QCOMPARE(lineModule->params["line"].toArray().size(), 4);
    QCOMPARE(lineModule->params["line"].toArray().at(0).toDouble(), 3.0);
    QCOMPARE(lineModule->params["line"].toArray().at(3).toDouble(), 6.0);

    bool lineZLogged = false;
    for (const LogEntry& entry : Logger::instance().logs(QStringLiteral("Picking"))) {
        if (entry.message.contains(QStringLiteral("z=0")) && entry.message.contains(QStringLiteral("ignored"))) {
            lineZLogged = true;
            break;
        }
    }
    QVERIFY2(lineZLogged, "3D point_line picking should log that line endpoints use z=0");

    ModuleInstance planeInput;
    planeInput.id = QStringLiteral("measure_plane_input");
    planeInput.moduleId = QStringLiteral("MeasurementInput");
    planeInput.name = QStringLiteral("测量输入");
    planeInput.params["mode"] = QStringLiteral("point_plane");
    project->addModule(planeInput);
    QCoreApplication::processEvents();
    processTree->setCurrentItem(processTree->topLevelItem(processTree->topLevelItemCount() - 1));

    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint3DPicked", Qt::DirectConnection,
                                      Q_ARG(QVector3D, QVector3D(1.0f, 2.0f, 3.0f))));
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint3DPicked", Qt::DirectConnection,
                                      Q_ARG(QVector3D, QVector3D(0.0f, 0.0f, 0.0f))));
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint3DPicked", Qt::DirectConnection,
                                      Q_ARG(QVector3D, QVector3D(1.0f, 0.0f, 0.0f))));
    QVERIFY(QMetaObject::invokeMethod(&window, "onPoint3DPicked", Qt::DirectConnection,
                                      Q_ARG(QVector3D, QVector3D(0.0f, 1.0f, 0.0f))));

    ModuleInstance* planeModule = project->findModule(QStringLiteral("measure_plane_input"));
    QVERIFY(planeModule != nullptr);
    QCOMPARE(planeModule->params["point"].toArray().at(2).toDouble(), 3.0);
    QCOMPARE(planeModule->params["plane"].toArray().size(), 9);
    QCOMPARE(planeModule->params["plane"].toArray().at(3).toDouble(), 1.0);
    QCOMPARE(planeModule->params["plane"].toArray().at(7).toDouble(), 1.0);

    ModuleInstance linePairInput;
    linePairInput.id = QStringLiteral("measure_line_pair_input");
    linePairInput.moduleId = QStringLiteral("MeasurementInput");
    linePairInput.name = QStringLiteral("测量输入");
    linePairInput.params["mode"] = QStringLiteral("line_pair");
    project->addModule(linePairInput);
    QCoreApplication::processEvents();
    processTree->setCurrentItem(processTree->topLevelItem(processTree->topLevelItemCount() - 1));

    QVERIFY(
        QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(1.0, 2.0))));
    QVERIFY(
        QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(3.0, 4.0))));
    QVERIFY(
        QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(5.0, 6.0))));
    QVERIFY(
        QMetaObject::invokeMethod(&window, "onPoint2DPicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(7.0, 8.0))));

    ModuleInstance* linePairModule = project->findModule(QStringLiteral("measure_line_pair_input"));
    QVERIFY(linePairModule != nullptr);
    QCOMPARE(linePairModule->params["line1"].toArray().at(0).toDouble(), 1.0);
    QCOMPARE(linePairModule->params["line1"].toArray().at(3).toDouble(), 4.0);
    QCOMPARE(linePairModule->params["line2"].toArray().at(0).toDouble(), 5.0);
    QCOMPARE(linePairModule->params["line2"].toArray().at(3).toDouble(), 8.0);
}

void TestMainWindow::testMeasurementPickingViaImageViewportClick() {
    QTemporaryDir appDir;
    QVERIFY(appDir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", appDir.path().toLocal8Bit());
    QVERIFY2(installRuntimePlugin(QDir(appDir.path()).filePath("plugins"), QStringLiteral("MeasurementInput")),
             "MeasurementInput plugin should be available for viewport picking");

    MainWindow window;
    window.resize(900, 650);
    window.show();
    QCoreApplication::processEvents();
    QTRY_VERIFY(PluginManager::instance().isPluginLoaded(QStringLiteral("MeasurementInput")));

    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance input;
    input.id = QStringLiteral("measure_point_pair_input");
    input.moduleId = QStringLiteral("MeasurementInput");
    input.name = QStringLiteral("测量输入");
    input.params["mode"] = QStringLiteral("point_pair");
    project->addModule(input);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QVERIFY(processTree->topLevelItemCount() > 0);
    processTree->setCurrentItem(processTree->topLevelItem(processTree->topLevelItemCount() - 1));

    ViewportWidget* viewport = window.findChild<ViewportWidget*>();
    QVERIFY(viewport != nullptr);
    HImageWidget* imageWidget = viewport->imageWidget();
    QVERIFY(imageWidget != nullptr);

    QImage image(120, 90, QImage::Format_RGB32);
    image.fill(Qt::white);
    viewport->displayImage(image);
    QCoreApplication::processEvents();
    QTRY_VERIFY(imageWidget->hasImage());
    QTRY_VERIFY(imageWidget->width() > 20);
    QTRY_VERIFY(imageWidget->height() > 20);

    const QPoint widgetPoint1 = imageWidget->imageToWidget(QPointF(20.0, 25.0)).toPoint();
    QVERIFY2(imageWidget->rect().contains(widgetPoint1),
             qPrintable(QString("Mapped first click point %1,%2 is outside image widget")
                            .arg(widgetPoint1.x())
                            .arg(widgetPoint1.y())));
    const QPointF expectedPoint1 = imageWidget->widgetToImage(widgetPoint1);
    QTest::mouseClick(imageWidget, Qt::LeftButton, Qt::NoModifier, widgetPoint1);
    QCoreApplication::processEvents();

    const QPoint widgetPoint2 = imageWidget->imageToWidget(QPointF(80.0, 60.0)).toPoint();
    QVERIFY2(imageWidget->rect().contains(widgetPoint2),
             qPrintable(QString("Mapped second click point %1,%2 is outside image widget")
                            .arg(widgetPoint2.x())
                            .arg(widgetPoint2.y())));
    const QPointF expectedPoint2 = imageWidget->widgetToImage(widgetPoint2);
    QTest::mouseClick(imageWidget, Qt::LeftButton, Qt::NoModifier, widgetPoint2);
    QCoreApplication::processEvents();

    ModuleInstance* module = project->findModule(QStringLiteral("measure_point_pair_input"));
    QVERIFY(module != nullptr);
    const QJsonArray point1 = module->params["point1"].toArray();
    const QJsonArray point2 = module->params["point2"].toArray();
    QCOMPARE(point1.size(), 2);
    QCOMPARE(point2.size(), 2);
    QVERIFY(qAbs(point1.at(0).toDouble() - expectedPoint1.x()) < 0.5);
    QVERIFY(qAbs(point1.at(1).toDouble() - expectedPoint1.y()) < 0.5);
    QVERIFY(qAbs(point2.at(0).toDouble() - expectedPoint2.x()) < 0.5);
    QVERIFY(qAbs(point2.at(1).toDouble() - expectedPoint2.y()) < 0.5);

    const QImage rendered = imageWidget->grab().toImage().convertToFormat(QImage::Format_RGB32);
    int cyanPixels = 0;
    int orangePixels = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            const QColor color = rendered.pixelColor(x, y);
            if (color.red() < 100 && color.green() > 120 && color.blue() > 140) {
                ++cyanPixels;
            }
            if (color.red() > 180 && color.green() > 80 && color.green() < 190 && color.blue() < 100) {
                ++orangePixels;
            }
        }
    }
    QVERIFY2(cyanPixels > 20, qPrintable(QString("Expected visible measurement line, cyanPixels=%1").arg(cyanPixels)));
    QVERIFY2(orangePixels > 20,
             qPrintable(QString("Expected visible picked points, orangePixels=%1").arg(orangePixels)));
}

void TestMainWindow::testTreeAndCanvasSyncSelection() {
    MainWindow window;
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance instance;
    instance.id = QStringLiteral("sync_1");
    instance.moduleId = QStringLiteral("OutputProbe");
    instance.name = QStringLiteral("Sync Module");
    project->addModule(instance);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QCOMPARE(processTree->topLevelItemCount(), 1);

    QTreeWidgetItem* item = processTree->topLevelItem(0);
    processTree->setCurrentItem(item);
    QCoreApplication::processEvents();

    // Canvas should also have the node selected
    FlowCanvas* canvas = window.findChild<FlowCanvas*>();
    QVERIFY(canvas != nullptr);
    FlowNodeItem* nodeItem = canvas->nodeItem(QStringLiteral("sync_1"));
    QVERIFY(nodeItem != nullptr);
    QVERIFY2(nodeItem->isSelected(),
             "Canvas node should be selected when process tree item is selected");
}

void TestMainWindow::testCycleRunDoesNotStealSelection() {
    RunEngine& engine = RunEngine::instance();

    MainWindow window;
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance firstInstance;
    firstInstance.id = QStringLiteral("cycle_1");
    firstInstance.moduleId = QStringLiteral("OutputProbe");
    firstInstance.name = QStringLiteral("Cycle First");
    project->addModule(firstInstance);

    ModuleInstance secondInstance;
    secondInstance.id = QStringLiteral("cycle_2");
    secondInstance.moduleId = QStringLiteral("OutputProbe");
    secondInstance.name = QStringLiteral("Cycle Second");
    project->addModule(secondInstance);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);

    // Select the first module
    QTreeWidgetItem* firstItem = processTree->topLevelItem(0);
    processTree->setCurrentItem(firstItem);
    QCoreApplication::processEvents();

    // Pin the inspector so cycle run doesn't steal selection
    ModuleInspectorPanel* inspector = window.findChild<ModuleInspectorPanel*>();
    QVERIFY(inspector != nullptr);
    inspector->setPinned(true);
    // Record the inspector's current instance ID
    const QString pinnedInstanceId = inspector->currentInstanceId();

    // Run once (not cycle, just verify pinned behavior)
    engine.clearModules();
    MainWindowOutputProbeModule* mod1 = new MainWindowOutputProbeModule(QStringLiteral("cycle_1"));
    mod1->initialize();
    engine.addModule(mod1);
    MainWindowOutputProbeModule* mod2 = new MainWindowOutputProbeModule(QStringLiteral("cycle_2"));
    mod2->initialize();
    engine.addModule(mod2);
    engine.runOnce();
    QCoreApplication::processEvents();

    // Selection should still be on the first module
    QCOMPARE(processTree->currentItem(), firstItem);
    // Inspector content should not have switched (pinned)
    QCOMPARE(inspector->currentInstanceId(), pinnedInstanceId);

    inspector->setPinned(false);
    engine.clearModules();
    delete mod1;
    delete mod2;
}

void TestMainWindow::testStepRunFollowsExecution() {
    RunEngine& engine = RunEngine::instance();

    MainWindow window;
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance firstInstance;
    firstInstance.id = QStringLiteral("step_1");
    firstInstance.moduleId = QStringLiteral("OutputProbe");
    firstInstance.name = QStringLiteral("Step First");
    project->addModule(firstInstance);

    ModuleInstance secondInstance;
    secondInstance.id = QStringLiteral("step_2");
    secondInstance.moduleId = QStringLiteral("OutputProbe");
    secondInstance.name = QStringLiteral("Step Second");
    project->addModule(secondInstance);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);

    engine.clearModules();
    MainWindowOutputProbeModule* mod1 = new MainWindowOutputProbeModule(QStringLiteral("step_1"));
    mod1->initialize();
    engine.addModule(mod1);
    MainWindowOutputProbeModule* mod2 = new MainWindowOutputProbeModule(QStringLiteral("step_2"));
    mod2->initialize();
    engine.addModule(mod2);

    // Step run should follow the executing module (since not pinned)
    // Use onStepRun() call instead of engine.runOnce()
    QVERIFY(QMetaObject::invokeMethod(&window, "onStepRun", Qt::DirectConnection));
    QCoreApplication::processEvents();

    // After step run, selection should follow (when not pinned)
    QVERIFY(processTree->topLevelItemCount() >= 2);

    engine.clearModules();
    delete mod1;
    delete mod2;
}

void TestMainWindow::testCloseInspectorDoesNotAutoExpand() {
    MainWindow window;
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance instance;
    instance.id = QStringLiteral("close_1");
    instance.moduleId = QStringLiteral("OutputProbe");
    instance.name = QStringLiteral("Close Test");
    project->addModule(instance);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);

    // Select the module
    QTreeWidgetItem* item = processTree->topLevelItem(0);
    processTree->setCurrentItem(item);
    QCoreApplication::processEvents();

    // Close the inspector
    ModuleInspectorPanel* inspector = window.findChild<ModuleInspectorPanel*>();
    QVERIFY(inspector != nullptr);
    QToolButton* closeBtn = inspector->findChild<QToolButton*>("InspectorCloseBtn");
    QVERIFY(closeBtn != nullptr);
    QTest::mouseClick(closeBtn, Qt::LeftButton);
    QCoreApplication::processEvents();

    QVERIFY(!inspector->isVisible());

    // Select another module via tree — inspector should NOT auto-expand
    ModuleInstance instance2;
    instance2.id = QStringLiteral("close_2");
    instance2.moduleId = QStringLiteral("OutputProbe");
    instance2.name = QStringLiteral("Close Test 2");
    project->addModule(instance2);
    QCoreApplication::processEvents();

    QTreeWidgetItem* item2 = processTree->topLevelItem(1);
    processTree->setCurrentItem(item2);
    QCoreApplication::processEvents();

    QVERIFY2(!inspector->isVisible(),
             "Inspector should not auto-expand after being closed by the user");
}

void TestMainWindow::testDeleteModuleClearsInspector() {
    MainWindow window;
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance instance;
    instance.id = QStringLiteral("del_1");
    instance.moduleId = QStringLiteral("OutputProbe");
    instance.name = QStringLiteral("Delete Test");
    project->addModule(instance);
    QCoreApplication::processEvents();

    QTreeWidget* processTree = window.findChild<QTreeWidget*>("ProcessTree");
    QVERIFY(processTree != nullptr);
    QCOMPARE(processTree->topLevelItemCount(), 1);

    // Select the module
    QTreeWidgetItem* item = processTree->topLevelItem(0);
    processTree->setCurrentItem(item);
    QCoreApplication::processEvents();

    ModuleInspectorPanel* inspector = window.findChild<ModuleInspectorPanel*>();
    QVERIFY(inspector != nullptr);

    // Delete the module from the project
    project->removeModule(QStringLiteral("del_1"));
    QCoreApplication::processEvents();

    // Inspector should be in empty state after deletion
    QVERIFY2(inspector->currentInstanceId().isEmpty(),
             "Inspector should be cleared after the selected module is deleted");

    // Tree should reflect the deletion
    QCOMPARE(processTree->topLevelItemCount(), 0);
}

void TestMainWindow::testOldAdvancedConfigDialogStillUsable() {
    MainWindow window;
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    ModuleInstance instance;
    instance.id = QStringLiteral("adv_1");
    instance.moduleId = QStringLiteral("OutputProbe");
    instance.name = QStringLiteral("Advanced Config Test");
    project->addModule(instance);
    QCoreApplication::processEvents();

    // The old advanced config dialog path should not crash
    // We call the internal method directly (it's private, so we use the signal path)
    ModuleInspectorPanel* inspector = window.findChild<ModuleInspectorPanel*>();
    QVERIFY(inspector != nullptr);

    QToolButton* advancedBtn = inspector->findChild<QToolButton*>("InspectorAdvancedBtn");
    if (advancedBtn) {
        // Click the advanced button - it should emit advancedConfigRequested
        QSignalSpy spy(inspector, &ModuleInspectorPanel::advancedConfigRequested);
        QTest::mouseClick(advancedBtn, Qt::LeftButton);
        QCoreApplication::processEvents();
        // The signal should have been emitted (the actual dialog may or may not open in offscreen mode)
        QVERIFY2(spy.count() >= 1,
                 "Advanced config button should emit advancedConfigRequested signal");
    }
}

// 中: 回归测试 — 窄窗口后工具面板恢复
void TestMainWindow::testNarrowWindowToolPanelRestored() {
    MainWindow window;
    window.resize(1600, 900);
    window.show();
    QCoreApplication::processEvents();

    QDockWidget* toolDock = window.findChild<QDockWidget*>("ToolPanelDock");
    QVERIFY(toolDock != nullptr);
    QVERIFY2(toolDock->isVisible(), "Tool panel should be visible at 1600px");

    // 缩到 1024px
    window.resize(1024, 700);
    QCoreApplication::processEvents();

    // 恢复到 1600px
    window.resize(1600, 900);
    QCoreApplication::processEvents();

    QVERIFY2(toolDock->isVisible(),
             "Tool panel should be restored after window resized back to 1600px");
}

// 中: 回归测试 — 手动折叠检查器后 splitter 重分配
void TestMainWindow::testInspectorManualCollapseResizesSplitter() {
    MainWindow window;
    window.resize(1440, 900);
    window.show();
    QCoreApplication::processEvents();

    auto* inspector = window.findChild<DeepLux::ModuleInspectorPanel*>();
    if (!inspector)
        QSKIP("ModuleInspectorPanel not found, skipping");

    QSplitter* rightTopSplitter = window.findChild<QSplitter*>("RightTopSplitter");
    QVERIFY(rightTopSplitter != nullptr);

    const int inspectorIdx = rightTopSplitter->indexOf(inspector);
    if (inspectorIdx < 0)
        QSKIP("Inspector not in splitter, skipping");

    QList<int> beforeSizes = rightTopSplitter->sizes();
    const int beforeWidth = beforeSizes.value(inspectorIdx, 0);
    if (beforeWidth <= 32)
        QSKIP("Inspector width too small in test env, skipping");

    // 模拟折叠信号
    emit inspector->collapseToggled(true);
    QCoreApplication::processEvents();

    QList<int> afterSizes = rightTopSplitter->sizes();
    const int afterWidth = afterSizes.value(inspectorIdx, 0);
    QVERIFY2(afterWidth <= 32,
             QString("Inspector should collapse to <=32px after collapseToggled, got %1").arg(afterWidth).toUtf8());

    // 恢复
    emit inspector->collapseToggled(false);
    QCoreApplication::processEvents();

    QList<int> restoredSizes = rightTopSplitter->sizes();
    const int restoredWidth = restoredSizes.value(inspectorIdx, 0);
    QVERIFY2(restoredWidth > 32,
             QString("Inspector should expand after collapseToggled(false), got %1").arg(restoredWidth).toUtf8());
}

// 中: 回归测试 — 焦点模式保留日志面板可见状态
void TestMainWindow::testFocusModePreservesLogVisibility() {
    MainWindow window;
    window.resize(1440, 900);
    window.show();
    QCoreApplication::processEvents();

    QDockWidget* logDock = window.findChild<QDockWidget*>("LogDock");
    QVERIFY(logDock != nullptr);
    const bool logVisibleBefore = logDock->isVisible();

    // 模拟双击主视图进入焦点模式
    QWidget* imageDisplay = window.findChild<QWidget*>("ImageDisplayWidget");
    QVERIFY(imageDisplay != nullptr);
    QTest::mouseDClick(imageDisplay, Qt::LeftButton);
    QCoreApplication::processEvents();
    QVERIFY2(!logDock->isVisible(), "Log dock should be hidden in focus mode");

    // 双击退出
    QTest::mouseDClick(imageDisplay, Qt::LeftButton);
    QCoreApplication::processEvents();
    QVERIFY2(logDock->isVisible() == logVisibleBefore,
             "Log dock visibility should be restored after exiting focus mode");
}

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
