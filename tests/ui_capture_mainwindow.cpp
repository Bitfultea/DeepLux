#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QDockWidget>
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

// 阶段 5：截图自校验——文件必须存在、可解码、尺寸正确、非空白，
// 任一不满足即判定截图失败（注册为 CTest 后使测试失败）。
// 注意：不做全局内容唯一性校验——截图含时间/日志等动态区域，
// 全局哈希既可能因空转但时间变化而漏报，也可能因合法同屏而误报；
// 状态真实性由 captureClickedStates 中的控件状态断言与针对性前后
// meanAbsDiff 比较保证。
bool verifyCapture(const QString& filePath, const QSize& expectedSize) {
    const QImage image(filePath);
    if (image.isNull()) {
        qWarning("capture missing or unreadable: %s", qPrintable(filePath));
        return false;
    }
    if (image.size() != expectedSize) {
        qWarning("capture size mismatch: %s got %dx%d want %dx%d", qPrintable(filePath), image.width(), image.height(),
                 expectedSize.width(), expectedSize.height());
        return false;
    }
    // 非空白：缩采样后统计灰度方差，纯色/空白画面方差≈0
    const QImage sampled =
        image.convertToFormat(QImage::Format_RGB32).scaled(160, 100, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    qint64 sum = 0;
    qint64 square = 0;
    int count = 0;
    for (int y = 0; y < sampled.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(sampled.constScanLine(y));
        for (int x = 0; x < sampled.width(); ++x) {
            const int gray = qGray(line[x]);
            sum += gray;
            square += gray * gray;
            ++count;
        }
    }
    const double mean = static_cast<double>(sum) / count;
    const double variance = static_cast<double>(square) / count - mean * mean;
    if (variance < 1.0) {
        qWarning("capture appears blank (variance %.2f): %s", variance, qPrintable(filePath));
        return false;
    }
    return true;
}

bool captureWindow(DeepLux::MainWindow& window, const QSize& size, const QString& filePath) {
    window.resize(size);
    window.show();
    QCoreApplication::processEvents();
    QTest::qWait(120);
    const QPixmap shot = window.grab();
    const qreal dpr = window.devicePixelRatio();
    const QSize expected(qRound(size.width() * dpr), qRound(size.height() * dpr));
    if (shot.size() != expected) {
        qWarning("grab size mismatch for %s: got %dx%d want %dx%d", qPrintable(filePath), shot.width(), shot.height(),
                 expected.width(), expected.height());
        return false;
    }
    if (!shot.save(filePath))
        return false;
    return verifyCapture(filePath, expected);
}

bool saveShot(DeepLux::MainWindow& window, const QDir& dir, const QString& name) {
    QCoreApplication::processEvents();
    QTest::qWait(120);
    const QPixmap shot = window.grab();
    const QString filePath = dir.filePath(name);
    if (!shot.save(filePath))
        return false;
    return verifyCapture(filePath, shot.size());
}

// 两张截图的平均绝对灰度差，用于断言主题切换等外观变化真实发生
// （避免"动作没生效但截图非空白"的假阳性）。
double meanAbsDiff(const QImage& a, const QImage& b) {
    const QImage ia =
        a.convertToFormat(QImage::Format_RGB32).scaled(160, 100, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    const QImage ib =
        b.convertToFormat(QImage::Format_RGB32).scaled(160, 100, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    if (ia.size() != ib.size())
        return 0.0;
    qint64 total = 0;
    int count = 0;
    for (int y = 0; y < ia.height(); ++y) {
        const QRgb* la = reinterpret_cast<const QRgb*>(ia.constScanLine(y));
        const QRgb* lb = reinterpret_cast<const QRgb*>(ib.constScanLine(y));
        for (int x = 0; x < ia.width(); ++x) {
            total += qAbs(qGray(la[x]) - qGray(lb[x]));
            ++count;
        }
    }
    return count ? static_cast<double>(total) / count : 0.0;
}

QTabWidget* tabsByName(DeepLux::MainWindow& window, const char* objectName) {
    return window.findChild<QTabWidget*>(QString::fromLatin1(objectName));
}

// 前置声明：按文本触发 QAction（定义在下方正式尺寸截图部分）
bool triggerActionByText(DeepLux::MainWindow& window, const QString& text);

// 点击切换页签并验证真实生效；控件缺失/隐藏、索引无效或点击后
// currentIndex 未变化都返回 false（不得静默跳过，否则截图门禁出现假阳性）。
bool clickTabChecked(QTabWidget* tabs, const QString& text, const char* what) {
    if (!tabs) {
        qWarning("%s: tab widget missing", what);
        return false;
    }
    if (!tabs->isVisible()) {
        qWarning("%s: tab widget hidden", what);
        return false;
    }
    int index = -1;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == text) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        qWarning("%s: tab '%s' not found", what, qPrintable(text));
        return false;
    }
    QTabBar* bar = tabs->tabBar();
    if (!bar || !bar->isVisible()) {
        qWarning("%s: tab bar hidden", what);
        return false;
    }
    const QRect tabRect = bar->tabRect(index);
    if (!tabRect.isValid()) {
        qWarning("%s: tab rect invalid for index %d", what, index);
        return false;
    }
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, tabRect.center());
    QCoreApplication::processEvents();
    if (tabs->currentIndex() != index) {
        qWarning("%s: click did not switch to tab '%s' (currentIndex=%d)", what, qPrintable(text),
                 tabs->currentIndex());
        return false;
    }
    return true;
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

// 阶段 5 复核（P1-3）：点击态截图必须"状态真实发生 + 截图真实变化"。
// 历史假阳性根因：窗口 1024 宽 < 自适应阈值 1100 → 工具面板被自动隐藏，
// "展开工具图标/关闭工具面板"空转；画布页签此前已被切中，点击无变化。
// 现固定初始状态（加宽窗口、强制工具面板可见、页签归位），并逐步断言
// currentIndex、控件可见性、关闭后状态与前后截图差异。
bool captureClickedStates(DeepLux::MainWindow& window, const QDir& dir) {
    bool ok = true;

    // 01: 紧凑窗口初始态（1024）
    window.resize(QSize(1024, 700));
    window.show();
    QCoreApplication::processEvents();
    QTest::qWait(300);
    ok = saveShot(window, dir, QStringLiteral("01-initial-1024.png")) && ok;

    // 固定初始状态：加宽窗口使自适应布局不再隐藏工具面板（阈值 1100）
    window.resize(QSize(1280, 800));
    window.show();
    QCoreApplication::processEvents();
    QTest::qWait(250);

    QDockWidget* toolDock = window.findChild<QDockWidget*>(QStringLiteral("ToolPanelDock"));
    if (!toolDock) {
        qWarning("tool dock not found");
        return false;
    }
    // 强制工具面板可见（若被用户态/自适应隐藏，则经"视图→工具"动作打开）
    if (!toolDock->isVisible()) {
        if (!triggerActionByText(window, QStringLiteral("工具"))) {
            qWarning("failed to trigger tool panel view action");
            return false;
        }
        QCoreApplication::processEvents();
        QTest::qWait(200);
    }
    if (!toolDock->isVisible()) {
        qWarning("tool panel still hidden after forcing open");
        return false;
    }

    // 02/03: 流程页签——先归位到非目标页，再点击切换并断言 currentIndex 与截图变化
    QTabWidget* processTabs = tabsByName(window, "ProcessTabWidget");
    if (!processTabs) {
        qWarning("process tab widget not found");
        return false;
    }
    processTabs->setCurrentIndex(0);
    QCoreApplication::processEvents();
    QTest::qWait(150);
    const QImage beforeCanvas = window.grab().toImage();
    if (!clickTabChecked(processTabs, QStringLiteral("画布"), "02-process-canvas-tab"))
        return false;
    QTest::qWait(150);
    ok = saveShot(window, dir, QStringLiteral("02-process-canvas-tab.png")) && ok;
    if (meanAbsDiff(beforeCanvas, window.grab().toImage()) < 1.0) {
        qWarning("switching to canvas tab did not change appearance");
        return false;
    }
    if (!clickTabChecked(processTabs, QStringLiteral("数据源"), "03-process-datasource-tab"))
        return false;
    QTest::qWait(150);
    ok = saveShot(window, dir, QStringLiteral("03-process-datasource-tab.png")) && ok;

    // 09: 展开工具分类图标——断言树可见、确有分类被展开、且截图相对展开前变化。
    // 置于页签截图之后：此时工具树尚未展开，09 与 02/03 的内容必然不同。
    QTreeWidget* toolTree = window.findChild<QTreeWidget*>(QStringLiteral("ToolBoxTree"));
    if (!toolTree) {
        qWarning("tool tree not found");
        return false;
    }
    if (!toolTree->isVisible()) {
        qWarning("tool tree hidden");
        return false;
    }
    const QImage beforeExpand = window.grab().toImage();
    int expanded = 0;
    for (int i = 0; i < toolTree->topLevelItemCount(); ++i) {
        const bool shouldExpand = (i == 1 || i == 3 || i == 4 || i == 7);
        toolTree->topLevelItem(i)->setExpanded(shouldExpand);
        if (shouldExpand && toolTree->topLevelItem(i)->isExpanded())
            ++expanded;
    }
    QCoreApplication::processEvents();
    QTest::qWait(200);
    if (expanded < 4) {
        qWarning("tool tree expansion incomplete: %d/4", expanded);
        return false;
    }
    ok = saveShot(window, dir, QStringLiteral("09-tool-plugin-icons.png")) && ok;
    const double expandDiff = meanAbsDiff(beforeExpand, window.grab().toImage());
    if (expandDiff < 1.0) {
        qWarning("tool tree expansion did not change appearance (mean diff %.2f)", expandDiff);
        return false;
    }

    // 04/05/06: 底部页签（终端 / Agent 对话 / Agent 日志）
    QTabWidget* bottomTabs = tabsByName(window, "LogTerminalTabs");
    if (!bottomTabs) {
        qWarning("bottom tab widget not found");
        return false;
    }
    if (!clickTabChecked(bottomTabs, QStringLiteral("终端"), "04-bottom-terminal-tab"))
        return false;
    QTest::qWait(150);
    ok = saveShot(window, dir, QStringLiteral("04-bottom-terminal-tab.png")) && ok;

    seedAgentChatDemo(window);
    if (!clickTabChecked(bottomTabs, QStringLiteral("Agent 对话"), "05-bottom-agent-chat-tab"))
        return false;
    QTest::qWait(150);
    ok = saveShot(window, dir, QStringLiteral("05-bottom-agent-chat-tab.png")) && ok;

    if (!clickTabChecked(bottomTabs, QStringLiteral("Agent 日志"), "06-bottom-agent-log-tab"))
        return false;
    QTest::qWait(150);
    ok = saveShot(window, dir, QStringLiteral("06-bottom-agent-log-tab.png")) && ok;

    // 07: "切换主题"动作位于"视图"菜单而非主工具栏：按文本查找 QAction 并 trigger，
    // 且断言外观真实变化，避免"动作未生效但截图非空白"的假阳性。
    const QImage beforeThemeToggle = window.grab().toImage();
    bool themeToggled = false;
    for (QAction* action : window.findChildren<QAction*>()) {
        if (action->text().remove('&') == QStringLiteral("切换主题")) {
            action->trigger();
            themeToggled = true;
            break;
        }
    }
    QCoreApplication::processEvents();
    QTest::qWait(250);
    if (!themeToggled) {
        qWarning("theme toggle action not found");
        return false;
    }
    const double themeDiff = meanAbsDiff(beforeThemeToggle, window.grab().toImage());
    if (themeDiff < 10.0) {
        qWarning("theme toggle did not change appearance (mean diff %.2f)", themeDiff);
        return false;
    }
    ok = saveShot(window, dir, QStringLiteral("07-theme-toggle.png")) && ok;

    // 08: 关闭工具面板——前置断言面板在打开态，点击关闭按钮后断言
    // dock 隐藏、"视图→工具"动作取消勾选、且截图相对关闭前真实变化。
    // （P1-3 深色退出回归：此后不再切回浅色，本测试以深色主题走到进程退出；
    //   若复现内存写越界，CTest 将因 abort 判失败。）
    if (!toolDock->isVisible()) {
        qWarning("tool panel must be visible before close test");
        return false;
    }
    QToolButton* toolClose = window.findChild<QToolButton*>(QStringLiteral("ToolCloseBtn"));
    if (!toolClose || !toolClose->isVisible()) {
        qWarning("tool close button missing or hidden");
        return false;
    }
    const QImage beforeClose = window.grab().toImage();
    QTest::mouseClick(toolClose, Qt::LeftButton, Qt::NoModifier, toolClose->rect().center());
    QCoreApplication::processEvents();
    QTest::qWait(200);
    if (toolDock->isVisible()) {
        qWarning("tool panel still visible after clicking close");
        return false;
    }
    ok = saveShot(window, dir, QStringLiteral("08-tool-panel-closed.png")) && ok;
    const double closeDiff = meanAbsDiff(beforeClose, window.grab().toImage());
    if (closeDiff < 1.0) {
        qWarning("closing tool panel did not change appearance (mean diff %.2f)", closeDiff);
        return false;
    }

    return ok;
}

// 收尾2: 正式尺寸截图 1920/1280 深浅（默认浅色起始，切换主题采集深色）
// "切换主题"动作位于"视图"菜单而非工具栏，需按文本查找 QAction 并 trigger
bool triggerActionByText(DeepLux::MainWindow& window, const QString& text) {
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

// 收尾2: 安装插件到临时目录，供截图工程加载。
// 插件库路径由 CMake 以 $<TARGET_FILE:...> 注入（libSrc），不在测试代码拼接库名，
// 以适配不同平台的库文件命名（.so/.dll/.dylib）。
bool installPluginForCapture(const QString& repoRoot, const QString& pluginTempRoot, const QString& dirName,
                             const QString& metadataRel, const QString& libSrc) {
    const QString pluginDir = pluginTempRoot + "/" + dirName;
    if (!QDir().mkpath(pluginDir))
        return false;
    const QString metaSrc = QDir(repoRoot).filePath(metadataRel);
    if (!QFileInfo::exists(metaSrc) || !QFileInfo::exists(libSrc))
        return false;
    const QString destLibName = QFileInfo(libSrc).fileName();
    QFile::remove(pluginDir + "/metadata.json");
    QFile::remove(pluginDir + "/" + destLibName);
    return QFile::copy(metaSrc, pluginDir + "/metadata.json") && QFile::copy(libSrc, pluginDir + "/" + destLibName);
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

    // 安装并加载所需插件（库路径由 CMake TARGET_FILE 注入）
    if (!installPluginForCapture(repoRoot, pluginTempRoot, "GrabImage",
                                 "src/plugins/image_processing/GrabImage/metadata.json",
                                 QStringLiteral(UICAP_PLUGIN_GrabImage)))
        return false;
    if (!installPluginForCapture(repoRoot, pluginTempRoot, "FindCircle",
                                 "src/plugins/detection/FindCircle/metadata.json",
                                 QStringLiteral(UICAP_PLUGIN_FindCircle)))
        return false;
    if (!installPluginForCapture(repoRoot, pluginTempRoot, "LoadPointCloud",
                                 "src/plugins/image_processing/LoadPointCloud/metadata.json",
                                 QStringLiteral(UICAP_PLUGIN_LoadPointCloud)))
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
                                 QStringLiteral(UICAP_PLUGIN_MeasurementInput)) ||
        !installPluginForCapture(repoRoot, pluginTempRoot, "FitCircle", "src/plugins/geometry/FitCircle/metadata.json",
                                 QStringLiteral(UICAP_PLUGIN_FitCircle))) {
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
                                 QStringLiteral(UICAP_PLUGIN_If)) ||
        !installPluginForCapture(repoRoot, pluginTempRoot, "Delay", "src/plugins/logic/Delay/metadata.json",
                                 QStringLiteral(UICAP_PLUGIN_Delay))) {
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
    if (!clickTabChecked(processTabs, QStringLiteral("画布"), "controlflow-canvas"))
        return false;

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
