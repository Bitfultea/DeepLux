#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidget>
#include <QtTest/QTest>
#include <core/engine/RunEngine.h>
#include <core/manager/PluginManager.h>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentChatPanel.h>
#include <ui/widgets/AgentMessageBubble.h>
#include <ui/widgets/FlowCanvas.h>
#include <ui/widgets/HImageWidget.h>
#include <ui/widgets/ViewportWidget.h>

namespace {

bool captureWindow(DeepLux::MainWindow& window, const QSize& size, const QString& filePath) {
    window.resize(size);
    window.show();
    QCoreApplication::processEvents();
    QTest::qWait(120);
    return window.grab().save(filePath);
}

bool saveShot(DeepLux::MainWindow& window, const QDir& dir, const QString& name) {
    QCoreApplication::processEvents();
    QTest::qWait(120);
    return window.grab().save(dir.filePath(name));
}

QTabWidget* tabsByName(DeepLux::MainWindow& window, const char* objectName) {
    return window.findChild<QTabWidget*>(QString::fromLatin1(objectName));
}

int tabIndex(QTabWidget* tabs, const QString& text) {
    if (!tabs)
        return -1;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == text)
            return i;
    }
    return -1;
}

void clickTab(QTabWidget* tabs, int index) {
    if (!tabs || index < 0 || index >= tabs->count())
        return;
    QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(index).center());
    QCoreApplication::processEvents();
}

void clickToolbarAction(QToolBar* toolbar, const QString& text) {
    if (!toolbar)
        return;
    for (QAction* action : toolbar->actions()) {
        if (action->text().remove('&') == text) {
            if (QWidget* button = toolbar->widgetForAction(action)) {
                QTest::mouseClick(button, Qt::LeftButton, Qt::NoModifier, button->rect().center());
            } else {
                QTest::mouseClick(toolbar, Qt::LeftButton, Qt::NoModifier, toolbar->actionGeometry(action).center());
            }
            QCoreApplication::processEvents();
            return;
        }
    }
}

void seedAgentChatDemo(DeepLux::MainWindow& window) {
    auto* panel = window.findChild<DeepLux::AgentChatPanel*>();
    if (!panel)
        return;
    panel->addMessage(DeepLux::AgentMessageBubble::Sender::User, QStringLiteral("帮我创建一个找圆的流程"));
    panel->addMessage(DeepLux::AgentMessageBubble::Sender::Agent,
                      QStringLiteral("参数说明\n\n- param1：Canny边缘检测的高阈值（值越小，检测到的边缘越多）\n\n"
                                     "- param2：圆心投票数阈值（值越小，越容易检测到圆，但也可能误检）"));
    panel->setThinking(true);
}

bool captureClickedStates(DeepLux::MainWindow& window, const QDir& dir) {
    window.resize(QSize(1024, 700));
    window.show();
    QCoreApplication::processEvents();
    QTest::qWait(300);

    bool ok = true;
    ok = saveShot(window, dir, QStringLiteral("01-initial-1024.png")) && ok;

    if (QTreeWidget* toolTree = window.findChild<QTreeWidget*>(QStringLiteral("ToolBoxTree"))) {
        for (int i = 0; i < toolTree->topLevelItemCount(); ++i) {
            toolTree->topLevelItem(i)->setExpanded(i == 1 || i == 3 || i == 4 || i == 7);
        }
        QCoreApplication::processEvents();
        ok = saveShot(window, dir, QStringLiteral("09-tool-plugin-icons.png")) && ok;
    }

    QTabWidget* processTabs = tabsByName(window, "ProcessTabWidget");
    clickTab(processTabs, tabIndex(processTabs, QStringLiteral("画布")));
    ok = saveShot(window, dir, QStringLiteral("02-process-canvas-tab.png")) && ok;

    clickTab(processTabs, tabIndex(processTabs, QStringLiteral("数据源")));
    ok = saveShot(window, dir, QStringLiteral("03-process-datasource-tab.png")) && ok;

    QTabWidget* bottomTabs = tabsByName(window, "LogTerminalTabs");
    clickTab(bottomTabs, tabIndex(bottomTabs, QStringLiteral("终端")));
    ok = saveShot(window, dir, QStringLiteral("04-bottom-terminal-tab.png")) && ok;

    seedAgentChatDemo(window);
    clickTab(bottomTabs, tabIndex(bottomTabs, QStringLiteral("Agent 对话")));
    ok = saveShot(window, dir, QStringLiteral("05-bottom-agent-chat-tab.png")) && ok;

    clickTab(bottomTabs, tabIndex(bottomTabs, QStringLiteral("Agent 日志")));
    ok = saveShot(window, dir, QStringLiteral("06-bottom-agent-log-tab.png")) && ok;

    QToolBar* mainToolbar = window.findChild<QToolBar*>(QStringLiteral("MainToolBar"));
    clickToolbarAction(mainToolbar, QStringLiteral("切换主题"));
    ok = saveShot(window, dir, QStringLiteral("07-theme-toggle.png")) && ok;

    if (QToolButton* toolClose = window.findChild<QToolButton*>(QStringLiteral("ToolCloseBtn"))) {
        QTest::mouseClick(toolClose, Qt::LeftButton, Qt::NoModifier, toolClose->rect().center());
        QCoreApplication::processEvents();
    }
    ok = saveShot(window, dir, QStringLiteral("08-tool-panel-closed.png")) && ok;

    return ok;
}

// 收尾2: 正式尺寸截图 1920/1280 深浅（默认浅色起始，切换主题采集深色）
// "切换主题"动作位于"视图"菜单而非工具栏，需按文本查找 QAction 并 trigger
static bool triggerActionByText(DeepLux::MainWindow& window, const QString& text) {
    for (QAction* action : window.findChildren<QAction*>()) {
        if (action->text().remove('&') == text) {
            action->trigger();
            QCoreApplication::processEvents();
            return true;
        }
    }
    return false;
}

bool captureFormalSizes(DeepLux::MainWindow& window, const QDir& dir) {
    bool ok = true;

    // 起始为浅色主题（配置默认 darkTheme=false）
    ok = captureWindow(window, QSize(1920, 1080), dir.filePath("formal_1920_light.png")) && ok;
    triggerActionByText(window, QStringLiteral("切换主题")); // → 深色
    QTest::qWait(200);
    ok = captureWindow(window, QSize(1920, 1080), dir.filePath("formal_1920_dark.png")) && ok;

    ok = captureWindow(window, QSize(1280, 800), dir.filePath("formal_1280_dark.png")) && ok;
    triggerActionByText(window, QStringLiteral("切换主题")); // → 浅色
    QTest::qWait(200);
    ok = captureWindow(window, QSize(1280, 800), dir.filePath("formal_1280_light.png")) && ok;

    return ok;
}

bool capturePluginConfigDialog(DeepLux::MainWindow& window, const QDir& dir) {
    if (!DeepLux::PluginManager::instance().isPluginLoaded(QStringLiteral("LoadPointCloud"))) {
        qWarning("LoadPointCloud is required for the configuration screenshot");
        return false;
    }

    DeepLux::Project* project = DeepLux::ProjectManager::instance().currentProject();
    if (!project) {
        project = DeepLux::ProjectManager::instance().newProject();
    }
    if (!project) {
        return false;
    }

    DeepLux::ModuleInstance loader;
    loader.id = QStringLiteral("capture_pointcloud_config");
    loader.moduleId = QStringLiteral("LoadPointCloud");
    loader.name = QStringLiteral("加载点云");
    project->addModule(loader);
    QCoreApplication::processEvents();

    window.selectModuleForCapture(loader.id);

    bool saved = false;
    QTimer::singleShot(80, [&]() {
        QWidget* dialog = window.findChild<QWidget*>(QStringLiteral("PluginConfigDialog"));
        if (!dialog) {
            return;
        }
        dialog->resize(560, 480);
        QCoreApplication::processEvents();
        saved = dialog->grab().save(dir.filePath(QStringLiteral("10-plugin-config-dialog.png")));
        dialog->close();
    });

    const bool invoked = QMetaObject::invokeMethod(&window, "_phase8_openAdvancedPluginConfig", Qt::DirectConnection,
                                                   Q_ARG(QString, loader.id));
    QTest::qWait(180);
    return invoked && saved;
}

// 收尾2: 安装插件到临时目录，供截图工程加载
bool installPluginForCapture(const QString& repoRoot, const QString& pluginTempRoot, const QString& dirName,
                             const QString& metadataRel, const QString& libName) {
    const QString pluginDir = pluginTempRoot + "/" + dirName;
    if (!QDir().mkpath(pluginDir))
        return false;
    const QString metaSrc = QDir(repoRoot).filePath(metadataRel);
    const QString libSrc = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../lib/" + libName);
    if (!QFileInfo::exists(metaSrc) || !QFileInfo::exists(libSrc))
        return false;
    QFile::remove(pluginDir + "/metadata.json");
    QFile::remove(pluginDir + "/" + libName);
    return QFile::copy(metaSrc, pluginDir + "/metadata.json") && QFile::copy(libSrc, pluginDir + "/" + libName);
}

// 阶3: 加载并运行找圆验收工程，等待 runFinished、校验圆结果在误差内、
// 选择找圆节点并刷新检查器/主视图。任何一步失败返回 false（任务失败）。
bool loadAndRunFindCircleAcceptance(const QString& repoRoot, const QString& pluginTempRoot,
                                    DeepLux::MainWindow& window) {
    const QString acceptanceRoot = repoRoot + "/tests/acceptance";
    const QString dataDir = acceptanceRoot + "/data";

    // 读取工程并替换 @ACCEPTANCE_DATA@ 占位符，写入临时文件
    QFile pf(acceptanceRoot + "/projects/accept_findcircle.json");
    if (!pf.open(QIODevice::ReadOnly))
        return false;
    QString text = QString::fromUtf8(pf.readAll());
    pf.close();
    text.replace(QStringLiteral("@ACCEPTANCE_DATA@"), dataDir);

    QTemporaryDir tmpProj;
    if (!tmpProj.isValid())
        return false;
    const QString tmpProjPath = tmpProj.filePath("accept_findcircle.json");
    QFile out(tmpProjPath);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(text.toUtf8());
    out.close();

    // 安装并加载所需插件
    if (!installPluginForCapture(repoRoot, pluginTempRoot, "GrabImage",
                                 "src/plugins/image_processing/GrabImage/metadata.json", "libGrabImagePlugin.so"))
        return false;
    if (!installPluginForCapture(repoRoot, pluginTempRoot, "FindCircle",
                                 "src/plugins/detection/FindCircle/metadata.json", "libFindCirclePlugin.so"))
        return false;
    if (!installPluginForCapture(repoRoot, pluginTempRoot, "LoadPointCloud",
                                 "src/plugins/image_processing/LoadPointCloud/metadata.json",
                                 "libLoadPointCloudPlugin.so"))
        return false;

    DeepLux::PluginManager::instance().addPluginPath(pluginTempRoot);
    DeepLux::PluginManager::instance().initialize();
    DeepLux::PluginManager::instance().loadPlugin("GrabImage");
    DeepLux::PluginManager::instance().loadPlugin("FindCircle");
    DeepLux::PluginManager::instance().loadPlugin("LoadPointCloud");

    // 打开工程并运行
    DeepLux::Project* project = DeepLux::ProjectManager::instance().openProject(tmpProjPath);
    if (!project)
        return false;

    // 显式将工程装入 RunEngine（确保有模块可运行）
    if (!DeepLux::RunEngine::instance().loadProject(project)) {
        qWarning("failed to load findcircle project into RunEngine");
        return false;
    }

    // P1-4: 捕获 RunResult 并校验真实圆结果；失败须使任务失败，不得静默跳过
    DeepLux::RunResult runResult;
    bool gotResult = false;
    QMetaObject::Connection conn = QObject::connect(&DeepLux::RunEngine::instance(), &DeepLux::RunEngine::runFinished,
                                                    [&](const DeepLux::RunResult& r) {
                                                        runResult = r;
                                                        gotResult = true;
                                                    });

    DeepLux::RunEngine::instance().runOnce();
    QElapsedTimer waitTimer;
    waitTimer.start();
    while (!gotResult && waitTimer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QTest::qWait(10);
    }
    QObject::disconnect(conn);

    if (!gotResult || !runResult.success) {
        qWarning("findcircle acceptance run failed: %s", qPrintable(runResult.errorMessage));
        return false;
    }

    // 阶3: 校验圆结果落在预期误差内（读 expected/circle_640x480.json）
    const DeepLux::ImageData fcOut = DeepLux::RunEngine::instance().moduleOutput("findcircle");
    const double gotR = fcOut.data("circle_radius").toDouble();
    const double gotCx = fcOut.data("circle_center_x").toDouble();
    const double gotCy = fcOut.data("circle_center_y").toDouble();
    if (gotR <= 0) {
        qWarning("findcircle produced no valid circle result");
        return false;
    }
    QFile ef(acceptanceRoot + "/expected/circle_640x480.json");
    if (!ef.open(QIODevice::ReadOnly)) {
        qWarning("failed to open findcircle expected result");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument expectedDocument = QJsonDocument::fromJson(ef.readAll(), &parseError);
    ef.close();
    if (parseError.error != QJsonParseError::NoError || !expectedDocument.isObject()) {
        qWarning("invalid findcircle expected result: %s", qPrintable(parseError.errorString()));
        return false;
    }
    const QJsonObject exp = expectedDocument.object();
    const double tolC = exp["tolerance_center_px"].toDouble(-1.0);
    const double tolR = exp["tolerance_radius_px"].toDouble(-1.0);
    if (tolC < 0.0 || tolR < 0.0 || qAbs(gotCx - exp["circle_center_x"].toDouble()) > tolC ||
        qAbs(gotCy - exp["circle_center_y"].toDouble()) > tolC || qAbs(gotR - exp["circle_radius"].toDouble()) > tolR) {
        qWarning("findcircle result outside tolerance: c(%g,%g) r=%g", gotCx, gotCy, gotR);
        return false;
    }

    // 阶3: 选择找圆节点并展示检查器，使截图含检查器结果与节点状态
    window.selectModuleForCapture(QStringLiteral("findcircle"));
    QTabWidget* inspectorTabs = window.findChild<QTabWidget*>(QStringLiteral("InspectorTabs"));
    if (!inspectorTabs || inspectorTabs->count() < 2) {
        qWarning("findcircle inspector result tab is unavailable");
        return false;
    }
    inspectorTabs->setCurrentIndex(1);
    QCoreApplication::processEvents();
    QTest::qWait(100);
    return true;
}

bool captureFitCirclePickAcceptance(const QString& repoRoot, const QString& pluginTempRoot, DeepLux::MainWindow& window,
                                    const QDir& outputDir) {
    if (!installPluginForCapture(repoRoot, pluginTempRoot, "MeasurementInput",
                                 "src/plugins/geometry/MeasurementInput/metadata.json",
                                 "libMeasurementInputPlugin.so") ||
        !installPluginForCapture(repoRoot, pluginTempRoot, "FitCircle", "src/plugins/geometry/FitCircle/metadata.json",
                                 "libFitCirclePlugin.so")) {
        return false;
    }

    DeepLux::PluginManager::instance().addPluginPath(pluginTempRoot);
    DeepLux::PluginManager::instance().initialize();
    if (!DeepLux::PluginManager::instance().loadPlugin("MeasurementInput") ||
        !DeepLux::PluginManager::instance().loadPlugin("FitCircle")) {
        return false;
    }

    DeepLux::Project* project = DeepLux::ProjectManager::instance().newProject();
    if (!project)
        return false;
    DeepLux::ModuleInstance circle;
    circle.id = QStringLiteral("capture_fitcircle");
    circle.moduleId = QStringLiteral("FitCircle");
    circle.name = QStringLiteral("圆拟合");
    circle.posX = 120;
    circle.posY = 120;
    project->addModule(circle);

    window.resize(1280, 800);
    window.show();
    QCoreApplication::processEvents();
    DeepLux::ViewportWidget* viewport = window.findChild<DeepLux::ViewportWidget*>();
    DeepLux::HImageWidget* imageWidget = viewport ? viewport->imageWidget() : nullptr;
    QToolButton* runButton = window.findChild<QToolButton*>(QStringLiteral("FlowRunButton"));
    if (!viewport || !imageWidget || !runButton)
        return false;

    QImage image(640, 480, QImage::Format_RGB32);
    image.fill(QColor("#111827"));
    viewport->displayImage(image);
    QCoreApplication::processEvents();

    DeepLux::RunResult result;
    bool finished = false;
    const QMetaObject::Connection connection =
        QObject::connect(&DeepLux::RunEngine::instance(), &DeepLux::RunEngine::runFinished, &window,
                         [&](const DeepLux::RunResult& runResult) {
                             result = runResult;
                             finished = true;
                         });
    QTest::mouseClick(runButton, Qt::LeftButton);
    QCoreApplication::processEvents();

    for (const QPointF& point : {QPointF(420.0, 240.0), QPointF(320.0, 340.0), QPointF(220.0, 240.0)}) {
        const QPoint widgetPoint = imageWidget->imageToWidget(point).toPoint();
        if (!imageWidget->rect().contains(widgetPoint)) {
            QObject::disconnect(connection);
            return false;
        }
        QTest::mouseClick(imageWidget, Qt::LeftButton, Qt::NoModifier, widgetPoint);
        QCoreApplication::processEvents();
    }

    QElapsedTimer timer;
    timer.start();
    while (!finished && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QTest::qWait(10);
    }
    QObject::disconnect(connection);
    const DeepLux::ImageData fitOutput = DeepLux::RunEngine::instance().moduleOutput(circle.id);
    if (!finished || !result.success || qAbs(fitOutput.data("circle_radius").toDouble() - 100.0) >= 1.0) {
        return false;
    }

    QTest::qWait(100);
    const QImage rendered = imageWidget->grab().toImage().convertToFormat(QImage::Format_RGB32);
    int cyanPixels = 0;
    int orangePixels = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            const QColor color = rendered.pixelColor(x, y);
            cyanPixels += color.red() < 100 && color.green() > 120 && color.blue() > 140;
            orangePixels += color.red() > 180 && color.green() > 80 && color.green() < 190 && color.blue() < 100;
        }
    }
    window.selectModuleForCapture(circle.id);
    QCoreApplication::processEvents();
    return cyanPixels > 100 && orangePixels > 20 &&
           captureWindow(window, QSize(1280, 800), outputDir.filePath("fitcircle_pick_result.png"));
}

bool captureControlFlowAcceptance(const QString& repoRoot, const QString& pluginTempRoot, DeepLux::MainWindow& window,
                                  const QDir& outputDir) {
    if (!installPluginForCapture(repoRoot, pluginTempRoot, "If", "src/plugins/logic/If/metadata.json",
                                 "libIfPlugin.so") ||
        !installPluginForCapture(repoRoot, pluginTempRoot, "Delay", "src/plugins/logic/Delay/metadata.json",
                                 "libDelayPlugin.so")) {
        return false;
    }

    DeepLux::PluginManager::instance().addPluginPath(pluginTempRoot);
    DeepLux::PluginManager::instance().initialize();
    if (!DeepLux::PluginManager::instance().loadPlugin(QStringLiteral("条件分支")) ||
        !DeepLux::PluginManager::instance().loadPlugin(QStringLiteral("延时"))) {
        return false;
    }

    DeepLux::Project* project = DeepLux::ProjectManager::instance().newProject();
    if (!project)
        return false;

    DeepLux::ModuleInstance condition;
    condition.id = QStringLiteral("capture_condition");
    condition.moduleId = QStringLiteral("条件分支");
    condition.name = QStringLiteral("条件");
    condition.posX = -180;
    condition.posY = -60;
    condition.params["conditionType"] = QStringLiteral("Expression");
    condition.params["expressionString"] = QStringLiteral("true");
    project->addModule(condition);

    DeepLux::ModuleInstance trueBranch;
    trueBranch.id = QStringLiteral("capture_true");
    trueBranch.moduleId = QStringLiteral("延时");
    trueBranch.name = QStringLiteral("真分支");
    trueBranch.posX = 120;
    trueBranch.posY = -150;
    trueBranch.params["delayMs"] = 1;
    project->addModule(trueBranch);

    DeepLux::ModuleInstance falseBranch = trueBranch;
    falseBranch.id = QStringLiteral("capture_false");
    falseBranch.name = QStringLiteral("假分支");
    falseBranch.posY = 50;
    project->addModule(falseBranch);

    DeepLux::ModuleConnection trueConnection;
    trueConnection.fromModuleId = condition.id;
    trueConnection.toModuleId = trueBranch.id;
    trueConnection.fromPort = QStringLiteral("true");
    trueConnection.toPort = QStringLiteral("control");
    trueConnection.edgeType = QStringLiteral("control");
    project->addConnection(trueConnection);
    DeepLux::ModuleConnection falseConnection = trueConnection;
    falseConnection.fromPort = QStringLiteral("false");
    falseConnection.toModuleId = falseBranch.id;
    project->addConnection(falseConnection);

    window.resize(1280, 800);
    window.show();
    QCoreApplication::processEvents();
    QTabWidget* processTabs = tabsByName(window, "ProcessTabWidget");
    DeepLux::FlowCanvas* canvas = window.findChild<DeepLux::FlowCanvas*>();
    QToolButton* runButton = window.findChild<QToolButton*>(QStringLiteral("FlowRunButton"));
    if (!processTabs || !canvas || !runButton)
        return false;
    clickTab(processTabs, processTabs->indexOf(canvas));

    DeepLux::RunResult result;
    bool finished = false;
    const QMetaObject::Connection connection =
        QObject::connect(&DeepLux::RunEngine::instance(), &DeepLux::RunEngine::runFinished, &window,
                         [&](const DeepLux::RunResult& runResult) {
                             result = runResult;
                             finished = true;
                         });
    QTest::mouseClick(runButton, Qt::LeftButton);
    QElapsedTimer timer;
    timer.start();
    while (!finished && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QTest::qWait(10);
    }
    QObject::disconnect(connection);

    QTreeWidget* tree = window.findChild<QTreeWidget*>(QStringLiteral("ProcessTree"));
    DeepLux::FlowNodeItem* conditionNode = canvas->nodeItem(condition.id);
    DeepLux::FlowNodeItem* trueNode = canvas->nodeItem(trueBranch.id);
    DeepLux::FlowNodeItem* falseNode = canvas->nodeItem(falseBranch.id);
    if (!finished || !result.success || !tree || !conditionNode || !trueNode || !falseNode ||
        conditionNode->executionStatus() != QStringLiteral("success") ||
        trueNode->executionStatus() != QStringLiteral("success") ||
        falseNode->executionStatus() != QStringLiteral("skipped")) {
        return false;
    }

    QSplitter* topSplitter = window.findChild<QSplitter*>(QStringLiteral("RightTopSplitter"));
    const QList<int> savedSizes = topSplitter ? topSplitter->sizes() : QList<int>();
    if (topSplitter) {
        topSplitter->setSizes({600, 600, 0});
    }
    canvas->fitInView(canvas->scene()->itemsBoundingRect().adjusted(-30, -30, 30, 30), Qt::KeepAspectRatio);
    QCoreApplication::processEvents();
    const QImage rendered = canvas->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    int greenPixels = 0;
    int grayPixels = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            const QColor color = rendered.pixelColor(x, y);
            greenPixels += color.green() > 170 && color.red() < 80 && color.blue() < 130;
            grayPixels +=
                qAbs(color.red() - 156) < 10 && qAbs(color.green() - 163) < 10 && qAbs(color.blue() - 175) < 10;
        }
    }
    const bool accepted = greenPixels > 5 && grayPixels > 5 &&
                          captureWindow(window, QSize(1280, 800), outputDir.filePath("controlflow_canvas_result.png"));
    if (topSplitter) {
        topSplitter->setSizes(savedSizes);
    }
    return accepted;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("ui_capture_mainwindow");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument("output-dir", "Directory where screenshots will be written.");
    parser.process(app);

    const QString outputDir = parser.positionalArguments().isEmpty() ? QStringLiteral("/tmp/deeplux-ui-review")
                                                                     : parser.positionalArguments().first();
    QDir dir(outputDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        qWarning("Failed to create screenshot output directory");
        return 1;
    }

    DeepLux::MainWindow window;
    bool ok = true;

    // 收尾2: 加载并运行找圆验收工程，使截图带真实结果（结果叠加/节点状态/检查器）
    const QString repoRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    QTemporaryDir pluginTempRoot;
    const bool ranAcceptance = pluginTempRoot.isValid() &&
                               loadAndRunFindCircleAcceptance(repoRoot, pluginTempRoot.filePath("plugins"), window);

    // P1-4: 验收运行失败须使任务失败，不得静默跳过
    if (!ranAcceptance) {
        qWarning("findcircle acceptance run did not succeed; task fails");
        ok = false;
    }

    QTimer::singleShot(800, [&]() {
        ok = captureFormalSizes(window, dir) && ok;
        // 带真实结果的桌面/紧凑截图（验收运行成功后）
        if (ranAcceptance) {
            ok = captureWindow(window, QSize(1920, 1080), dir.filePath("findcircle_desktop_result.png")) && ok;
            ok = captureWindow(window, QSize(1280, 800), dir.filePath("findcircle_compact_result.png")) && ok;
        }
        ok = captureWindow(window, QSize(1440, 900), dir.filePath("deeplux_mainwindow_1440x900.png")) && ok;
        ok = captureWindow(window, QSize(1024, 700), dir.filePath("deeplux_mainwindow_1024x700.png")) && ok;
        ok = capturePluginConfigDialog(window, dir) && ok;
        ok = captureFitCirclePickAcceptance(repoRoot, pluginTempRoot.filePath("plugins"), window, dir) && ok;
        ok = captureControlFlowAcceptance(repoRoot, pluginTempRoot.filePath("plugins"), window, dir) && ok;
        ok = captureClickedStates(window, dir) && ok;
        app.quit();
    });

    app.exec();
    return ok ? 0 : 1;
}
