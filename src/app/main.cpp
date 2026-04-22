#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QDebug>

#include "core/common/CLIHandler.h"
#include "ui/views/MainWindow.h"
#include "core/platform/Platform.h"
#include "core/platform/PathUtils.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

using namespace DeepLux;

void initAppDirectories()
{
    PathUtils::ensureDirExists(PathUtils::appDataPath());
    PathUtils::ensureDirExists(PathUtils::configPath());
    PathUtils::ensureDirExists(PathUtils::pluginPath());
    PathUtils::ensureDirExists(PathUtils::logPath());
    PathUtils::ensureDirExists(PathUtils::projectPath());
}

bool checkOpenCV()
{
#ifdef DEEPLUX_HAS_OPENCV
    qDebug() << "OpenCV version:" << CV_VERSION;
    return true;
#else
    qDebug() << "OpenCV not available, running in demo mode";
    return true;
#endif
}

void setupStyleSheet(QApplication& app)
{
    QString style = R"(
        QMainWindow {
            background-color: #2b2b2b;
        }
        QWidget {
            background-color: #2b2b2b;
            color: #ffffff;
        }
        QMenuBar {
            background-color: #3c3c3c;
            color: #ffffff;
        }
        QMenuBar::item:selected {
            background-color: #505050;
        }
        QMenu {
            background-color: #3c3c3c;
            color: #ffffff;
            border: 1px solid #505050;
        }
        QMenu::item:selected {
            background-color: #0078d4;
        }
        QToolBar {
            background-color: #3c3c3c;
            border: none;
            spacing: 5px;
            padding: 5px;
        }
        QToolButton {
            background-color: transparent;
            border: none;
            padding: 5px;
            border-radius: 3px;
        }
        QToolButton:hover {
            background-color: #505050;
        }
        QToolButton:pressed {
            background-color: #0078d4;
        }
        QStatusBar {
            background-color: #3c3c3c;
            color: #ffffff;
        }
        QDockWidget::title {
            background-color: #3c3c3c;
            padding: 6px;
        }
        QTreeWidget {
            background-color: #2b2b2b;
            border: 1px solid #505050;
        }
        QTreeWidget::item {
            padding: 5px;
        }
        QTreeWidget::item:selected {
            background-color: #0078d4;
        }
    )";

    app.setStyleSheet(style);
}

int runCLI(const QStringList& args)
{
    CLIHandler& cli = CLIHandler::instance();

    if (!cli.parse(args)) {
        fprintf(stderr, "Error: %s\n\n%s\n",
                qPrintable(cli.errorString()),
                qPrintable(cli.helpText()));
        return 1;
    }

    if (cli.command().isEmpty()) {
        // 无命令，显示帮助
        printf("%s\n", qPrintable(cli.helpText()));
        return 0;
    }

    CommandContext ctx;
    ctx.setGuiMode(false);

    ICommand* cmd = cli.findCommand(cli.command());
    if (!cmd) {
        fprintf(stderr, "Error: Command '%s' not found\n", qPrintable(cli.command()));
        return 1;
    }

    int result = cmd->execute(cli.commandArgs(), ctx);

    if (ctx.hasError()) {
        fprintf(stderr, "Error: %s\n", qPrintable(ctx.errorString()));
        return 1;
    }

    return result;
}

int main(int argc, char* argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

    // 构建参数列表
    QStringList args;
    for (int i = 0; i < argc; ++i) {
        args << QString::fromLocal8Bit(argv[i]);
    }

    // 检查是否为纯 CLI 模式
    bool forceCLI = args.contains("--cli") || args.contains("-c");

    // 检查是否有任何 CLI 选项（这些都会触发 CLI 模式）
    bool hasCLIOptions = forceCLI ||
                          args.contains("--help") || args.contains("-h") ||
                          args.contains("--version") || args.contains("-V");

    // 检查是否有子命令
    bool hasCommand = false;
    for (int i = 1; i < args.size(); ++i) {
        QString a = args[i];
        if (a != "--cli" && a != "-c" && a != "--gui" && a != "-g" &&
            a != "--verbose" && a != "-v" && a != "--help" && a != "-h" &&
            a != "--version" && a != "-V") {
            hasCommand = true;
            break;
        }
    }

    // 如果有 CLI 选项或子命令且不是强制 GUI 模式，使用 CLI
    if ((hasCLIOptions || hasCommand) && !args.contains("--gui") && !args.contains("-g")) {
        // 初始化必要的管理器
        initAppDirectories();
        ProjectManager::instance();
        PluginManager::instance().initialize();

        return runCLI(args);
    }

    // 否则启动 GUI 模式
    QApplication app(argc, argv);

    app.setApplicationName("DeepLux");
    app.setApplicationVersion(DEEPLUX_VERSION_STRING);
    app.setOrganizationName("DeepLux");

    qDebug() << "DeepLux Vision" << DEEPLUX_VERSION_STRING;
    qDebug() << "Platform:" << DEEPLUX_PLATFORM_NAME;

    initAppDirectories();

    // 初始化管理器
    DeepLux::ProjectManager::instance();

    if (!checkOpenCV()) {
        QMessageBox::critical(nullptr,
            QObject::tr("OpenCV 错误"),
            QObject::tr("OpenCV 初始化失败"));
        return -1;
    }

    setupStyleSheet(app);

    MainWindow mainWindow;
    mainWindow.setWindowTitle("DeepLux Vision");
    mainWindow.resize(1600, 900);

    return app.exec();
}
