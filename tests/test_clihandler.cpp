#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextCodec>
#include <QtTest/QtTest>
#include <clocale>
#include <core/common/CLIHandler.h>
#include <core/engine/RunEngine.h>
#include <core/manager/PluginManager.h>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>

using namespace DeepLux;

class TestCLIHandler : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void testRunCommandRequiresProjectPath();
    void testRunCommandRejectsMissingProjectFile();
    void testRunCommandReportsEmptyProjectAsExecutionFailure();
    void testRunCommandExecutesProjectWithAvailablePlugin();
    void testListPluginsLineEmitsUtf8();

private:
    bool installSystemTimePlugin(const QString& pluginRoot) const;
};

void TestCLIHandler::init() {
    ProjectManager::instance().closeProject();
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
}

void TestCLIHandler::cleanup() {
    ProjectManager::instance().closeProject();
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
    PluginManager::instance().shutdown();
    qunsetenv("DEEPLUX_APP_DATA_DIR");
}

bool TestCLIHandler::installSystemTimePlugin(const QString& pluginRoot) const {
    QDir root(pluginRoot);
    if (!root.mkpath("SystemTime")) {
        return false;
    }

    QDir pluginDir(root.filePath("SystemTime"));
    // The plugin library/metadata paths are injected by CMake via
    // $<TARGET_FILE:...>: never hardcode lib*.so, which does not exist on Windows.
#ifdef TEST_CLIHANDLER_PLUGIN_SystemTime
    const QString libSrc = QString::fromUtf8(TEST_CLIHANDLER_PLUGIN_SystemTime);
    const QString metadataSrc = QString::fromUtf8(TEST_CLIHANDLER_META_SystemTime);
#else
    const QString metadataSrc =
        QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../src/plugins/system/SystemTime/metadata.json");
    const QString libSrc = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../lib/libSystemTimePlugin.so");
#endif
    const QString libName = QFileInfo(libSrc).fileName();

    if (!QFileInfo::exists(metadataSrc) || !QFileInfo::exists(libSrc)) {
        return false;
    }

    QFile::remove(pluginDir.filePath("metadata.json"));
    QFile::remove(pluginDir.filePath(libName));

    return QFile::copy(metadataSrc, pluginDir.filePath("metadata.json")) &&
           QFile::copy(libSrc, pluginDir.filePath(libName));
}

void TestCLIHandler::testRunCommandRequiresProjectPath() {
    ICommand* run = CLIHandler::instance().findCommand("run");
    QVERIFY(run != nullptr);

    CommandContext ctx;
    QCOMPARE(run->execute({}, ctx), 1);
    QVERIFY(ctx.errorString().contains("工程文件"));
}

void TestCLIHandler::testRunCommandRejectsMissingProjectFile() {
    ICommand* run = CLIHandler::instance().findCommand("run");
    QVERIFY(run != nullptr);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    CommandContext ctx;
    QCOMPARE(run->execute({dir.filePath("missing.dproj")}, ctx), 1);
    QVERIFY(ctx.errorString().contains("文件不存在"));
}

void TestCLIHandler::testRunCommandReportsEmptyProjectAsExecutionFailure() {
    ICommand* run = CLIHandler::instance().findCommand("run");
    QVERIFY(run != nullptr);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("empty.dproj");

    Project project;
    QVERIFY(project.save(path));

    CommandContext ctx;
    QCOMPARE(run->execute({path}, ctx), 1);
    QVERIFY(ctx.errorString().contains("No modules to run"));
    QVERIFY(RunEngine::instance().isStopped() || RunEngine::instance().state() == RunState::Idle);
}

void TestCLIHandler::testRunCommandExecutesProjectWithAvailablePlugin() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString appDataPath = dir.filePath("appdata");
    qputenv("DEEPLUX_APP_DATA_DIR", appDataPath.toLocal8Bit());

    const QString pluginRoot = dir.filePath("plugins");
    QVERIFY(installSystemTimePlugin(pluginRoot));

    PluginManager::instance().shutdown();
    PluginManager::instance().addPluginPath(pluginRoot);
    QVERIFY(PluginManager::instance().initialize());
    QVERIFY(PluginManager::instance().availableModules().contains("SystemTime"));

    const QString projectPath = dir.filePath("system-time.dproj");
    Project project;
    ModuleInstance module;
    module.id = "system_time_1";
    module.moduleId = "SystemTime";
    module.name = "System Time";
    project.addModule(module);
    QVERIFY(project.save(projectPath));

    ICommand* run = CLIHandler::instance().findCommand("run");
    QVERIFY(run != nullptr);

    CommandContext ctx;
    QCOMPARE(run->execute({projectPath}, ctx), 0);
    QVERIFY2(!ctx.hasError(), qPrintable(ctx.errorString()));
    QCOMPARE(RunEngine::instance().successRuns(), 1);
}

void TestCLIHandler::testListPluginsLineEmitsUtf8() {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Simulate a CP936 (GBK) Windows locale so a toLocal8Bit()/qPrintable()
    // regression actually changes the bytes here, not just on Windows.
    QTextCodec* gbk = QTextCodec::codecForName("GBK");
    if (!gbk)
        QSKIP("GBK codec not available in this Qt build");

    QTextCodec* previous = QTextCodec::codecForLocale();
    QTextCodec::setCodecForLocale(gbk);
    const QByteArray gbkBytes = QStringLiteral("变量赋值").toLocal8Bit();
    const QByteArray line = CLIHandler::formatPluginLine(QStringLiteral("OK"), QStringLiteral("变量赋值"),
                                                         QStringLiteral("1.0.0"), QStringLiteral("variable"));
    QTextCodec::setCodecForLocale(previous);

    // Must be UTF-8 bytes, and must not be the GBK local-encoding bytes.
    QVERIFY2(line.contains(QStringLiteral("变量赋值").toUtf8()), "plugin line must contain UTF-8 bytes");
    QVERIFY2(!line.contains(gbkBytes), "plugin line must not use the local (GBK) encoding");
    QVERIFY(QString::fromUtf8(line).contains(QStringLiteral("变量赋值")));
#else
    QSKIP("Qt6 is UTF-8 everywhere; the toUtf8 contract is covered by code review");
#endif
}

QTEST_MAIN(TestCLIHandler)
#include "test_clihandler.moc"
