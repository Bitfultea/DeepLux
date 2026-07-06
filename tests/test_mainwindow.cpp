#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVector3D>
#include <QtTest/QtTest>
#include <core/agent/AgentController.h>
#include <core/manager/PluginManager.h>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentChatPanel.h>
#include <ui/widgets/AgentMessageBubble.h>
#include <ui/widgets/AgentToolPreviewCard.h>
#include <ui/widgets/FlowCanvas.h>

using namespace DeepLux;

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
    void testMeasurementPickingUpdatesInputAndClipboard();

private:
    bool installFitLinePlugin(const QString& pluginRoot) const;
    bool installRuntimePlugin(const QString& pluginRoot, const QString& pluginName) const;
};

void TestMainWindow::init() {
    ProjectManager::instance().closeProject();
    PluginManager::instance().shutdown();
    qunsetenv("DEEPLUX_APP_DATA_DIR");
}

void TestMainWindow::cleanup() {
    ProjectManager::instance().closeProject();
    PluginManager::instance().shutdown();
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
    const QString metadataSrc =
        QDir::cleanPath(QCoreApplication::applicationDirPath() +
                        QString("/../../src/plugins/geometry/%1/metadata.json").arg(pluginName));
    const QString libSrc =
        QDir::cleanPath(QCoreApplication::applicationDirPath() + QString("/../lib/lib%1Plugin.so").arg(pluginName));

    if (!QFileInfo::exists(metadataSrc) || !QFileInfo::exists(libSrc)) {
        return false;
    }

    QFile::remove(pluginDir.filePath("metadata.json"));
    QFile::remove(pluginDir.filePath(QString("lib%1Plugin.so").arg(pluginName)));

    return QFile::copy(metadataSrc, pluginDir.filePath("metadata.json")) &&
           QFile::copy(libSrc, pluginDir.filePath(QString("lib%1Plugin.so").arg(pluginName)));
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
    QCOMPARE(mainContentMargins.right(), 0);
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

    QWidget* processToolBar = window.findChild<QWidget*>("ProcessToolBar");
    QVERIFY(processToolBar != nullptr);
    QVERIFY(processToolBar->layout() != nullptr);
    const QMargins processToolbarMargins = processToolBar->layout()->contentsMargins();
    QVERIFY2(window.findChild<QWidget*>("ToolToolBar") == nullptr,
             "Tool panel should not contain dead shortcut buttons above the plugin list");
    QVERIFY2(window.findChild<QToolButton*>("ToolCreateProcessBtn") == nullptr,
             "Removed tool-panel shortcut buttons should not remain clickable but inert");
    QVERIFY2(processToolbarMargins.top() <= 8, "Panel toolbars should not have excessive top padding");

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

    QWidget* processTabContent = window.findChild<QWidget*>("ProcessTabContent");
    QVERIFY(processTabContent != nullptr);
    QVERIFY(processTabContent->layout() != nullptr);
    QCOMPARE(processTabContent->layout()->contentsMargins().left(), 6);
    QCOMPARE(processTabContent->layout()->contentsMargins().right(), 6);

    QWidget* dataSourcePanelForMargins = window.findChild<QWidget*>("DataSourcePanel");
    QVERIFY(dataSourcePanelForMargins != nullptr);
    QVERIFY(dataSourcePanelForMargins->layout() != nullptr);
    QCOMPARE(dataSourcePanelForMargins->layout()->contentsMargins().left(), 6);
    QCOMPARE(dataSourcePanelForMargins->layout()->contentsMargins().right(), 6);

    QToolButton* processStartButton = window.findChild<QToolButton*>("ProcessStartPauseBtn");
    QVERIFY(processStartButton != nullptr);
    QVERIFY2(processStartButton->styleSheet().contains(QStringLiteral("QToolButton")),
             "Process toolbar buttons should use the same styled tool-button rules as the tool panel");

    QLabel* viewportTitle = window.findChild<QLabel*>("ViewportTitle");
    QVERIFY(viewportTitle != nullptr);
    QVERIFY2(viewportTitle->styleSheet().contains(QStringLiteral("font-size: 13px")),
             "Viewport title should use the same panel title font size");

    QTabWidget* logTabs = window.findChild<QTabWidget*>("LogTerminalTabs");
    QVERIFY(logTabs != nullptr);
    QVERIFY(logTabs->tabBar() != nullptr);
    QVERIFY2(processTabs->styleSheet().contains(QStringLiteral("font-size: 13px")),
             "Process tabs should use the shared panel tab font size");
    QVERIFY2(logTabs->styleSheet().contains(QStringLiteral("font-size: 13px")),
             "Bottom tabs should use the shared panel tab font size");
    const int logTabTextHeight = logTabs->tabBar()->fontMetrics().height();
    QVERIFY2(logTabs->tabBar()->tabRect(0).height() >= logTabTextHeight + 10,
             qPrintable(QString("Bottom tab height %1 clips text height %2")
                            .arg(logTabs->tabBar()->tabRect(0).height())
                            .arg(logTabTextHeight)));

    const int agentChatIndex = logTabs->indexOf(agentPanel);
    QVERIFY(agentChatIndex >= 0);
    QJsonArray pendingTools;
    pendingTools.append(QJsonObject{
        {"id", "call_remove"},
        {"type", "function"},
        {"name", "remove_module"},
        {"arguments", QJsonObject{{"instanceId", "grab_1"}}},
    });
    emit AgentController::instance().toolsPendingConfirmation(pendingTools);
    QCoreApplication::processEvents();
    QCOMPARE(logTabs->currentIndex(), agentChatIndex);
    QVERIFY2(logTabs->tabToolTip(agentChatIndex).contains(QStringLiteral("等待确认")),
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
    QCOMPARE(logTable->parentWidget()->layout()->contentsMargins().left(), 6);
    QCOMPARE(logTable->parentWidget()->layout()->contentsMargins().right(), 6);
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
}

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
