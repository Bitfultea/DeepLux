#include <QtTest/QtTest>
#include <core/common/CLIHandler.h>
#include <core/engine/RunEngine.h>
#include <core/manager/PluginManager.h>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

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
    const QString metadataSrc =
        QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../src/plugins/system/SystemTime/metadata.json");
    const QString libSrc =
        QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../lib/libSystemTimePlugin.so");

    if (!QFileInfo::exists(metadataSrc) || !QFileInfo::exists(libSrc)) {
        return false;
    }

    QFile::remove(pluginDir.filePath("metadata.json"));
    QFile::remove(pluginDir.filePath("libSystemTimePlugin.so"));

    return QFile::copy(metadataSrc, pluginDir.filePath("metadata.json")) &&
           QFile::copy(libSrc, pluginDir.filePath("libSystemTimePlugin.so"));
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

QTEST_MAIN(TestCLIHandler)
#include "test_clihandler.moc"
