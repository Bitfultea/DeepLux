#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidget>
#include <QtTest/QTest>
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

    QTimer::singleShot(800, [&]() {
        ok = captureWindow(window, QSize(1440, 900), dir.filePath("deeplux_mainwindow_1440x900.png"));
        ok = captureWindow(window, QSize(1024, 700), dir.filePath("deeplux_mainwindow_1024x700.png")) && ok;
        ok = captureClickedStates(window, dir) && ok;
        app.quit();
    });

    app.exec();
    return ok ? 0 : 1;
}
