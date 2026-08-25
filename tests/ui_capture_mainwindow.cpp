#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
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
        return true;
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

    QTreeWidget* processTree = window.findChild<QTreeWidget*>(QStringLiteral("ProcessTree"));
    if (!processTree || processTree->topLevelItemCount() == 0) {
        return false;
    }

    QTreeWidgetItem* item = processTree->topLevelItem(processTree->topLevelItemCount() - 1);
    bool saved = false;
    QTimer::singleShot(80, [&]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal) {
            return;
        }
        modal->resize(560, 430);
        QCoreApplication::processEvents();
        saved = modal->grab().save(dir.filePath(QStringLiteral("10-plugin-config-dialog.png")));
        modal->close();
    });

    const QRect itemRect = processTree->visualItemRect(item);
    QTest::mouseDClick(processTree->viewport(), Qt::LeftButton, Qt::NoModifier, itemRect.center());
    QTest::qWait(180);
    return saved;
}

// 收尾2: 安装插件到临时目录，供截图工程加载
bool installPluginForCapture(const QString& repoRoot, const QString& pluginTempRoot, const QString& dirName,
                             const QString& metadataRel, const QString& libName) {
    const QString pluginDir = pluginTempRoot + "/" + dirName;
    if (!QDir().mkpath(pluginDir))
        return false;
    const QString metaSrc = QDir(repoRoot).filePath(metadataRel);
    const QString libSrc = QDir(repoRoot).filePath("build/lib/" + libName);
    if (!QFileInfo::exists(metaSrc) || !QFileInfo::exists(libSrc))
        return false;
    QFile::remove(pluginDir + "/metadata.json");
    QFile::remove(pluginDir + "/" + libName);
    return QFile::copy(metaSrc, pluginDir + "/metadata.json") && QFile::copy(libSrc, pluginDir + "/" + libName);
}

// 收尾2: 加载并运行找圆验收工程，使截图带真实结果。
// 返回是否成功加载并运行。
bool loadAndRunFindCircleAcceptance(const QString& repoRoot, const QString& pluginTempRoot) {
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

    DeepLux::PluginManager::instance().addPluginPath(pluginTempRoot);
    DeepLux::PluginManager::instance().initialize();
    DeepLux::PluginManager::instance().loadPlugin("GrabImage");
    DeepLux::PluginManager::instance().loadPlugin("FindCircle");

    // 打开工程并运行
    DeepLux::Project* project = DeepLux::ProjectManager::instance().openProject(tmpProjPath);
    if (!project)
        return false;

    DeepLux::RunEngine::instance().runOnce();
    QCoreApplication::processEvents();
    QTest::qWait(200);
    return true;
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
                               loadAndRunFindCircleAcceptance(repoRoot, pluginTempRoot.filePath("plugins"));

    QTimer::singleShot(800, [&]() {
        ok = captureFormalSizes(window, dir);
        // 带真实结果的桌面/紧凑截图（若找圆工程成功运行）
        if (ranAcceptance) {
            ok = captureWindow(window, QSize(1920, 1080), dir.filePath("findcircle_desktop_result.png")) && ok;
            ok = captureWindow(window, QSize(1280, 800), dir.filePath("findcircle_compact_result.png")) && ok;
        }
        ok = captureWindow(window, QSize(1440, 900), dir.filePath("deeplux_mainwindow_1440x900.png")) && ok;
        ok = captureWindow(window, QSize(1024, 700), dir.filePath("deeplux_mainwindow_1024x700.png")) && ok;
        ok = capturePluginConfigDialog(window, dir) && ok;
        ok = captureClickedStates(window, dir) && ok;
        app.quit();
    });

    app.exec();
    return ok ? 0 : 1;
}
