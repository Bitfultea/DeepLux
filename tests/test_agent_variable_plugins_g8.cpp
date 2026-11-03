#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "core/engine/RunEngine.h"
#include "core/manager/GlobalVarManager.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestAgentVariablePluginsG8 : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appDataDir;
    QByteArray m_previousAppDataDir;
    bool m_hadAppDataDir = false;

    static bool installPlugin(const QString& root, const QString& name, const QString& library,
                              const QString& metadata) {
        const QString pluginDirectory = QDir(root).filePath(name);
        if (!QDir().mkpath(pluginDirectory))
            return false;
        const QString libraryName = QFileInfo(library).fileName();
        return QFile::copy(library, QDir(pluginDirectory).filePath(libraryName)) &&
               QFile::copy(metadata, QDir(pluginDirectory).filePath(QStringLiteral("metadata.json")));
    }

    static QJsonObject setParams(AgentActor& actor, const QString& instanceId, const QJsonObject& values) {
        return actor.executeTool(
            QStringLiteral("set_module_params"),
            QJsonObject{{QStringLiteral("instanceId"), instanceId}, {QStringLiteral("values"), values}});
    }

    static QJsonObject runToTerminal(AgentActor& actor) {
        const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"),
                                                      QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        if (started.value(QStringLiteral("status")).toString() != QLatin1String("started"))
            return started;

        const QString runId = started.value(QStringLiteral("runId")).toString();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            const QJsonObject result =
                actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
            const QString status = result.value(QStringLiteral("status")).toString();
            if (status == QLatin1String("completed") || status == QLatin1String("failed") ||
                status == QLatin1String("cancelled") || status == QLatin1String("uncertain"))
                return result;
            QTest::qWait(5);
        }
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("test_timeout")}};
    }

private slots:
    void initTestCase() {
        QVERIFY(m_appDataDir.isValid());
        m_hadAppDataDir = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
        m_previousAppDataDir = qgetenv("DEEPLUX_APP_DATA_DIR");
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());

        const QString pluginRoot = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins"));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("VarDefinePlugin"), QStringLiteral(G8_VARDEFINE_PLUGIN),
                              QStringLiteral(G8_VARDEFINE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("VarSetPlugin"), QStringLiteral(G8_VARSET_PLUGIN),
                              QStringLiteral(G8_VARSET_METADATA)));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        QCOMPARE(plugins.pluginPaths(), QStringList{QDir(pluginRoot).absolutePath()});
        QCOMPARE(plugins.availableModules().size(), 2);
        ToolSchema::instance().registerDefaultTools();
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        qRegisterMetaType<RunResult>("DeepLux::RunResult");
    }

    void cleanupTestCase() {
        RunEngine& engine = RunEngine::instance();
        if (engine.isBusy()) {
            engine.stop();
            QElapsedTimer timer;
            timer.start();
            while (engine.isBusy() && timer.elapsed() < 5000)
                QTest::qWait(10);
            if (engine.isBusy()) {
                QFAIL("RunEngine did not stop within 5 seconds; leaving project and plugin resources intact");
                return;
            }
        }
        ProjectManager::instance().closeProject();
        GlobalVarManager::instance().clearAllVariables();
        PluginManager::instance().shutdown();
        if (m_hadAppDataDir)
            qputenv("DEEPLUX_APP_DATA_DIR", m_previousAppDataDir);
        else
            qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void testLocalizedPluginNamesConfigureRunAndReportBusinessResults() {
        AgentActor actor;
        const QJsonObject created = actor.executeTool(
            QStringLiteral("create_project"), QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 variables")}});
        QCOMPARE(created.value(QStringLiteral("status")).toString(), QStringLiteral("created"));

        PluginManager& plugins = PluginManager::instance();
        const QMap<QString, QString> pluginIds{
            {QStringLiteral("变量定义"), QStringLiteral("com.deeplux.plugin.vardefine")},
            {QStringLiteral("变量赋值"), QStringLiteral("com.deeplux.plugin.varset")}};
        for (auto it = pluginIds.cbegin(); it != pluginIds.cend(); ++it) {
            const PluginInfo info = plugins.pluginInfo(it.key());
            QCOMPARE(info.id, it.value());
            QCOMPARE(info.name, it.key()); // Metadata display name is also the PluginManager registration key.
            const QJsonObject loaded = actor.executeTool(QStringLiteral("load_module_plugin"),
                                                         QJsonObject{{QStringLiteral("plugin"), it.key()}});
            QCOMPARE(loaded.value(QStringLiteral("status")).toString(), QStringLiteral("loaded"));
        }

        const QJsonObject define =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.vardefine")},
                                          {QStringLiteral("instanceName"), QStringLiteral("define_value")}});
        QCOMPARE(define.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString defineId = define.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(ProjectManager::instance().currentProject()->moduleById(defineId)->moduleId,
                 QStringLiteral("变量定义"));

        const QJsonObject set = actor.executeTool(
            QStringLiteral("add_module"), QJsonObject{{QStringLiteral("plugin"), QStringLiteral("变量赋值")},
                                                      {QStringLiteral("instanceName"), QStringLiteral("set_value")}});
        QCOMPARE(set.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString setId = set.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(ProjectManager::instance().currentProject()->moduleById(setId)->moduleId, QStringLiteral("变量赋值"));

        const QJsonObject defineConfigured =
            setParams(actor, defineId,
                      QJsonObject{{QStringLiteral("variableName"), QStringLiteral("g8_agent_value")},
                                  {QStringLiteral("variableType"), QStringLiteral("double")},
                                  {QStringLiteral("initialValue"), 2.5}});
        QCOMPARE(defineConfigured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject setConfigured =
            setParams(actor, setId,
                      QJsonObject{{QStringLiteral("variableName"), QStringLiteral("g8_agent_value")},
                                  {QStringLiteral("value"), QStringLiteral("2 + 5.25")}});
        QCOMPARE(setConfigured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));

        const QJsonObject disableSet =
            actor.executeTool(QStringLiteral("set_module_enabled"),
                              QJsonObject{{QStringLiteral("instanceId"), setId}, {QStringLiteral("enabled"), false}});
        QCOMPARE(disableSet.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
        const QJsonObject defineOnlyRun = runToTerminal(actor);
        QCOMPARE(defineOnlyRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QJsonObject initialVariable =
            actor.executeTool(QStringLiteral("get_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("g8_agent_value")}});
        QCOMPARE(initialVariable.value(QStringLiteral("value")).toDouble(), 2.5);

        const QJsonObject enableSet =
            actor.executeTool(QStringLiteral("set_module_enabled"),
                              QJsonObject{{QStringLiteral("instanceId"), setId}, {QStringLiteral("enabled"), true}});
        QCOMPARE(enableSet.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QJsonObject variable =
            actor.executeTool(QStringLiteral("get_global_variable"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("g8_agent_value")}});
        QCOMPARE(variable.value(QStringLiteral("value")).toDouble(), 7.25);

        QCOMPARE(setParams(actor, defineId, QJsonObject{{QStringLiteral("variableName"), QString()}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject defineFailure = runToTerminal(actor);
        QCOMPARE(defineFailure.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject defineError = defineFailure.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(defineError.value(QStringLiteral("moduleId")).toString(), defineId);
        QVERIFY(!defineError.value(QStringLiteral("message")).toString().isEmpty());

        QCOMPARE(
            setParams(actor, defineId, QJsonObject{{QStringLiteral("variableName"), QStringLiteral("g8_agent_value")}})
                .value(QStringLiteral("status"))
                .toString(),
            QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, setId,
                           QJsonObject{{QStringLiteral("variableName"), QStringLiteral("missing_g8_variable")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject setFailure = runToTerminal(actor);
        QCOMPARE(setFailure.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject setError = setFailure.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(setError.value(QStringLiteral("moduleId")).toString(), setId);
        QVERIFY(!setError.value(QStringLiteral("message")).toString().isEmpty());
    }
};

QTEST_MAIN(TestAgentVariablePluginsG8)
#include "test_agent_variable_plugins_g8.moc"
