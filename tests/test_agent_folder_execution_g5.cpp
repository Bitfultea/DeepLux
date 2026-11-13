#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/base/ModuleBase.h"
#include "core/common/UserManager.h"
#include "core/engine/FlowExecutionService.h"
#include "core/engine/RunEngine.h"
#include "core/interface/IModule.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <atomic>
#include <cstdio>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/core.hpp>
#endif

#ifdef Q_OS_LINUX
#include <sys/stat.h>
#endif

using namespace DeepLux;

namespace {

struct FilePermissionRestore {
    QString path;
    QFile::Permissions permissions;

    bool restore() {
        if (path.isEmpty())
            return true;
        const bool restored = QFile::setPermissions(path, permissions);
        if (restored)
            path.clear();
        return restored;
    }

    ~FilePermissionRestore() {
        restore();
    }
};

struct FolderTestStateRestore {
    AgentController& controller;
    RunEngine& engine;
    QTemporaryDir& targets;
    QTemporaryDir& appDataDir;

    ~FolderTestStateRestore() {
        engine.stop();
        QElapsedTimer wait;
        wait.start();
        while (engine.isBusy() && wait.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        if (engine.isBusy()) {
            targets.setAutoRemove(false);
            appDataDir.setAutoRemove(false);
            qCritical() << "Folder cycle cleanup timed out; preserving test directories" << targets.path()
                        << appDataDir.path();
            return;
        }
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }
};

} // namespace

class FolderExecutionMockLLMClient : public ILLMClient {
    Q_OBJECT

public:
    using ILLMClient::ILLMClient;

    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}

    AgentConversation lastContext;

    void sendRequest(const AgentConversation& context, const QList<ToolDefinition>&) override {
        lastContext = context;
    }

    void respond(const AgentResponse& response) {
        AgentResponse correlated = response;
        if (correlated.requestId.isEmpty())
            correlated.requestId = lastContext.requestId;
        emit responseReceived(correlated);
    }
};

class TestAgentFolderExecutionG5 : public QObject {
    Q_OBJECT

private:
    struct EnvironmentRestore {
        QByteArray appDataDir = qgetenv("DEEPLUX_APP_DATA_DIR");
        bool hadAppDataDir = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");

        ~EnvironmentRestore() {
            if (hadAppDataDir)
                qputenv("DEEPLUX_APP_DATA_DIR", appDataDir);
            else
                qunsetenv("DEEPLUX_APP_DATA_DIR");
        }
    };

    EnvironmentRestore m_environmentRestore;
    QTemporaryDir m_appDataDir;
    QTemporaryDir m_targets;

    static bool installFolderPlugin(const QString& pluginRoot) {
        const QDir folderDir(pluginRoot + QStringLiteral("/Folder"));
        if (!QDir().mkpath(folderDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_FOLDER_G5_METADATA),
                           folderDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_FOLDER_G5_LIBRARY),
                           folderDir.filePath(QFileInfo(QStringLiteral(AGENT_FOLDER_G5_LIBRARY)).fileName()));
    }

    static bool installWriteTextPlugin(const QString& pluginRoot) {
        const QDir writeTextDir(pluginRoot + QStringLiteral("/WriteText"));
        if (!QDir().mkpath(writeTextDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_WRITETEXT_G5_METADATA),
                           writeTextDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_WRITETEXT_G5_LIBRARY),
                           writeTextDir.filePath(QFileInfo(QStringLiteral(AGENT_WRITETEXT_G5_LIBRARY)).fileName()));
    }

    static bool installSaveDataPlugin(const QString& pluginRoot) {
        const QDir saveDataDir(pluginRoot + QStringLiteral("/SaveData"));
        if (!QDir().mkpath(saveDataDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_SAVEDATA_G5_METADATA),
                           saveDataDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_SAVEDATA_G5_LIBRARY),
                           saveDataDir.filePath(QFileInfo(QStringLiteral(AGENT_SAVEDATA_G5_LIBRARY)).fileName()));
    }

    static bool installGrabImagePlugin(const QString& pluginRoot) {
        const QDir grabImageDir(pluginRoot + QStringLiteral("/GrabImage"));
        if (!QDir().mkpath(grabImageDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_GRABIMAGE_G5_METADATA),
                           grabImageDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_GRABIMAGE_G5_LIBRARY),
                           grabImageDir.filePath(QFileInfo(QStringLiteral(AGENT_GRABIMAGE_G5_LIBRARY)).fileName()));
    }

    static bool installSaveImagePlugin(const QString& pluginRoot) {
        const QDir saveImageDir(pluginRoot + QStringLiteral("/SaveImage"));
        if (!QDir().mkpath(saveImageDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_SAVEIMAGE_G5_METADATA),
                           saveImageDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_SAVEIMAGE_G5_LIBRARY),
                           saveImageDir.filePath(QFileInfo(QStringLiteral(AGENT_SAVEIMAGE_G5_LIBRARY)).fileName()));
    }

    static int countModuleSignal(const QSignalSpy& spy, const QString& instanceId) {
        int count = 0;
        for (const QList<QVariant>& signalArgs : spy)
            if (!signalArgs.isEmpty() && signalArgs.first().toString() == instanceId)
                ++count;
        return count;
    }

    QString makeDeleteTarget(const QString& name) const {
        const QString target = m_targets.filePath(name);
        if (!QDir().mkpath(target))
            return {};
        QFile probe(QDir(target).filePath(QStringLiteral("probe.txt")));
        if (!probe.open(QIODevice::WriteOnly))
            return {};
        probe.write("slice31\n");
        probe.close();
        return target;
    }

    void verifyChatRejectsUnresolvedFileTarget(AgentController& controller, RunEngine& engine,
                                               FolderExecutionMockLLMClient& mock, QSignalSpy& moduleStarted,
                                               QSignalSpy& moduleFinished, QSignalSpy& runFinished,
                                               QSignalSpy& toolExecuted, QSignalSpy& pendingConfirmation,
                                               QSignalSpy& audit, const QString& instanceId, const QString& toolName,
                                               const QString& callId, const QString& targetDirectory,
                                               bool expectPaused) {
        const int startedBefore = countModuleSignal(moduleStarted, instanceId);
        const int finishedBefore = countModuleSignal(moduleFinished, instanceId);
        const int runFinishedBefore = runFinished.count();
        const int toolExecutedBefore = toolExecuted.count();
        const int pendingBefore = pendingConfirmation.count();

        controller.sendUserMessage(QStringLiteral("Run the file flow tool."));
        AgentResponse toolResponse;
        toolResponse.success = true;
        toolResponse.toolCalls = QJsonArray{QJsonObject{
            {QStringLiteral("id"), callId},
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), toolName}, {QStringLiteral("arguments"), QStringLiteral("{}")}}}}};
        mock.respond(toolResponse);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);

        QVERIFY(!controller.hasPendingTools());
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QCOMPARE(pendingConfirmation.count(), pendingBefore);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), startedBefore);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), finishedBefore);
        QCOMPARE(runFinished.count(), runFinishedBefore);
        QCOMPARE(toolExecuted.count(), toolExecutedBefore);
        if (expectPaused) {
            QCOMPARE(engine.state(), RunState::Paused);
        } else {
            QVERIFY(!engine.isBusy());
        }
        QVERIFY(QDir(targetDirectory).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, toolName);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));

        controller.sendUserMessage(QStringLiteral("Check the structured tool response."));
        int matchingResults = 0;
        for (const AgentMessage& message : mock.lastContext.messages) {
            if (message.role != QLatin1String("tool") || message.toolCallId != callId)
                continue;
            ++matchingResults;
            const QJsonObject result = QJsonDocument::fromJson(message.content.toUtf8()).object();
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
            QVERIFY(!result.value(QStringLiteral("retryable")).toBool());
        }
        QCOMPARE(matchingResults, 1);
        AgentResponse finalResponse;
        finalResponse.success = true;
        finalResponse.content = QStringLiteral("Unresolved target rejected.");
        mock.respond(finalResponse);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
    }

    void verifyResumeSnapshotUnchanged(RunEngine& engine, const RunResumeSnapshot& expected) {
        const RunResumeSnapshot actual = engine.resumeSnapshot();
        QCOMPARE(actual.runId, expected.runId);
        QCOMPARE(actual.sessionId, expected.sessionId);
        QCOMPARE(actual.state, expected.state);
        QCOMPARE(actual.lifecycleGeneration, expected.lifecycleGeneration);
        QCOMPARE(actual.project.data(), expected.project.data());
        QCOMPARE(actual.projectFingerprint, expected.projectFingerprint);
    }

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        QVERIFY(m_appDataDir.isValid());
        QVERIFY(m_targets.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());

        const QString pluginRoot = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins"));
        QVERIFY(installFolderPlugin(pluginRoot));
        QVERIFY(installWriteTextPlugin(pluginRoot));
        QVERIFY(installSaveDataPlugin(pluginRoot));
        QVERIFY(installGrabImagePlugin(pluginRoot));
        QVERIFY(installSaveImagePlugin(pluginRoot));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        QVERIFY(plugins.pluginPaths().contains(QDir(pluginRoot).absolutePath()));
        const QList<PluginInfo> scannedPlugins = plugins.moduleInfos();
        QCOMPARE(scannedPlugins.size(), 5);
        QStringList discoveredNames;
        for (const PluginInfo& info : scannedPlugins)
            discoveredNames.append(info.name);
        discoveredNames.sort();
        QCOMPARE(discoveredNames,
                 QStringList({QStringLiteral("Folder"), QStringLiteral("GrabImage"), QStringLiteral("SaveData"),
                              QStringLiteral("SaveImage"), QStringLiteral("WriteText")}));
        QVERIFY(plugins.cameraInfos().isEmpty());
        QVERIFY(plugins.loadPlugin(QStringLiteral("Folder")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("SaveData")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("SaveImage")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("WriteText")));
        QVERIFY(AgentController::instance().initialize());
    }

    void cleanupTestCase() {
        RunEngine& engine = RunEngine::instance();
        engine.stop();
        QElapsedTimer wait;
        wait.start();
        while (engine.isBusy() && wait.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QTest::qWait(10);
        }
        const bool engineIdle = !engine.isBusy();
        if (!engineIdle) {
            // Execution may still hold the Folder object and access its target or loaded library.
            // Keep both backing trees on this failure path; process teardown will stop the test.
            m_targets.setAutoRemove(false);
            m_appDataDir.setAutoRemove(false);
            qCritical() << "Folder execution did not become idle; preserving test directories" << m_targets.path()
                        << m_appDataDir.path();
            QFAIL("RunEngine remained busy; temporary plugin and target trees were preserved");
            return;
        }

        engine.clearModules();
        ProjectManager::instance().closeProject();
        AgentController::instance().shutdown();
        PluginManager::instance().shutdown();
    }

    void testWriteTextRunFlowConfirmsConfiguredAndRejectsUnresolvedTarget() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice42_writetext_target");
        const QString outputPath = m_targets.filePath(QStringLiteral("agent-output.txt"));
        const QString outputText = QStringLiteral("agent-confirmed-output");
        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 42 WriteText output");
        writer.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                    {QStringLiteral("textContent"), outputText},
                                    {QStringLiteral("appendMode"), false}};
        project->addModule(writer);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QJsonArray expectedEffects{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                                         QStringLiteral("file_write:%1 format=text mode=overwrite").arg(outputPath)};

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!pending.contains(QStringLiteral("runId")));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects);
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(!engine.isBusy());
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);

        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(!engine.isBusy());
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const QJsonObject runToolResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(runToolResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = runToolResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        const QString runFailure =
            QStringLiteral("%1 (failed module: %2)").arg(result.errorMessage, result.failedModule);
        QVERIFY2(result.success, qPrintable(runFailure));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(QFileInfo::exists(outputPath));
        QFile outputFile(outputPath);
        QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(outputFile.readAll()), outputText + QLatin1Char('\n'));
        outputFile.close();
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject writerOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("write_result")}});
        QCOMPARE(writerOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(writerOutput.value(QStringLiteral("runId")).toString(), runId);
        QVERIFY(writerOutput.value(QStringLiteral("value")).toBool());
        const QJsonObject writerPathOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("write_path")}});
        QCOMPARE(writerPathOutput.value(QStringLiteral("value")).toString(), outputPath);
        const QJsonObject writerLengthOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("write_length")}});
        QCOMPARE(writerLengthOutput.value(QStringLiteral("value")).toInt(), outputText.size());
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(!engine.isBusy());

        QVERIFY(QFile::remove(outputPath));
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("filePath"), QString()));
        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
        QVERIFY(!pending.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!controller.hasPendingTools());
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QVERIFY(!QFileInfo::exists(outputPath));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QVERIFY(!engine.isBusy());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testWriteTextOpenFailurePropagatesThroughAgentRunFlow() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice47_writetext_failure");
        const QString missingDirectory = m_targets.filePath(QStringLiteral("missing-write-parent"));
        const QString outputPath = QDir(missingDirectory).filePath(QStringLiteral("agent-output.txt"));
        QVERIFY(!QDir(missingDirectory).exists());
        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 47 WriteText failure");
        writer.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                    {QStringLiteral("textContent"), QStringLiteral("must fail")},
                                    {QStringLiteral("appendMode"), false}};
        project->addModule(writer);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        const QJsonArray expectedEffects{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                                         QStringLiteral("file_write:%1 format=text mode=overwrite").arg(outputPath)};

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QVERIFY(!QDir(missingDirectory).exists());
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QVERIFY(!QDir(missingDirectory).exists());
        QVERIFY(!QFileInfo::exists(outputPath));

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(started.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);

        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY(!result.success);
        QCOMPARE(result.failedModule, instanceId);
        QVERIFY(result.errorMessage.contains(outputPath));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QVERIFY(!QDir(missingDirectory).exists());
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(!engine.isBusy());

        const QJsonObject runStatus =
            controller.handleToolCall(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(runStatus.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(runStatus.value(QStringLiteral("runId")).toString(), runId);
        QCOMPARE(runStatus.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("moduleId")).toString(),
                 instanceId);
        QVERIFY(runStatus.value(QStringLiteral("lastError"))
                    .toObject()
                    .value(QStringLiteral("message"))
                    .toString()
                    .contains(outputPath));
        const QJsonObject outputStatus = controller.handleToolCall(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), instanceId}});
        QCOMPARE(outputStatus.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(outputStatus.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testWriteTextStepOnceConfirmsConfiguredFileTarget() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice72_step_writetext");
        const QString outputPath = m_targets.filePath(QStringLiteral("slice72-step-output.txt"));
        const QString textContent = QStringLiteral("slice72 step once output");
        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 72 step WriteText");
        writer.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                    {QStringLiteral("textContent"), textContent},
                                    {QStringLiteral("appendMode"), false}};
        project->addModule(writer);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QString fileEffect = QStringLiteral("file_write:%1 format=text mode=overwrite").arg(outputPath);
        const auto expectFileTarget = [&controller, &fileEffect] {
            return controller.pendingConfirmationPayload()
                .value(QStringLiteral("context"))
                .toObject()
                .value(QStringLiteral("sideEffects"))
                .toArray()
                .contains(fileEffect);
        };

        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectFileTarget());
        QVERIFY(!QFileInfo::exists(outputPath));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(!engine.isBusy());
        QVERIFY(!QFileInfo::exists(outputPath));
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectFileTarget());
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject stepResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = stepResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(output.readAll()), textContent + QLatin1Char('\n'));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testWriteTextResumeFlowConfirmsBreakpointFileTarget() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice72_resume_writetext");
        const QString outputPath = m_targets.filePath(QStringLiteral("slice72-resume-output.txt"));
        const QString textContent = QStringLiteral("slice72 resume output");
        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 72 resume WriteText");
        writer.breakpoint = true;
        writer.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                    {QStringLiteral("textContent"), textContent},
                                    {QStringLiteral("appendMode"), false}};
        project->addModule(writer);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        engine.runOnce();
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Paused, 5000);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QVERIFY(!QFileInfo::exists(outputPath));
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);

        const QString fileEffect = QStringLiteral("file_write:%1 format=text mode=overwrite").arg(outputPath);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(fileEffect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, runId);
        QVERIFY(!QFileInfo::exists(outputPath));
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(fileEffect));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));

        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(engine.state(), RunState::Idle);
        QVERIFY(!engine.isBusy());
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(output.readAll()), textContent + QLatin1Char('\n'));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testSaveImageRunFlowConfirmsFileOutput() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString inputPath = m_targets.filePath(QStringLiteral("save-image-input.png"));
        const QString outputPath = m_targets.filePath(QStringLiteral("save-image-output.png"));
        const QRgb pixel = qRgb(32, 96, 160);
        QImage sourceImage(4, 3, QImage::Format_RGB32);
        sourceImage.fill(pixel);
        QVERIFY(sourceImage.save(inputPath, "PNG"));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString sourceId = QStringLiteral("slice44_grabimage_source");
        const QString saveId = QStringLiteral("slice44_saveimage_target");
        ModuleInstance source;
        source.id = sourceId;
        source.moduleId = QStringLiteral("GrabImage");
        source.name = QStringLiteral("Slice 44 temporary image source");
        source.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("File")},
                                    {QStringLiteral("filePath"), inputPath}};
        project->addModule(source);
        ModuleInstance save;
        save.id = saveId;
        save.moduleId = QStringLiteral("SaveImage");
        save.name = QStringLiteral("Slice 44 SaveImage output");
        save.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                  {QStringLiteral("format"), QStringLiteral("png")},
                                  {QStringLiteral("quality"), 95}};
        project->addModule(save);
        ModuleConnection imageEdge;
        imageEdge.fromModuleId = sourceId;
        imageEdge.toModuleId = saveId;
        imageEdge.fromPort = QStringLiteral("image");
        imageEdge.toPort = QStringLiteral("image");
        imageEdge.edgeType = QStringLiteral("data");
        project->addConnection(imageEdge);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QJsonArray expectedEffects{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                                         QStringLiteral("file_write:%1").arg(outputPath)};

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects);
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(!engine.isBusy());
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 0);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);

        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(!engine.isBusy());
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 0);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, sourceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, saveId), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const QJsonObject runToolResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(runToolResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = runToolResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        const QString runFailure =
            QStringLiteral("%1 (failed module: %2)").arg(result.errorMessage, result.failedModule);
        QCOMPARE(moduleFinished.count(), 2);
        QCOMPARE(moduleFinished.at(0).at(0).toString(), sourceId);
        QVERIFY(moduleFinished.at(0).at(1).toBool());
        QVERIFY2(result.success, qPrintable(runFailure));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, saveId), 1);
        QVERIFY(moduleFinished.at(0).at(1).toBool());
        QVERIFY(moduleFinished.at(1).at(1).toBool());
        QImage savedImage;
        QVERIFY(savedImage.load(outputPath));
        QCOMPARE(savedImage.size(), sourceImage.size());
        QCOMPARE(savedImage.pixel(0, 0), pixel);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject imageOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), saveId}});
        QCOMPARE(imageOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(imageOutput.value(QStringLiteral("runId")).toString(), runId);
        const QJsonObject imageSummary = imageOutput.value(QStringLiteral("summary")).toObject();
        QCOMPARE(imageSummary.value(QStringLiteral("instanceId")).toString(), saveId);
        QVERIFY(imageSummary.value(QStringLiteral("hasOutput")).toBool());
        const QJsonObject imageMetadata = imageSummary.value(QStringLiteral("image")).toObject();
        QCOMPARE(imageMetadata.value(QStringLiteral("width")).toInt(), sourceImage.width());
        QCOMPARE(imageMetadata.value(QStringLiteral("height")).toInt(), sourceImage.height());
        QCOMPARE(imageMetadata.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const ImageData savedOutput = engine.moduleOutput(saveId);
        QVERIFY(savedOutput.isValid());
        const QImage passedThroughImage = savedOutput.toQImage();
        QVERIFY(!passedThroughImage.isNull());
        QCOMPARE(passedThroughImage.size(), sourceImage.size());
        QCOMPARE(passedThroughImage.pixel(0, 0), pixel);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testSaveImageStepOnceConfirmsEachOutputStep() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString inputPath = m_targets.filePath(QStringLiteral("slice71-step-input.png"));
        const QString outputPath = m_targets.filePath(QStringLiteral("slice71-step-output.png"));
        const QRgb pixel = qRgb(32, 96, 160);
        QImage sourceImage(4, 3, QImage::Format_RGB32);
        sourceImage.fill(pixel);
        QVERIFY(sourceImage.save(inputPath, "PNG"));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString sourceId = QStringLiteral("slice71_step_grabimage");
        const QString saveId = QStringLiteral("slice71_step_saveimage");
        ModuleInstance source;
        source.id = sourceId;
        source.moduleId = QStringLiteral("GrabImage");
        source.name = QStringLiteral("Slice 71 step image source");
        source.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("File")},
                                    {QStringLiteral("filePath"), inputPath}};
        project->addModule(source);
        ModuleInstance save;
        save.id = saveId;
        save.moduleId = QStringLiteral("SaveImage");
        save.name = QStringLiteral("Slice 71 step SaveImage target");
        save.breakpoint = true;
        save.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                  {QStringLiteral("format"), QStringLiteral("png")},
                                  {QStringLiteral("quality"), 95}};
        project->addModule(save);
        ModuleConnection connection;
        connection.fromModuleId = sourceId;
        connection.toModuleId = saveId;
        connection.fromPort = QStringLiteral("image");
        connection.toPort = QStringLiteral("image");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QString fileEffect = QStringLiteral("file_write:%1").arg(outputPath);
        const auto requestStep = [&controller] { return controller.handleToolCall(QStringLiteral("step_once"), {}); };
        const auto expectFileTarget = [&controller, &fileEffect] {
            const QJsonArray effects = controller.pendingConfirmationPayload()
                                           .value(QStringLiteral("context"))
                                           .toObject()
                                           .value(QStringLiteral("sideEffects"))
                                           .toArray();
            return effects.contains(fileEffect);
        };

        QJsonObject pending = requestStep();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectFileTarget());
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(!engine.isBusy());
        QVERIFY(!QFileInfo::exists(outputPath));
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = requestStep();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectFileTarget());
        const QString sourceConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!sourceConfirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(sourceConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject sourceStep = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(sourceStep.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = sourceStep.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(qvariant_cast<RunResult>(runFinished.last().at(0)).runId, runId);
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, saveId), 0);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(engine.state(), RunState::Idle);
        QVERIFY(!QFileInfo::exists(outputPath));

        pending = requestStep();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectFileTarget());
        const QString rejectedSaveId = controller.pendingConfirmationId();
        QVERIFY(!rejectedSaveId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedSaveId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(toolExecuted.count(), 1);
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
        QVERIFY(!QFileInfo::exists(outputPath));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        pending = requestStep();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(expectFileTarget());
        const QString approvedSaveId = controller.pendingConfirmationId();
        QVERIFY(!approvedSaveId.isEmpty());
        QVERIFY(approvedSaveId != rejectedSaveId);
        QVERIFY(controller.confirmPendingTools(approvedSaveId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 2, 5000);
        const QJsonObject saveStep = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(saveStep.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        QCOMPARE(saveStep.value(QStringLiteral("runId")).toString(), runId);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, saveId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(QFileInfo::exists(outputPath));
        QImage savedImage;
        QVERIFY(savedImage.load(outputPath));
        QCOMPARE(savedImage.size(), sourceImage.size());
        QCOMPARE(savedImage.pixel(0, 0), pixel);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(engine.state(), RunState::Idle);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testSaveImageResumeFlowConfirmsBreakpointOutput() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString inputPath = m_targets.filePath(QStringLiteral("slice71-resume-input.png"));
        const QString outputPath = m_targets.filePath(QStringLiteral("slice71-resume-output.png"));
        const QRgb pixel = qRgb(96, 32, 160);
        QImage sourceImage(4, 3, QImage::Format_RGB32);
        sourceImage.fill(pixel);
        QVERIFY(sourceImage.save(inputPath, "PNG"));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString sourceId = QStringLiteral("slice71_resume_grabimage");
        const QString saveId = QStringLiteral("slice71_resume_saveimage");
        ModuleInstance source;
        source.id = sourceId;
        source.moduleId = QStringLiteral("GrabImage");
        source.name = QStringLiteral("Slice 71 resume image source");
        source.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("File")},
                                    {QStringLiteral("filePath"), inputPath}};
        project->addModule(source);
        ModuleInstance save;
        save.id = saveId;
        save.moduleId = QStringLiteral("SaveImage");
        save.name = QStringLiteral("Slice 71 resume SaveImage target");
        save.breakpoint = true;
        save.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                  {QStringLiteral("format"), QStringLiteral("png")},
                                  {QStringLiteral("quality"), 95}};
        project->addModule(save);
        ModuleConnection connection;
        connection.fromModuleId = sourceId;
        connection.toModuleId = saveId;
        connection.fromPort = QStringLiteral("image");
        connection.toPort = QStringLiteral("image");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        engine.runOnce();
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Paused, 5000);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, saveId), 0);
        QCOMPARE(runFinished.count(), 0);
        QVERIFY(!QFileInfo::exists(outputPath));

        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString fileEffect = QStringLiteral("file_write:%1").arg(outputPath);
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(fileEffect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, runId);
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
        QCOMPARE(runFinished.count(), 0);
        QVERIFY(!QFileInfo::exists(outputPath));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(fileEffect));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));

        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(engine.state(), RunState::Idle);
        QVERIFY(!engine.isBusy());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, saveId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(QFileInfo::exists(outputPath));
        QImage savedImage;
        QVERIFY(savedImage.load(outputPath));
        QCOMPARE(savedImage.size(), sourceImage.size());
        QCOMPARE(savedImage.pixel(0, 0), pixel);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testSaveDataRunFlowConfirmsConfiguredAndRejectsUnresolvedTarget() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice43_savedata_target");
        const QString outputPath = m_targets.filePath(QStringLiteral("agent-data.json"));
        ModuleInstance saveData;
        saveData.id = instanceId;
        saveData.moduleId = QStringLiteral("SaveData");
        saveData.name = QStringLiteral("Slice 43 SaveData output");
        saveData.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                      {QStringLiteral("fileFormat"), QStringLiteral("json")},
                                      {QStringLiteral("appendMode"), false},
                                      {QStringLiteral("operation"), QStringLiteral("Delete")}};
        project->addModule(saveData);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QJsonArray expectedEffects{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                                         QStringLiteral("file_write:%1 format=json mode=overwrite").arg(outputPath)};

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffects);
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(!engine.isBusy());
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);

        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(!engine.isBusy());
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const QJsonObject runToolResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(runToolResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = runToolResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(QFileInfo::exists(outputPath));
        QFile outputFile(outputPath);
        QVERIFY(outputFile.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        const QJsonDocument savedDocument = QJsonDocument::fromJson(outputFile.readAll(), &parseError);
        outputFile.close();
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(savedDocument.isObject());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject saveResult = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("save_result")}});
        QCOMPARE(saveResult.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(saveResult.value(QStringLiteral("runId")).toString(), runId);
        QVERIFY(saveResult.value(QStringLiteral("value")).toBool());
        const QJsonObject savePath = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("save_path")}});
        QCOMPARE(savePath.value(QStringLiteral("value")).toString(), outputPath);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(!engine.isBusy());

        QVERIFY(QFile::remove(outputPath));
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("filePath"), QString()));
        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
        QVERIFY(!pending.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!controller.hasPendingTools());
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QVERIFY(!QFileInfo::exists(outputPath));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QVERIFY(!engine.isBusy());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testFileWriteConfirmationShowsModeAndExpiresOnModeDrift() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice89_writetext_mode");
        const QString outputPath = m_targets.filePath(QStringLiteral("slice89-mode.txt"));
        const QByteArray originalBytes("preserve-existing-bytes\n");
        QFile outputFile(outputPath);
        QVERIFY(outputFile.open(QIODevice::WriteOnly));
        QCOMPARE(outputFile.write(originalBytes), originalBytes.size());
        outputFile.close();

        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 89 WriteText mode drift");
        writer.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                    {QStringLiteral("textContent"), QStringLiteral("private-payload")},
                                    {QStringLiteral("appendMode"), false}};
        project->addModule(writer);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        auto sideEffects = [&controller]() {
            return controller.pendingConfirmationPayload()
                .value(QStringLiteral("context"))
                .toObject()
                .value(QStringLiteral("sideEffects"))
                .toArray();
        };
        auto readBytes = [&outputPath]() {
            QFile file(outputPath);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            return file.readAll();
        };

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray overwriteEffects = sideEffects();
        QVERIFY(!overwriteEffects.isEmpty());
        QCOMPARE(overwriteEffects.last().toString(),
                 QStringLiteral("file_write:%1 format=text mode=overwrite").arg(outputPath));
        QVERIFY(!overwriteEffects.last().toString().contains(QStringLiteral("private-payload")));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("appendMode"), true));
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_COMPARE_WITH_TIMEOUT(audit.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(readBytes(), originalBytes);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray appendEffects = sideEffects();
        QVERIFY(!appendEffects.isEmpty());
        QCOMPARE(appendEffects.last().toString(),
                 QStringLiteral("file_write:%1 format=text mode=append").arg(outputPath));
        QVERIFY(!appendEffects.last().toString().contains(QStringLiteral("private-payload")));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(readBytes(), originalBytes);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testFileWriteConfirmationExpiresWhenSymlinkDirectoryChangesTarget() {
        const QString targetA = m_targets.filePath(QStringLiteral("slice91-target-a"));
        const QString targetB = m_targets.filePath(QStringLiteral("slice91-target-b"));
        const QString linkedDirectory = m_targets.filePath(QStringLiteral("slice91-output-link"));
        QVERIFY(QDir().mkpath(targetA));
        QVERIFY(QDir().mkpath(targetB));
        if (!QFile::link(targetA, linkedDirectory) || !QFileInfo(linkedDirectory).isSymLink())
            QSKIP("The current filesystem does not provide file symlink support");

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice91_writetext_symlink");
        const QString outputPath = QDir(linkedDirectory).filePath(QStringLiteral("output.txt"));
        const QString targetAOutput = QDir(targetA).filePath(QStringLiteral("output.txt"));
        const QString targetBOutput = QDir(targetB).filePath(QStringLiteral("output.txt"));
        QVERIFY(!QFileInfo::exists(targetAOutput));
        QVERIFY(!QFileInfo::exists(targetBOutput));

        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 91 symlink output");
        writer.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                    {QStringLiteral("textContent"), QStringLiteral("slice91-target-content")},
                                    {QStringLiteral("appendMode"), false}};
        project->addModule(writer);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const auto lastFileEffect = [&controller]() {
            const QJsonArray effects = controller.pendingConfirmationPayload()
                                           .value(QStringLiteral("context"))
                                           .toObject()
                                           .value(QStringLiteral("sideEffects"))
                                           .toArray();
            return effects.last().toString();
        };

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString oldEffect = lastFileEffect();
        QVERIFY(oldEffect.startsWith(QStringLiteral("file_write:%1").arg(outputPath)));
        QVERIFY(oldEffect.contains(QStringLiteral("resolved_target=%1").arg(targetAOutput)));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());

        QVERIFY(QFile::remove(linkedDirectory));
        QVERIFY(QFile::link(targetB, linkedDirectory));
        QVERIFY(QFileInfo(linkedDirectory).isSymLink());
        QCOMPARE(lastFileEffect(), oldEffect);
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_COMPARE_WITH_TIMEOUT(audit.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QVERIFY(!QFileInfo::exists(targetAOutput));
        QVERIFY(!QFileInfo::exists(targetBOutput));

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString newEffect = lastFileEffect();
        QVERIFY(newEffect.startsWith(QStringLiteral("file_write:%1").arg(outputPath)));
        QVERIFY(newEffect.contains(QStringLiteral("resolved_target=%1").arg(targetBOutput)));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QVERIFY(!QFileInfo::exists(targetAOutput));
        QFile outputFile(targetBOutput);
        QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(outputFile.readAll()), QStringLiteral("slice91-target-content\n"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testFileWriteConfirmationExpiresWhenDanglingLeafSymlinkChangesTarget() {
        const QString targetA = m_targets.filePath(QStringLiteral("slice91-leaf-target-a"));
        const QString targetB = m_targets.filePath(QStringLiteral("slice91-leaf-target-b"));
        const QString targetAFile = QDir(targetA).filePath(QStringLiteral(" output file.txt "));
        const QString targetBFile = QDir(targetB).filePath(QStringLiteral(" output file.txt "));
        const QString outputLink = m_targets.filePath(QStringLiteral(" slice91-leaf-link "));
        QVERIFY(QDir().mkpath(targetA));
        QVERIFY(QDir().mkpath(targetB));
        if (!QFile::link(targetAFile, outputLink) || !QFileInfo(outputLink).isSymLink())
            QSKIP("The current filesystem does not support dangling file symlinks");
        QVERIFY(!QFileInfo::exists(targetAFile));
        QVERIFY(!QFileInfo::exists(targetBFile));

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice91_writetext_leaf_symlink");
        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 91 dangling leaf symlink output");
        writer.params = QJsonObject{{QStringLiteral("filePath"), outputLink},
                                    {QStringLiteral("textContent"), QStringLiteral("slice91-leaf-content")},
                                    {QStringLiteral("appendMode"), false}};
        project->addModule(writer);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const auto lastFileEffect = [&controller]() {
            const QJsonArray effects = controller.pendingConfirmationPayload()
                                           .value(QStringLiteral("context"))
                                           .toObject()
                                           .value(QStringLiteral("sideEffects"))
                                           .toArray();
            return effects.last().toString();
        };

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString oldEffect = lastFileEffect();
        QVERIFY(oldEffect.startsWith(QStringLiteral("file_write:%1").arg(outputLink)));
        QVERIFY(oldEffect.contains(QStringLiteral("resolved_target=%1").arg(targetAFile)));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());

        QVERIFY(QFile::remove(outputLink));
        QVERIFY(QFile::link(targetBFile, outputLink));
        QVERIFY(QFileInfo(outputLink).isSymLink());
        QCOMPARE(lastFileEffect(), oldEffect);
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QVERIFY(!QFileInfo::exists(targetAFile));
        QVERIFY(!QFileInfo::exists(targetBFile));

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString newEffect = lastFileEffect();
        QVERIFY(newEffect.startsWith(QStringLiteral("file_write:%1").arg(outputLink)));
        QVERIFY(newEffect.contains(QStringLiteral("resolved_target=%1").arg(targetBFile)));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QVERIFY(!QFileInfo::exists(targetAFile));
        QFile outputFile(targetBFile);
        QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(outputFile.readAll()), QStringLiteral("slice91-leaf-content\n"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testSaveDataConfirmationShowsModeDriftAndApprovedJsonMerge() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString imagePath = m_targets.filePath(QStringLiteral("slice89-source.png"));
        QImage sourceImage(3, 2, QImage::Format_RGB32);
        sourceImage.fill(qRgb(20, 60, 100));
        QVERIFY(sourceImage.save(imagePath, "PNG"));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString sourceId = QStringLiteral("slice89_grabimage_source");
        const QString saveId = QStringLiteral("slice89_savedata_output");
        const QString outputPath = m_targets.filePath(QStringLiteral("slice89-merge.json"));
        const QByteArray originalBytes("{\"existing_key\":\"preserve\","
                                       "\"source_file_name\":\"old-source.png\"}\n");
        QFile outputFile(outputPath);
        QVERIFY(outputFile.open(QIODevice::WriteOnly));
        QCOMPARE(outputFile.write(originalBytes), originalBytes.size());
        outputFile.close();

        ModuleInstance source;
        source.id = sourceId;
        source.moduleId = QStringLiteral("GrabImage");
        source.name = QStringLiteral("Slice 89 file source");
        source.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                    {QStringLiteral("filePath"), imagePath}};
        project->addModule(source);
        ModuleInstance save;
        save.id = saveId;
        save.moduleId = QStringLiteral("SaveData");
        save.name = QStringLiteral("Slice 89 JSON merge output");
        save.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                  {QStringLiteral("fileFormat"), QStringLiteral("json")},
                                  {QStringLiteral("appendMode"), false}};
        project->addModule(save);
        ModuleConnection edge;
        edge.fromModuleId = sourceId;
        edge.toModuleId = saveId;
        edge.fromPort = QStringLiteral("image");
        edge.toPort = QStringLiteral("image");
        edge.edgeType = QStringLiteral("data");
        project->addConnection(edge);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        auto sideEffects = [&controller]() {
            return controller.pendingConfirmationPayload()
                .value(QStringLiteral("context"))
                .toObject()
                .value(QStringLiteral("sideEffects"))
                .toArray();
        };
        auto readBytes = [&outputPath]() {
            QFile file(outputPath);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            return file.readAll();
        };

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(sideEffects().last().toString(),
                 QStringLiteral("file_write:%1 format=json mode=overwrite").arg(outputPath));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(project->setModuleParam(saveId, QStringLiteral("appendMode"), true));
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 0);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(readBytes(), originalBytes);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(sideEffects().last().toString(), QStringLiteral("file_write:%1 format=json mode="
                                                                 "merge_json_keys_if_existing_object_else_overwrite")
                                                      .arg(outputPath));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 0);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(readBytes(), originalBytes);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 1);
        QFile mergedFile(outputPath);
        QVERIFY(mergedFile.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        const QJsonDocument mergedDocument = QJsonDocument::fromJson(mergedFile.readAll(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(mergedDocument.isObject());
        const QJsonObject merged = mergedDocument.object();
        QCOMPARE(merged.value(QStringLiteral("existing_key")).toString(), QStringLiteral("preserve"));
        QVERIFY(merged.value(QStringLiteral("source_path")).isString());
        QVERIFY(merged.value(QStringLiteral("source_file_name")).toString() != QStringLiteral("old-source.png"));
        QCOMPARE(merged.value(QStringLiteral("source_file_name")).toString(), QStringLiteral("slice89-source.png"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testSaveDataStepOnceRejectsStaleTargetAndWritesApprovedTargetOnce() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice64_savedata_step_target");
        const QString pathA = m_targets.filePath(QStringLiteral("slice64-A.json"));
        const QString pathB = m_targets.filePath(QStringLiteral("slice64-B.json"));
        const QByteArray sentinelA("slice64-A sentinel\n");
        const QByteArray sentinelB("slice64-B sentinel\n");
        const auto writeBytes = [](const QString& path, const QByteArray& bytes) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            const qint64 written = file.write(bytes);
            file.close();
            return written == bytes.size();
        };
        const auto readBytes = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            return file.readAll();
        };
        QVERIFY(writeBytes(pathA, sentinelA));
        QVERIFY(writeBytes(pathB, sentinelB));

        ModuleInstance saveData;
        saveData.id = instanceId;
        saveData.moduleId = QStringLiteral("SaveData");
        saveData.name = QStringLiteral("Slice 64 SaveData step target");
        saveData.params = QJsonObject{{QStringLiteral("filePath"), pathA},
                                      {QStringLiteral("fileFormat"), QStringLiteral("json")},
                                      {QStringLiteral("appendMode"), false},
                                      {QStringLiteral("operation"), QStringLiteral("Delete")}};
        project->addModule(saveData);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        const QJsonObject staleContext =
            controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
        const QJsonArray staleEffects = staleContext.value(QStringLiteral("sideEffects")).toArray();
        const QString fileEffectA = QStringLiteral("file_write:%1 format=json mode=overwrite").arg(pathA);
        QVERIFY(staleEffects.contains(fileEffectA));
        QVERIFY(!staleEffects.contains(QStringLiteral("folder_delete:%1").arg(pathA)));
        QCOMPARE(readBytes(pathA), sentinelA);
        QCOMPARE(readBytes(pathB), sentinelB);

        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("filePath"), pathB));
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(readBytes(pathA), sentinelA);
        QCOMPARE(readBytes(pathB), sentinelB);
        QVERIFY(!engine.isBusy());

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString freshId = controller.pendingConfirmationId();
        QVERIFY(!freshId.isEmpty());
        QVERIFY(freshId != staleId);
        const QJsonObject freshContext =
            controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
        const QJsonArray freshEffects = freshContext.value(QStringLiteral("sideEffects")).toArray();
        const QString fileEffectB = QStringLiteral("file_write:%1 format=json mode=overwrite").arg(pathB);
        QVERIFY(freshEffects.contains(fileEffectB));
        QVERIFY(!freshEffects.contains(QStringLiteral("folder_delete:%1").arg(pathB)));
        QCOMPARE(readBytes(pathA), sentinelA);
        QCOMPARE(readBytes(pathB), sentinelB);

        QVERIFY(controller.confirmPendingTools(freshId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const QJsonObject stepToolResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepToolResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = stepToolResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(readBytes(pathA), sentinelA);
        const QByteArray savedBytes = readBytes(pathB);
        QVERIFY(savedBytes != sentinelB);
        QJsonParseError parseError;
        const QJsonDocument savedDocument = QJsonDocument::fromJson(savedBytes, &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(savedDocument.isObject());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testSaveDataResumeFlowRejectsAndExpiresStalePathThenWritesOriginalOnce() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice65_savedata_resume_target");
        const QString pathA = m_targets.filePath(QStringLiteral("slice65-A.json"));
        const QString pathB = m_targets.filePath(QStringLiteral("slice65-B.json"));
        const QByteArray sentinelA("slice65-A sentinel\n");
        const QByteArray sentinelB("slice65-B sentinel\n");
        const auto writeBytes = [](const QString& path, const QByteArray& bytes) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            const qint64 written = file.write(bytes);
            file.close();
            return written == bytes.size();
        };
        const auto readBytes = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            return file.readAll();
        };
        QVERIFY(writeBytes(pathA, sentinelA));
        QVERIFY(writeBytes(pathB, sentinelB));

        ModuleInstance saveData;
        saveData.id = instanceId;
        saveData.moduleId = QStringLiteral("SaveData");
        saveData.name = QStringLiteral("Slice 65 SaveData resume target");
        saveData.breakpoint = true;
        saveData.params = QJsonObject{{QStringLiteral("filePath"), pathA},
                                      {QStringLiteral("fileFormat"), QStringLiteral("json")},
                                      {QStringLiteral("appendMode"), false},
                                      {QStringLiteral("operation"), QStringLiteral("Delete")}};
        project->addModule(saveData);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QCOMPARE(readBytes(pathA), sentinelA);
        QCOMPARE(readBytes(pathB), sentinelB);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString fileEffectA = QStringLiteral("file_write:%1 format=json mode=overwrite").arg(pathA);
        const QJsonArray firstEffects = controller.pendingConfirmationPayload()
                                            .value(QStringLiteral("context"))
                                            .toObject()
                                            .value(QStringLiteral("sideEffects"))
                                            .toArray();
        QVERIFY(firstEffects.contains(fileEffectA));
        QVERIFY(!firstEffects.contains(QStringLiteral("folder_delete:%1").arg(pathA)));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(readBytes(pathA), sentinelA);
        QCOMPARE(readBytes(pathB), sentinelB);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(staleId != rejectedId);
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("filePath"), pathB));
        QVERIFY(controller.confirmPendingTools(staleId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(readBytes(pathA), sentinelA);
        QCOMPARE(readBytes(pathB), sentinelB);

        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("filePath"), pathA));
        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != staleId);
        const QJsonArray freshEffects = controller.pendingConfirmationPayload()
                                            .value(QStringLiteral("context"))
                                            .toObject()
                                            .value(QStringLiteral("sideEffects"))
                                            .toArray();
        QVERIFY(freshEffects.contains(fileEffectA));
        QVERIFY(!freshEffects.contains(QStringLiteral("folder_delete:%1").arg(pathA)));
        QCOMPARE(readBytes(pathA), sentinelA);
        QCOMPARE(readBytes(pathB), sentinelB);

        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(engine.state(), RunState::Idle);
        QVERIFY(!engine.isBusy());
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        const QByteArray writtenA = readBytes(pathA);
        QVERIFY(writtenA != sentinelA);
        QJsonParseError parseError;
        const QJsonDocument saved = QJsonDocument::fromJson(writtenA, &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(saved.isObject());
        QCOMPARE(readBytes(pathB), sentinelB);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testUnresolvedFileTargetsFailClosedForChatRunFlow() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const AgentController::PermissionLevel oldPermission = controller.permissionLevel();
        ILLMClient* const oldClient = controller.llmClient();
        FolderExecutionMockLLMClient mock;
        struct RestoreAgentState {
            AgentController& controller;
            ILLMClient* client;
            AgentController::PermissionLevel permission;
            ~RestoreAgentState() {
                controller.setLLMClient(client);
                controller.setPermissionLevel(permission);
            }
        } restore{controller, oldClient, oldPermission};
        controller.setLLMClient(&mock);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        for (const QString& pluginName :
             {QStringLiteral("WriteText"), QStringLiteral("SaveData"), QStringLiteral("Folder")}) {
            ProjectManager::instance().closeProject();
            engine.stop();
            engine.clearModules();
            controller.clearConversation();

            Project* project = ProjectManager::instance().newProject();
            QVERIFY(project);
            const QString instanceId = QStringLiteral("slice66_%1_unresolved").arg(pluginName.toLower());
            ModuleInstance output;
            output.id = instanceId;
            output.moduleId = pluginName;
            output.name = QStringLiteral("Slice 66 unresolved %1 target").arg(pluginName);
            if (pluginName == QLatin1String("WriteText")) {
                output.params = QJsonObject{{QStringLiteral("filePath"), QString()},
                                            {QStringLiteral("textContent"), QStringLiteral("must not be written")},
                                            {QStringLiteral("appendMode"), false}};
            } else if (pluginName == QLatin1String("SaveData")) {
                output.params = QJsonObject{{QStringLiteral("filePath"), QString()},
                                            {QStringLiteral("fileFormat"), QStringLiteral("json")},
                                            {QStringLiteral("appendMode"), false}};
            } else {
                output.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                            {QStringLiteral("folderPath"), QString()}};
            }
            project->addModule(output);

            const QString targetDirectory = m_targets.filePath(QStringLiteral("slice66-%1").arg(pluginName.toLower()));
            QVERIFY(QDir().mkpath(targetDirectory));
            QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
            QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
            QSignalSpy pendingConfirmation(&controller, &AgentController::toolsPendingConfirmation);
            QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

            controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
            QJsonObject bridgeResult = controller.handleToolCall(
                QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
            QCOMPARE(bridgeResult.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
            QVERIFY(!bridgeResult.value(QStringLiteral("retryable")).toBool());
            QCOMPARE(pendingConfirmation.count(), 0);
            QVERIFY(!controller.hasPendingTools());
            QVERIFY(controller.pendingConfirmationPayload().isEmpty());
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
            QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
            QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
            QCOMPARE(runFinished.count(), 0);
            QVERIFY(QDir(targetDirectory).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

            verifyChatRejectsUnresolvedFileTarget(
                controller, engine, mock, moduleStarted, moduleFinished, runFinished, toolExecuted, pendingConfirmation,
                audit, instanceId, QStringLiteral("run_flow"),
                QStringLiteral("slice66-advisor-chat-%1").arg(pluginName.toLower()), targetDirectory, false);
            controller.clearConversation();

            controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
            bridgeResult = controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
            QCOMPARE(bridgeResult.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
            QVERIFY(!bridgeResult.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!controller.hasPendingTools());
            QVERIFY(controller.pendingConfirmationPayload().isEmpty());
            QCOMPARE(pendingConfirmation.count(), 0);
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
            QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
            QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
            QCOMPARE(runFinished.count(), 0);
            QVERIFY(!engine.isBusy());
            QCOMPARE(QDir(targetDirectory).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
            audit.clear();

            const QString callId = QStringLiteral("slice66-chat-%1").arg(pluginName.toLower());
            controller.sendUserMessage(QStringLiteral("Run the configured file output flow."));
            AgentResponse toolResponse;
            toolResponse.success = true;
            toolResponse.toolCalls = QJsonArray{
                QJsonObject{{QStringLiteral("id"), callId},
                            {QStringLiteral("type"), QStringLiteral("function")},
                            {QStringLiteral("function"),
                             QJsonObject{{QStringLiteral("name"), QStringLiteral("run_flow")},
                                         {QStringLiteral("arguments"), QStringLiteral("{\"mode\":\"once\"}")}}}}};
            mock.respond(toolResponse);
            QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);

            QVERIFY(!controller.hasPendingTools());
            QVERIFY(controller.pendingConfirmationPayload().isEmpty());
            QCOMPARE(pendingConfirmation.count(), 0);
            QCOMPARE(controller.conversationHistorySize(), 3);
            QVERIFY(!engine.isBusy());
            QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
            QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
            QCOMPARE(runFinished.count(), 0);
            QCOMPARE(QDir(targetDirectory).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
            QCOMPARE(audit.count(), 1);
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
            QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

            controller.sendUserMessage(QStringLiteral("Check the tool result."));
            int toolResultCount = 0;
            for (const AgentMessage& message : mock.lastContext.messages) {
                if (message.role != QLatin1String("tool"))
                    continue;
                ++toolResultCount;
                QCOMPARE(message.toolCallId, callId);
                const QJsonObject result = QJsonDocument::fromJson(message.content.toUtf8()).object();
                QCOMPARE(result.value(QStringLiteral("errorCode")).toString(),
                         QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
                QVERIFY(!result.value(QStringLiteral("retryable")).toBool());
            }
            QCOMPARE(toolResultCount, 1);
            AgentResponse finalResponse;
            finalResponse.success = true;
            finalResponse.content = QStringLiteral("The unresolved target was rejected.");
            mock.respond(finalResponse);
            QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        }

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testUnresolvedTargetScopeUnknownFailsClosed() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        engine.stop();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);

        ModuleInstance writer;
        writer.id = QStringLiteral("slice122_unknown_scope_writer");
        writer.moduleId = QStringLiteral("WriteText");
        writer.params = QJsonObject{{QStringLiteral("filePath"), QString()},
                                    {QStringLiteral("textContent"), QStringLiteral("must not be written")}};
        project->addModule(writer);
        ModuleInstance next;
        next.id = QStringLiteral("slice122_unknown_scope_next");
        next.moduleId = QStringLiteral("WriteText");
        next.params =
            QJsonObject{{QStringLiteral("filePath"), m_targets.filePath(QStringLiteral("slice122-known-path"))}};
        project->addModule(next);
        ModuleConnection connection;
        connection.fromModuleId = writer.id;
        connection.toModuleId = next.id;
        connection.fromPort = QStringLiteral("image");
        connection.toPort = QStringLiteral("image");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);

        QSignalSpy pending(&controller, &AgentController::toolsPendingConfirmation);
        QSignalSpy started(&engine, &RunEngine::moduleStarted);
        QSignalSpy finished(&engine, &RunEngine::moduleFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject result = controller.handleToolCall(QStringLiteral("run_flow"), {});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("EXECUTION_SCOPE_UNRESOLVED"));
        QVERIFY(!result.value(QStringLiteral("retryable")).toBool());
        QVERIFY(result.value(QStringLiteral("details"))
                    .toObject()
                    .value(QStringLiteral("instanceIds"))
                    .toArray()
                    .contains(writer.id));
        QVERIFY(!controller.hasPendingTools());
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QCOMPARE(pending.count(), 0);
        QCOMPARE(started.count(), 0);
        QCOMPARE(finished.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        engine.clearModules();
        ProjectManager::instance().closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testDisabledUnresolvedTargetOutsideExecutionScopeDoesNotBlockConfirmation() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        engine.stop();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance disabledFolder;
        disabledFolder.id = QStringLiteral("slice122_disabled_unresolved_folder");
        disabledFolder.moduleId = QStringLiteral("Folder");
        disabledFolder.enabled = false;
        disabledFolder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                            {QStringLiteral("folderPath"), QString()}};
        project->addModule(disabledFolder);

        QSignalSpy pending(&controller, &AgentController::toolsPendingConfirmation);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject result = controller.handleToolCall(QStringLiteral("run_flow"), {});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(pending.count(), 1);
        QVERIFY(controller.hasPendingTools());
        const QJsonArray effects = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray();
        QVERIFY(std::none_of(effects.cbegin(), effects.cend(), [](const QJsonValue& value) {
            return value.toString().contains(QStringLiteral("slice122_disabled_unresolved_folder"));
        }));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        engine.clearModules();
        ProjectManager::instance().closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testEnabledUnresolvedTargetOutsideStepCandidatesDoesNotBlockConfirmation() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        engine.stop();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);

        const QString configuredDirectory = m_targets.filePath(QStringLiteral("slice122-step-known-folder"));
        ModuleInstance first;
        first.id = QStringLiteral("slice122_step_candidate_folder");
        first.moduleId = QStringLiteral("Folder");
        first.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                   {QStringLiteral("folderPath"), configuredDirectory}};
        project->addModule(first);
        ModuleInstance second;
        second.id = QStringLiteral("slice122_step_outside_unresolved_writer");
        second.moduleId = QStringLiteral("WriteText");
        second.params = QJsonObject{{QStringLiteral("filePath"), QString()},
                                    {QStringLiteral("textContent"), QStringLiteral("must not be written")}};
        project->addModule(second);
        QVERIFY(engine.loadProject(project));

        QSignalSpy pending(&controller, &AgentController::toolsPendingConfirmation);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject result = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(pending.count(), 1);
        const QJsonArray effects = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray();
        QVERIFY(std::any_of(effects.cbegin(), effects.cend(), [&configuredDirectory](const QJsonValue& value) {
            return value.toString().startsWith(QStringLiteral("folder_create:")) &&
                   value.toString().contains(configuredDirectory);
        }));
        QVERIFY(std::none_of(effects.cbegin(), effects.cend(),
                             [&second](const QJsonValue& value) { return value.toString().contains(second.id); }));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!QFileInfo::exists(configuredDirectory));
        QVERIFY(!QFileInfo::exists(QDir(configuredDirectory).filePath(QStringLiteral("must not be written"))));
        engine.clearModules();
        ProjectManager::instance().closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testUnresolvedFileTargetsFailClosedForStepAndResume() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        const AgentController::PermissionLevel oldPermission = controller.permissionLevel();
        ILLMClient* const oldClient = controller.llmClient();
        FolderExecutionMockLLMClient mock;
        struct RestoreAgentState {
            AgentController& controller;
            ILLMClient* client;
            AgentController::PermissionLevel permission;
            ~RestoreAgentState() {
                controller.setLLMClient(client);
                controller.setPermissionLevel(permission);
            }
        } restore{controller, oldClient, oldPermission};
        controller.setLLMClient(&mock);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        for (const QString& pluginName :
             {QStringLiteral("WriteText"), QStringLiteral("SaveData"), QStringLiteral("Folder")}) {
            ProjectManager::instance().closeProject();
            engine.stop();
            engine.clearModules();
            controller.clearConversation();

            Project* project = ProjectManager::instance().newProject();
            QVERIFY(project);
            const QString instanceId = QStringLiteral("slice67_%1_unresolved").arg(pluginName.toLower());
            ModuleInstance output;
            output.id = instanceId;
            output.moduleId = pluginName;
            output.name = QStringLiteral("Slice 67 unresolved %1 target").arg(pluginName);
            output.breakpoint = true;
            if (pluginName == QLatin1String("WriteText")) {
                output.params = QJsonObject{{QStringLiteral("filePath"), QString()},
                                            {QStringLiteral("textContent"), QStringLiteral("must not be written")},
                                            {QStringLiteral("appendMode"), false}};
            } else if (pluginName == QLatin1String("SaveData")) {
                output.params = QJsonObject{{QStringLiteral("filePath"), QString()},
                                            {QStringLiteral("fileFormat"), QStringLiteral("json")},
                                            {QStringLiteral("appendMode"), false}};
            } else {
                output.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                            {QStringLiteral("folderPath"), QString()}};
            }
            project->addModule(output);
            QVERIFY(engine.loadProject(project));

            const QString targetDirectory = m_targets.filePath(QStringLiteral("slice67-%1").arg(pluginName.toLower()));
            QVERIFY(QDir().mkpath(targetDirectory));
            QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
            QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
            QSignalSpy pendingConfirmation(&controller, &AgentController::toolsPendingConfirmation);
            QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

            const RunResumeSnapshot beforeStep = engine.resumeSnapshot();
            const QJsonObject bridgeStep = controller.handleToolCall(QStringLiteral("step_once"), {});
            QCOMPARE(bridgeStep.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
            QVERIFY(!bridgeStep.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!controller.hasPendingTools());
            QVERIFY(controller.pendingConfirmationPayload().isEmpty());
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
            QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
            QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
            QCOMPARE(runFinished.count(), 0);
            QVERIFY(!engine.isBusy());
            verifyResumeSnapshotUnchanged(engine, beforeStep);
            QVERIFY(QDir(targetDirectory).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

            verifyChatRejectsUnresolvedFileTarget(
                controller, engine, mock, moduleStarted, moduleFinished, runFinished, toolExecuted, pendingConfirmation,
                audit, instanceId, QStringLiteral("step_once"),
                QStringLiteral("slice67-step-%1").arg(pluginName.toLower()), targetDirectory, false);
            verifyResumeSnapshotUnchanged(engine, beforeStep);

            controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
            QJsonObject advisorStep = controller.handleToolCall(QStringLiteral("step_once"), {});
            QCOMPARE(advisorStep.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
            QVERIFY(!advisorStep.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!controller.hasPendingTools());
            QVERIFY(controller.pendingConfirmationPayload().isEmpty());
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
            QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
            QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
            QCOMPARE(runFinished.count(), 0);
            verifyResumeSnapshotUnchanged(engine, beforeStep);
            QCOMPARE(QDir(targetDirectory).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);

            verifyChatRejectsUnresolvedFileTarget(
                controller, engine, mock, moduleStarted, moduleFinished, runFinished, toolExecuted, pendingConfirmation,
                audit, instanceId, QStringLiteral("step_once"),
                QStringLiteral("slice67-advisor-step-%1").arg(pluginName.toLower()), targetDirectory, false);
            verifyResumeSnapshotUnchanged(engine, beforeStep);

            controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
            engine.runOnce();
            QCOMPARE(engine.state(), RunState::Paused);
            const QString pausedRunId = engine.runObservation().runId;
            const RunResumeSnapshot beforeResume = engine.resumeSnapshot();
            QVERIFY(!pausedRunId.isEmpty());
            QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
            QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
            QCOMPARE(runFinished.count(), 0);

            const QJsonObject bridgeResume = controller.handleToolCall(QStringLiteral("resume_flow"), {});
            QCOMPARE(bridgeResume.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
            QVERIFY(!bridgeResume.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!controller.hasPendingTools());
            QVERIFY(controller.pendingConfirmationPayload().isEmpty());
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
            QCOMPARE(engine.state(), RunState::Paused);
            QCOMPARE(engine.runObservation().runId, pausedRunId);
            QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
            QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
            QCOMPARE(runFinished.count(), 0);
            QCOMPARE(toolExecuted.count(), 0);
            verifyResumeSnapshotUnchanged(engine, beforeResume);
            QVERIFY(QDir(targetDirectory).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

            verifyChatRejectsUnresolvedFileTarget(
                controller, engine, mock, moduleStarted, moduleFinished, runFinished, toolExecuted, pendingConfirmation,
                audit, instanceId, QStringLiteral("resume_flow"),
                QStringLiteral("slice67-resume-%1").arg(pluginName.toLower()), targetDirectory, true);
            QCOMPARE(engine.runObservation().runId, pausedRunId);
            verifyResumeSnapshotUnchanged(engine, beforeResume);

            controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
            QJsonObject advisorResume = controller.handleToolCall(QStringLiteral("resume_flow"), {});
            QCOMPARE(advisorResume.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED"));
            QVERIFY(!advisorResume.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!controller.hasPendingTools());
            QVERIFY(controller.pendingConfirmationPayload().isEmpty());
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
            QCOMPARE(engine.state(), RunState::Paused);
            QCOMPARE(engine.runObservation().runId, pausedRunId);
            QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
            QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
            QCOMPARE(runFinished.count(), 0);
            verifyResumeSnapshotUnchanged(engine, beforeResume);
            QVERIFY(QDir(targetDirectory).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

            verifyChatRejectsUnresolvedFileTarget(
                controller, engine, mock, moduleStarted, moduleFinished, runFinished, toolExecuted, pendingConfirmation,
                audit, instanceId, QStringLiteral("resume_flow"),
                QStringLiteral("slice67-advisor-resume-%1").arg(pluginName.toLower()), targetDirectory, true);
            QCOMPARE(engine.runObservation().runId, pausedRunId);
            verifyResumeSnapshotUnchanged(engine, beforeResume);

            controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
            engine.stop();
            QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 5000);
            engine.clearModules();
            ProjectManager::instance().closeProject();
        }

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testSaveImageRejectsEmptyInputBeforeCreatingOutputDirectory() {
        const QString outputDirectory = m_targets.filePath(QStringLiteral("empty-save-image-parent"));
        const QString outputPath = QDir(outputDirectory).filePath(QStringLiteral("empty.png"));
        QVERIFY(!QDir(outputDirectory).exists());

        PluginManager::ModulePtr managed = PluginManager::instance().createManagedModule(QStringLiteral("SaveImage"));
        QVERIFY(managed);
        ModuleBase* saveImage = qobject_cast<ModuleBase*>(managed.get());
        QVERIFY(saveImage);
        QVERIFY(saveImage->initialize());
        saveImage->setParams(QJsonObject{{QStringLiteral("filePath"), outputPath}});
        QSignalSpy errors(saveImage, &IModule::errorOccurred);
        ImageData invalidInput;
#ifdef DEEPLUX_HAS_OPENCV
        invalidInput.setMat(cv::Mat());
#endif
        ImageData output;

        QVERIFY(!saveImage->execute(invalidInput, output));
        QVERIFY(!errors.isEmpty());
        QVERIFY(!QDir(outputDirectory).exists());
        QVERIFY(!QFileInfo::exists(outputPath));
        saveImage->shutdown();
    }

    void testFolderDeleteRequiresConfirmationForRunFlowAndStepOnce() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice31_folder_delete");
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 31 temporary Folder delete");
        folder.params.insert(QStringLiteral("operation"), QStringLiteral("Delete"));
        const QString runTarget = makeDeleteTarget(QStringLiteral("run-flow-target"));
        QVERIFY(!runTarget.isEmpty());
        folder.params.insert(QStringLiteral("folderPath"), runTarget);
        project->addModule(folder);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(QFileInfo::exists(QDir(runTarget).filePath(QStringLiteral("probe.txt"))));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(QFileInfo::exists(QDir(runTarget).filePath(QStringLiteral("probe.txt"))));
        QVERIFY(!engine.isBusy());
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(QFileInfo::exists(runTarget));
        const QString driftTarget = makeDeleteTarget(QStringLiteral("run-flow-drift-target"));
        QVERIFY(!driftTarget.isEmpty());
        const QString expectedRunDeleteEffect = QStringLiteral("folder_delete:%1@%2").arg(instanceId, runTarget);
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(expectedRunDeleteEffect));
        const QString staleRunConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!staleRunConfirmationId.isEmpty());
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("folderPath"), driftTarget));
        QVERIFY(controller.confirmPendingTools(staleRunConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(QFileInfo::exists(QDir(runTarget).filePath(QStringLiteral("probe.txt"))));
        QVERIFY(QFileInfo::exists(QDir(driftTarget).filePath(QStringLiteral("probe.txt"))));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QVERIFY(!engine.isBusy());

        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("folderPath"), runTarget));
        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(expectedRunDeleteEffect));
        const QString runConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!runConfirmationId.isEmpty());
        QVERIFY(runConfirmationId != staleRunConfirmationId);
        QVERIFY(controller.confirmPendingTools(runConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(started.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult runResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(runResult.runId, runId);
        QVERIFY2(runResult.success, qPrintable(runResult.errorMessage));
        QVERIFY(!QFileInfo::exists(runTarget));
        QVERIFY(QFileInfo::exists(QDir(driftTarget).filePath(QStringLiteral("probe.txt"))));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        const QJsonObject deleteResult = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("operation_result")}});
        QCOMPARE(deleteResult.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(deleteResult.value(QStringLiteral("runId")).toString(), runId);
        QVERIFY(deleteResult.value(QStringLiteral("value")).isBool());
        QVERIFY(deleteResult.value(QStringLiteral("value")).toBool());
        const QJsonObject deletedPath = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("folder_path")}});
        QCOMPARE(deletedPath.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(deletedPath.value(QStringLiteral("runId")).toString(), runId);
        QCOMPARE(deletedPath.value(QStringLiteral("value")).toString(), runTarget);
        QVERIFY(!QFileInfo::exists(runTarget));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("get_module_output"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!engine.isBusy());

        const QString stepTarget = makeDeleteTarget(QStringLiteral("step-once-target"));
        QVERIFY(!stepTarget.isEmpty());
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("folderPath"), stepTarget));
        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(QFileInfo::exists(QDir(stepTarget).filePath(QStringLiteral("probe.txt"))));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(QFileInfo::exists(QDir(stepTarget).filePath(QStringLiteral("probe.txt"))));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(QFileInfo::exists(stepTarget));
        const QString stepConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!stepConfirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(stepConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 4, 5000); // run_flow + 两次输出查询 + step_once
        const QJsonObject stepped = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepped.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString stepRunId = stepped.value(QStringLiteral("runId")).toString();
        QVERIFY(!stepRunId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult stepResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(stepResult.runId, stepRunId);
        QVERIFY2(stepResult.success, qPrintable(stepResult.errorMessage));
        QVERIFY(!QFileInfo::exists(stepTarget));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 2);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 2);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testFolderDeleteConfirmationExpiresWhenSymlinkParentChangesTarget() {
#ifndef Q_OS_UNIX
        QSKIP("The test requires POSIX symlink and rename semantics");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString rootA = m_targets.filePath(QStringLiteral("slice108-root-a"));
        const QString rootB = m_targets.filePath(QStringLiteral("slice108-root-b"));
        const QString targetA = QDir(rootA).filePath(QStringLiteral("target"));
        const QString targetB = QDir(rootB).filePath(QStringLiteral("target"));
        const QString link = m_targets.filePath(QStringLiteral("slice108-current-root"));
        const QString replacementLink = m_targets.filePath(QStringLiteral("slice108-next-root"));
        const QString instanceId = QStringLiteral("slice108_folder_delete_symlink_target");
        const QString sentinelA = QStringLiteral("slice108 A sentinel\n");
        const QString sentinelB = QStringLiteral("slice108 B sentinel\n");
        QVERIFY(QDir().mkpath(targetA));
        QVERIFY(QDir().mkpath(targetB));
        const auto writeSentinel = [](const QString& path, const QString& contents) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            const QByteArray bytes = contents.toUtf8();
            return file.write(bytes) == bytes.size() && file.flush();
        };
        const auto readSentinel = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return QString();
            return QString::fromUtf8(file.readAll());
        };
        const QString markerA = QDir(targetA).filePath(QStringLiteral("marker.txt"));
        const QString markerB = QDir(targetB).filePath(QStringLiteral("marker.txt"));
        QVERIFY(writeSentinel(markerA, sentinelA));
        QVERIFY(writeSentinel(markerB, sentinelB));
        if (!QFile::link(rootA, link) || !QFileInfo(link).isSymLink())
            QSKIP("The current filesystem does not provide directory symlink support");

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 108 symlink Folder delete");
        folder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Delete")},
                                    {QStringLiteral("folderPath"), QDir(link).filePath(QStringLiteral("target"))}};
        project->addModule(folder);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QString configuredPath = folder.params.value(QStringLiteral("folderPath")).toString();
        const QString oldEffect = QStringLiteral("folder_delete:%1@%2 resolved_target=%3")
                                      .arg(instanceId, configuredPath, QDir(targetA).canonicalPath());

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray oldEffects = controller.pendingConfirmationPayload()
                                          .value(QStringLiteral("context"))
                                          .toObject()
                                          .value(QStringLiteral("sideEffects"))
                                          .toArray();
        QVERIFY(oldEffects.contains(oldEffect));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(QFileInfo::exists(markerA));
        QVERIFY(QFileInfo::exists(markerB));

        if (!QFile::link(rootB, replacementLink) || !QFileInfo(replacementLink).isSymLink()) {
            QFile::remove(link);
            QVERIFY(controller.rejectPendingTools(staleId));
            controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
            engine.clearModules();
            ProjectManager::instance().closeProject();
            QSKIP("The current filesystem cannot create the replacement directory symlink");
        }
        const QByteArray replacementName = QFile::encodeName(replacementLink);
        const QByteArray currentName = QFile::encodeName(link);
        if (std::rename(replacementName.constData(), currentName.constData()) != 0) {
            QFile::remove(replacementLink);
            QFile::remove(link);
            QVERIFY(controller.rejectPendingTools(staleId));
            controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
            engine.clearModules();
            ProjectManager::instance().closeProject();
            QSKIP("The current filesystem does not atomically replace an existing symlink with POSIX rename");
        }
        QVERIFY(QFileInfo(link).isSymLink());
        QCOMPARE(QFileInfo(link).symLinkTarget(), QFileInfo(rootB).canonicalFilePath());

        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QVERIFY(!engine.isBusy());
        QCOMPARE(readSentinel(markerA), sentinelA);
        QCOMPARE(readSentinel(markerB), sentinelB);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString freshId = controller.pendingConfirmationId();
        QVERIFY(!freshId.isEmpty());
        QVERIFY(freshId != staleId);
        const QString newEffect = QStringLiteral("folder_delete:%1@%2 resolved_target=%3")
                                      .arg(instanceId, configuredPath, QDir(targetB).canonicalPath());
        const QJsonArray newEffects = controller.pendingConfirmationPayload()
                                          .value(QStringLiteral("context"))
                                          .toObject()
                                          .value(QStringLiteral("sideEffects"))
                                          .toArray();
        QVERIFY(newEffects.contains(newEffect));
        QVERIFY(controller.confirmPendingTools(freshId));
        QTRY_COMPARE_WITH_TIMEOUT(moduleStarted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(moduleFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(moduleStarted.first().at(0).toString(), instanceId);
        QCOMPARE(moduleFinished.first().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.first().at(1).toBool());
        const RunResult result = qvariant_cast<RunResult>(runFinished.first().at(0));
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QCOMPARE(readSentinel(markerA), sentinelA);
        QVERIFY(!QDir(targetB).exists());
        QVERIFY(QFileInfo::exists(link));

        const QJsonObject operationOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), result.runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("operation_result")}});
        QCOMPARE(operationOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(operationOutput.value(QStringLiteral("runId")).toString(), result.runId);
        QVERIFY(operationOutput.value(QStringLiteral("value")).toBool());
        const QJsonObject pathOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), result.runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("folder_path")}});
        QCOMPARE(pathOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(pathOutput.value(QStringLiteral("value")).toString(), configuredPath);
        QVERIFY(!engine.isBusy());

        QFile::remove(link);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testFolderCreateBoundedCycleRequiresExactConfirmation() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice85_folder_create");
        const QString target = m_targets.filePath(QStringLiteral("bounded-cycle-create"));
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 85 bounded Folder create");
        folder.params.insert(QStringLiteral("operation"), QStringLiteral("Create"));
        folder.params.insert(QStringLiteral("folderPath"), target);
        project->addModule(folder);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        QString startedSessionId;
        QJsonObject terminalSession;
        QObject signalContext;
        connect(actor, &AgentActor::toolExecuted, &signalContext,
                [&startedSessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        startedSessionId = result.value(QStringLiteral("sessionId")).toString();
                });
        QJsonObject invalid = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("wait"), false}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("MISSING_PARAM"));
        QVERIFY(!controller.hasPendingTools());
        invalid = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("maxCycles"), 3}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!controller.hasPendingTools());
        for (const QJsonValue& value :
             {QJsonValue(0), QJsonValue(1.5), QJsonValue(11), QJsonValue(QStringLiteral("2"))}) {
            invalid = controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), value}});
            QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
            QVERIFY(!controller.hasPendingTools());
        }
        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QJsonArray effects = controller.pendingConfirmationPayload()
                                 .value(QStringLiteral("context"))
                                 .toObject()
                                 .value(QStringLiteral("sideEffects"))
                                 .toArray();
        QVERIFY(effects.contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
        QVERIFY(effects.contains(QStringLiteral("folder_create:%1@%2").arg(instanceId, target)));
        QVERIFY(!QFileInfo::exists(target));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(!QFileInfo::exists(target));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        connect(
            &engine, &RunEngine::cycleStopped, &signalContext,
            [&]() {
                terminalSession = actor->executeTool(QStringLiteral("get_run_results"),
                                                     QJsonObject{{QStringLiteral("runId"), startedSessionId}});
            },
            Qt::QueuedConnection);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QVERIFY(!startedSessionId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(countModuleSignal(moduleStarted, instanceId), 3, 10000);
        QVERIFY(QFileInfo::exists(target));
        QTest::qWait(350);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 3);
        QCOMPARE(cycleStopped.count(), 1);

        QTRY_VERIFY_WITH_TIMEOUT(!terminalSession.isEmpty(), 5000);
        QCOMPARE(terminalSession.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(terminalSession.value(QStringLiteral("maxCycles")).toInt(), 3);
        QCOMPARE(terminalSession.value(QStringLiteral("completedCycles")).toInt(), 3);
        QVERIFY(!terminalSession.value(QStringLiteral("lastCompletedRunId")).toString().isEmpty());
        QCOMPARE(terminalSession.value(QStringLiteral("lastSuccessfulRunId")).toString(),
                 terminalSession.value(QStringLiteral("lastCompletedRunId")).toString());
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testWriteTextAppendBoundedCycleRequiresExactConfirmation() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice98_writetext_append_cycle");
        const QString outputPath = m_targets.filePath(QStringLiteral("slice98-append-cycle.txt"));
        const QByteArray sentinel("existing-prefix\n");
        const QString textContent = QStringLiteral("确认追加/帧-α\n");
        const QByteArray appendedBytes = textContent.toUtf8();
        const QByteArray expectedBytes = sentinel + appendedBytes + appendedBytes + appendedBytes;
        {
            QFile file(outputPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(sentinel), sentinel.size());
        }

        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 98 bounded WriteText append");
        writer.params = QJsonObject{{QStringLiteral("filePath"), outputPath},
                                    {QStringLiteral("textContent"), textContent},
                                    {QStringLiteral("appendMode"), true}};
        project->addModule(writer);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        auto fileBytes = [&outputPath]() {
            QFile file(outputPath);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            return file.readAll();
        };
        auto requestCycle = [&controller]() {
            return controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        };

        QJsonObject pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QJsonObject payload = controller.pendingConfirmationPayload();
        QJsonObject context = payload.value(QStringLiteral("context")).toObject();
        QJsonArray effects = context.value(QStringLiteral("sideEffects")).toArray();
        QVERIFY(effects.contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
        QVERIFY(effects.contains(QStringLiteral("file_write:%1 format=text mode=append").arg(outputPath)));
        QCOMPARE(context.value(QStringLiteral("reversibilityLabel")).toString(),
                 QStringLiteral("不可撤销：Agent Undo 不会回滚外部副作用"));
        QVERIFY(!QJsonDocument(payload).toJson(QJsonDocument::Compact).contains(textContent.toUtf8()));
        QCOMPARE(fileBytes(), sentinel);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(fileBytes(), sentinel);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(staleId != rejectedId);
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("appendMode"), false));
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(fileBytes(), sentinel);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("appendMode"), true));
        pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != staleId);
        payload = controller.pendingConfirmationPayload();
        context = payload.value(QStringLiteral("context")).toObject();
        QCOMPARE(context.value(QStringLiteral("reversibilityLabel")).toString(),
                 QStringLiteral("不可撤销：Agent Undo 不会回滚外部副作用"));
        QVERIFY(!QJsonDocument(payload).toJson(QJsonDocument::Compact).contains(textContent.toUtf8()));

        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        QString sessionId;
        QJsonObject terminalSession;
        QObject signalContext;
        connect(actor, &AgentActor::toolExecuted, &signalContext,
                [&sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                });
        connect(
            &engine, &RunEngine::cycleStopped, &signalContext,
            [&]() {
                terminalSession = actor->executeTool(QStringLiteral("get_run_results"),
                                                     QJsonObject{{QStringLiteral("runId"), sessionId}});
            },
            Qt::QueuedConnection);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QVERIFY(!sessionId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 10000);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 3);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 3);
        QCOMPARE(cycleStopped.count(), 1);
        for (const QList<QVariant>& args : moduleFinished) {
            QCOMPARE(args.at(0).toString(), instanceId);
            QVERIFY(args.at(1).toBool());
        }
        for (const QList<QVariant>& args : runFinished) {
            const RunResult result = qvariant_cast<RunResult>(args.at(0));
            QVERIFY2(result.success, qPrintable(result.errorMessage));
        }
        QCOMPARE(fileBytes(), expectedBytes);
        QCOMPARE(QFileInfo(outputPath).size(), expectedBytes.size());
        QTRY_VERIFY_WITH_TIMEOUT(!terminalSession.isEmpty(), 5000);
        QCOMPARE(terminalSession.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(terminalSession.value(QStringLiteral("isBusy")).toBool(), false);
        QCOMPARE(terminalSession.value(QStringLiteral("maxCycles")).toInt(), 3);
        QCOMPARE(terminalSession.value(QStringLiteral("completedCycles")).toInt(), 3);
        const QString lastRunId = terminalSession.value(QStringLiteral("lastCompletedRunId")).toString();
        QVERIFY(!lastRunId.isEmpty());
        QCOMPARE(terminalSession.value(QStringLiteral("lastSuccessfulRunId")).toString(), lastRunId);
        QCOMPARE(qvariant_cast<RunResult>(runFinished.last().at(0)).runId, lastRunId);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject writeResult = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), lastRunId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("write_result")}});
        const QJsonObject writePath = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), lastRunId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("write_path")}});
        const QJsonObject writeLength = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), lastRunId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("write_length")}});
        QCOMPARE(writeResult.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(writeResult.value(QStringLiteral("value")).toBool());
        QCOMPARE(writePath.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(writePath.value(QStringLiteral("value")).toString(), outputPath);
        QCOMPARE(writeLength.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(writeLength.value(QStringLiteral("value")).toInt(), textContent.length());
        const QByteArray stableBytes = fileBytes();
        QCOMPARE(stableBytes, expectedBytes);
        QTest::qWait(550);
        QCOMPARE(fileBytes(), stableBytes);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 3);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 3);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }

    void testSaveDataAndSaveImageSuccessfulBoundedCyclesExactSideEffects() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager& projects = ProjectManager::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        auto readBytes = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            return file.readAll();
        };

        for (const bool saveImage : {false, true}) {
            projects.closeProject();
            engine.stop();
            engine.clearModules();
            controller.clearConversation();

            const QString caseName = saveImage ? QStringLiteral("saveimage") : QStringLiteral("savedata");
            const QString inputDir = m_targets.filePath(QStringLiteral("slice128-%1-input").arg(caseName));
            QVERIFY(QDir().mkpath(inputDir));
            QStringList inputNames;
            QList<QImage> inputImages;
            const QList<QRgb> colors{qRgb(23, 71, 119), qRgb(137, 43, 89), qRgb(211, 163, 37)};
            for (int index = 0; index < colors.size(); ++index) {
                const QString name = QStringLiteral("frame-%1.png").arg(index + 1, 2, 10, QLatin1Char('0'));
                QImage input(7, 5, QImage::Format_RGB32);
                input.fill(colors.at(index));
                QVERIFY(input.save(QDir(inputDir).filePath(name), "PNG"));
                inputNames.append(name);
                inputImages.append(input);
            }

            const QString outputPath = m_targets.filePath(saveImage ? QStringLiteral("slice128-saveimage-output.png")
                                                                    : QStringLiteral("slice128-savedata-output.txt"));
            const QByteArray sentinel =
                saveImage ? QByteArray("existing-image-sentinel\n") : QByteArray("existing-data-sentinel\n");
            QVERIFY(QFile::remove(outputPath) || !QFileInfo::exists(outputPath));
            {
                QFile output(outputPath);
                QVERIFY(output.open(QIODevice::WriteOnly));
                QCOMPARE(output.write(sentinel), sentinel.size());
            }

            Project* project = projects.newProject();
            QVERIFY(project);
            const QString sourceId = QStringLiteral("slice128_%1_source").arg(caseName);
            const QString targetId = QStringLiteral("slice128_%1_target").arg(caseName);
            ModuleInstance source;
            source.id = sourceId;
            source.moduleId = QStringLiteral("GrabImage");
            source.name = QStringLiteral("Slice 128 folder image source");
            source.params = {{QStringLiteral("grabSource"), QStringLiteral("Folder")},
                             {QStringLiteral("filePath"), inputDir},
                             {QStringLiteral("folderPath"), inputDir},
                             {QStringLiteral("folderLoop"), true}};
            project->addModule(source);

            ModuleInstance target;
            target.id = targetId;
            if (saveImage) {
                target.moduleId = QStringLiteral("SaveImage");
                target.name = QStringLiteral("Slice 128 bounded SaveImage target");
                target.params = {{QStringLiteral("filePath"), outputPath},
                                 {QStringLiteral("format"), QStringLiteral("png")},
                                 {QStringLiteral("quality"), 95}};
            } else {
                target.moduleId = QStringLiteral("SaveData");
                target.name = QStringLiteral("Slice 128 bounded SaveData target");
                target.params = {{QStringLiteral("filePath"), outputPath},
                                 {QStringLiteral("fileFormat"), QStringLiteral("text")},
                                 {QStringLiteral("appendMode"), true}};
            }
            project->addModule(target);

            ModuleConnection edge;
            edge.fromModuleId = sourceId;
            edge.toModuleId = targetId;
            edge.fromPort = QStringLiteral("image");
            edge.toPort = QStringLiteral("image");
            edge.edgeType = QStringLiteral("data");
            project->addConnection(edge);

            QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
            QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
            QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
            QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

            auto requestCycle = [&controller]() {
                return controller.handleToolCall(
                    QStringLiteral("run_flow"),
                    QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
            };
            QJsonObject pending = requestCycle();
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QJsonObject confirmationContext =
                controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
            const QJsonArray effects = confirmationContext.value(QStringLiteral("sideEffects")).toArray();
            QVERIFY(effects.contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
            if (saveImage)
                QVERIFY(effects.contains(QStringLiteral("file_write:%1").arg(outputPath)));
            else
                QVERIFY(effects.contains(QStringLiteral("file_write:%1 format=text mode=append").arg(outputPath)));
            QCOMPARE(readBytes(outputPath), sentinel);
            QVERIFY(!engine.isBusy());
            QCOMPARE(moduleStarted.count(), 0);
            QCOMPARE(moduleFinished.count(), 0);
            QCOMPARE(runFinished.count(), 0);
            QCOMPARE(cycleStopped.count(), 0);
            QCOMPARE(toolExecuted.count(), 0);

            const QString rejectedId = controller.pendingConfirmationId();
            QVERIFY(!rejectedId.isEmpty());
            QVERIFY(controller.rejectPendingTools(rejectedId));
            QVERIFY(!controller.hasPendingTools());
            QVERIFY(controller.pendingConfirmationPayload().isEmpty());
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
            QCOMPARE(readBytes(outputPath), sentinel);
            QVERIFY(!engine.isBusy());
            QCOMPARE(moduleStarted.count(), 0);
            QCOMPARE(moduleFinished.count(), 0);
            QCOMPARE(runFinished.count(), 0);
            QCOMPARE(cycleStopped.count(), 0);
            QCOMPARE(toolExecuted.count(), 0);

            pending = requestCycle();
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString approvedId = controller.pendingConfirmationId();
            QVERIFY(!approvedId.isEmpty());
            QVERIFY(approvedId != rejectedId);

            AgentActor* actor = controller.actor();
            QVERIFY(actor);
            QString sessionId;
            QJsonObject terminalSession;
            QObject signalContext;
            connect(actor, &AgentActor::toolExecuted, &signalContext,
                    [&sessionId](const QString& toolName, const QJsonObject& result) {
                        if (toolName == QLatin1String("run_flow"))
                            sessionId = result.value(QStringLiteral("sessionId")).toString();
                    });
            connect(
                &engine, &RunEngine::cycleStopped, &signalContext,
                [&]() {
                    terminalSession =
                        actor->executeTool(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), sessionId}});
                },
                Qt::QueuedConnection);
            QVERIFY(controller.confirmPendingTools(approvedId));
            QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
            QVERIFY(!sessionId.isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 10000);
            QCOMPARE(countModuleSignal(moduleStarted, sourceId), 3);
            QCOMPARE(countModuleSignal(moduleFinished, sourceId), 3);
            QCOMPARE(countModuleSignal(moduleStarted, targetId), 3);
            QCOMPARE(countModuleSignal(moduleFinished, targetId), 3);
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 5000);
            for (const QList<QVariant>& args : moduleFinished) {
                QVERIFY(args.at(1).toBool());
            }
            for (const QList<QVariant>& args : runFinished) {
                const RunResult result = qvariant_cast<RunResult>(args.at(0));
                QVERIFY2(result.success, qPrintable(result.errorMessage));
            }

            QTRY_VERIFY_WITH_TIMEOUT(!terminalSession.isEmpty(), 5000);
            QCOMPARE(terminalSession.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
            QCOMPARE(terminalSession.value(QStringLiteral("isBusy")).toBool(), false);
            QCOMPARE(terminalSession.value(QStringLiteral("maxCycles")).toInt(), 3);
            QCOMPARE(terminalSession.value(QStringLiteral("completedCycles")).toInt(), 3);
            const QString lastRunId = terminalSession.value(QStringLiteral("lastCompletedRunId")).toString();
            QVERIFY(!lastRunId.isEmpty());
            QCOMPARE(terminalSession.value(QStringLiteral("lastSuccessfulRunId")).toString(), lastRunId);
            QCOMPARE(qvariant_cast<RunResult>(runFinished.last().at(0)).runId, lastRunId);
            QCOMPARE(audit.count(), 2);
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
            int runFlowToolCalls = 0;
            int runResultsToolCalls = 0;
            for (const QList<QVariant>& args : toolExecuted) {
                const QString toolName = args.at(0).toString();
                runFlowToolCalls += toolName == QLatin1String("run_flow") ? 1 : 0;
                runResultsToolCalls += toolName == QLatin1String("get_run_results") ? 1 : 0;
            }
            QCOMPARE(runFlowToolCalls, 1);
            QCOMPARE(runResultsToolCalls, 1);

            if (saveImage) {
                QImage saved;
                QVERIFY(saved.load(outputPath));
                QCOMPARE(saved.size(), inputImages.constLast().size());
                for (int y = 0; y < saved.height(); ++y)
                    for (int x = 0; x < saved.width(); ++x)
                        QCOMPARE(saved.pixel(x, y), inputImages.constLast().pixel(x, y));
            } else {
                QByteArray expected = sentinel;
                for (int index = 0; index < inputNames.size(); ++index) {
                    const QList<QPair<QString, QString>> fields{
                        {QStringLiteral("folder_count"), QStringLiteral("3")},
                        {QStringLiteral("folder_path"), QDir(inputDir).absolutePath()},
                        {QStringLiteral("folder_position"), QString::number(index + 1)},
                        {QStringLiteral("source_file_name"), inputNames.at(index)},
                        {QStringLiteral("source_path"), QDir(inputDir).absoluteFilePath(inputNames.at(index))}};
                    for (const auto& field : fields)
                        expected += QStringLiteral("%1: %2\n").arg(field.first, field.second).toUtf8();
                }
                QCOMPARE(readBytes(outputPath), expected);
                QCOMPARE(QFileInfo(outputPath).size(), expected.size());
            }

            const QByteArray stableBytes = readBytes(outputPath);
            QCOMPARE(QFileInfo(outputPath).size(), stableBytes.size());
            QTest::qWait(350);
            QCOMPARE(readBytes(outputPath), stableBytes);
            QCOMPARE(countModuleSignal(moduleStarted, sourceId), 3);
            QCOMPARE(countModuleSignal(moduleFinished, sourceId), 3);
            QCOMPARE(countModuleSignal(moduleStarted, targetId), 3);
            QCOMPARE(countModuleSignal(moduleFinished, targetId), 3);
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 5000);
            QCOMPARE(cycleStopped.count(), 1);
            const QJsonObject quietSession =
                actor->executeTool(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), sessionId}});
            QCOMPARE(quietSession.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
            QCOMPARE(quietSession.value(QStringLiteral("isBusy")).toBool(), false);
            QCOMPARE(quietSession.value(QStringLiteral("maxCycles")).toInt(), 3);
            QCOMPARE(quietSession.value(QStringLiteral("completedCycles")).toInt(), 3);
            QCOMPARE(quietSession.value(QStringLiteral("lastCompletedRunId")).toString(), lastRunId);
            QCOMPARE(quietSession.value(QStringLiteral("lastSuccessfulRunId")).toString(), lastRunId);
            QCOMPARE(toolExecuted.count(), 3);
            QCOMPARE(audit.count(), 2);
            QVERIFY(!engine.isBusy());
        }

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        projects.closeProject();
    }

    void testAgentFolderDeleteBoundedCycleAuthorization() {
#ifndef Q_OS_UNIX
        QSKIP("The test requires POSIX symlink and rename semantics");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager& projects = ProjectManager::instance();
        FolderTestStateRestore stateRestore{controller, engine, m_targets, m_appDataDir};
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString instanceId = QStringLiteral("slice125_bounded_folder_delete");
        const QString stableTarget = m_targets.filePath(QStringLiteral("slice125-stable-target"));
        const QString stableCanonicalTarget = QFileInfo(stableTarget).absoluteFilePath();
        const QString stableMarker = QDir(stableTarget).filePath(QStringLiteral("marker.txt"));
        QVERIFY(QDir().mkpath(stableTarget));
        {
            QFile marker(stableMarker);
            QVERIFY(marker.open(QIODevice::WriteOnly));
            QCOMPARE(marker.write("stable"), qint64(6));
        }

        Project* project = projects.newProject();
        QVERIFY(project);
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 125 bounded Folder authorization");
        folder.params = {{QStringLiteral("operation"), QStringLiteral("Delete")},
                         {QStringLiteral("folderPath"), stableTarget}};
        project->addModule(folder);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const auto executedToolCount = [&toolExecuted](const QString& toolName) {
            int count = 0;
            for (const QList<QVariant>& args : toolExecuted) {
                if (args.at(0).toString() == toolName)
                    ++count;
            }
            return count;
        };
        const auto auditActionCount = [&audit](const QString& action) {
            int count = 0;
            for (const QList<QVariant>& args : audit) {
                if (args.at(0).value<AgentActionLogEntry>().action == action)
                    ++count;
            }
            return count;
        };
        QString cycleSessionId;
        QList<QJsonObject> observedAuthorizations;
        QMutex authorizationMutex;
        QObject signalContext;
        const QMetaObject::Connection observeAuthorization = QObject::connect(
            &engine, &RunEngine::moduleStarted, &signalContext,
            [&](const QString& startedId) {
                if (startedId != instanceId)
                    return;
                ModuleBase* module = engine.getModule(instanceId);
                if (!module)
                    return;
                QMutexLocker locker(&authorizationMutex);
                observedAuthorizations.append(module->property(AgentFolderDeleteAuthorizationProperty).toJsonObject());
            },
            Qt::DirectConnection);
        auto requestCycle = [&controller](int maxCycles, const QString& mode = QStringLiteral("cycle")) {
            return controller.handleToolCall(
                QStringLiteral("run_flow"), {{QStringLiteral("mode"), mode}, {QStringLiteral("maxCycles"), maxCycles}});
        };

        QJsonObject pending = requestCycle(1);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString stableConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!stableConfirmationId.isEmpty());
        QCOMPARE(QFileInfo::exists(stableMarker), true);
        QCOMPARE(moduleStarted.count(), 0);
        QVERIFY(controller.confirmPendingTools(stableConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 10000);
        QVERIFY(!QFileInfo::exists(stableTarget));
        const RunResult stableResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY2(stableResult.success, qPrintable(stableResult.errorMessage));
        QCOMPARE(moduleStarted.count(), 1);
        QCOMPARE(moduleFinished.count(), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(executedToolCount(QStringLiteral("run_flow")), 1);
        QCOMPARE(auditActionCount(QStringLiteral("run_flow")), 1);
        QCOMPARE(toolExecuted.first().at(0).toString(), QStringLiteral("run_flow"));
        cycleSessionId = toolExecuted.first().at(1).toJsonObject().value(QStringLiteral("sessionId")).toString();
        QVERIFY(!cycleSessionId.isEmpty());
        {
            QMutexLocker locker(&authorizationMutex);
            QCOMPARE(observedAuthorizations.size(), 1);
            const QJsonObject authorization = observedAuthorizations.first();
            QCOMPARE(authorization.value(QStringLiteral("confirmationId")).toString(), stableConfirmationId);
            QVERIFY(!authorization.value(QStringLiteral("callId")).toString().isEmpty());
            QVERIFY(!authorization.value(QStringLiteral("executionAuthorizationId")).toString().isEmpty());
            QCOMPARE(authorization.value(QStringLiteral("resolvedTarget")).toString(), stableCanonicalTarget);
        }

        // A later ordinary GUI cycle receives no session authorization.
        QVERIFY(QDir().mkpath(stableTarget));
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("operation"), QStringLiteral("Exists")));
        QVERIFY(engine.loadProject(project));
        moduleStarted.clear();
        moduleFinished.clear();
        runFinished.clear();
        cycleStopped.clear();
        toolExecuted.clear();
        audit.clear();
        {
            QMutexLocker locker(&authorizationMutex);
            observedAuthorizations.clear();
        }
        QString guiSessionId;
        QVERIFY(engine.startCycle(&guiSessionId, 100, false, true, false, 1));
        QVERIFY(!guiSessionId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 10000);
        QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
        {
            QMutexLocker locker(&authorizationMutex);
            QCOMPARE(observedAuthorizations.size(), 1);
            QVERIFY(observedAuthorizations.first().isEmpty());
        }
        QCOMPARE(moduleStarted.count(), 1);
        QCOMPARE(toolExecuted.count(), 0);
        QVERIFY(!engine.isBusy());

        // In a confirmed 3-iteration cycle, move the parent alias after iteration 1.
        const QString rootA = m_targets.filePath(QStringLiteral("slice125-root-a"));
        const QString rootB = m_targets.filePath(QStringLiteral("slice125-root-b"));
        const QString targetA = QDir(rootA).filePath(QStringLiteral("target"));
        const QString targetB = QDir(rootB).filePath(QStringLiteral("target"));
        const QString canonicalTargetA = QDir().mkpath(targetA) ? QFileInfo(targetA).canonicalFilePath() : QString();
        const QString markerA = QDir(targetA).filePath(QStringLiteral("sentinel-a.txt"));
        const QString markerB = QDir(targetB).filePath(QStringLiteral("sentinel-b.txt"));
        const QString alias = m_targets.filePath(QStringLiteral("slice125-current-root"));
        const QString replacementAlias = m_targets.filePath(QStringLiteral("slice125-replacement-root"));
        QVERIFY(QDir().mkpath(targetA));
        QVERIFY(QDir().mkpath(targetB));
        for (const QString& markerPath : {markerA, markerB}) {
            QFile marker(markerPath);
            QVERIFY(marker.open(QIODevice::WriteOnly));
            QCOMPARE(marker.write("sentinel"), qint64(8));
        }
        QVERIFY(QFile::link(rootA, alias));
        const QString configuredAliasTarget = QDir(alias).filePath(QStringLiteral("target"));
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("operation"), QStringLiteral("Delete")));
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("folderPath"), configuredAliasTarget));
        QVERIFY(engine.loadProject(project));
        moduleStarted.clear();
        moduleFinished.clear();
        runFinished.clear();
        cycleStopped.clear();
        toolExecuted.clear();
        audit.clear();
        {
            QMutexLocker locker(&authorizationMutex);
            observedAuthorizations.clear();
        }
        std::atomic_bool aliasReplaced{false};
        QMutex replacementMutex;
        QString replacementError;
        QObject replacementSignalContext;
        const QMetaObject::Connection replaceBetweenIterations = QObject::connect(
            &engine, &RunEngine::runFinished, &replacementSignalContext,
            [&](const RunResult& result) {
                if (!result.success || aliasReplaced.exchange(true))
                    return;
                if (!QFile::link(rootB, replacementAlias)) {
                    QMutexLocker locker(&replacementMutex);
                    replacementError = QStringLiteral("could not create replacement symlink");
                    return;
                }
                const QByteArray replacementName = QFile::encodeName(replacementAlias);
                const QByteArray currentName = QFile::encodeName(alias);
                if (std::rename(replacementName.constData(), currentName.constData()) != 0) {
                    QMutexLocker locker(&replacementMutex);
                    replacementError = QStringLiteral("could not atomically replace symlink");
                }
            },
            Qt::DirectConnection);
        pending = requestCycle(3);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(executedToolCount(QStringLiteral("run_flow")), 1, 5000);
        cycleSessionId = toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("sessionId")).toString();
        QVERIFY(!cycleSessionId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QObject::disconnect(replaceBetweenIterations);
        QVERIFY(aliasReplaced.load());
        {
            QMutexLocker locker(&replacementMutex);
            QVERIFY2(replacementError.isEmpty(), qPrintable(replacementError));
        }
        QCOMPARE(QFileInfo(alias).symLinkTarget(), QFileInfo(rootB).canonicalFilePath());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 10000);
        QCOMPARE(moduleStarted.count(), 2);
        QCOMPARE(moduleFinished.count(), 2);
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(executedToolCount(QStringLiteral("run_flow")), 1);
        QCOMPARE(auditActionCount(QStringLiteral("run_flow")), 1);
        {
            QMutexLocker locker(&authorizationMutex);
            QCOMPARE(observedAuthorizations.size(), 2);
            const QJsonObject firstAuthorization = observedAuthorizations.at(0);
            const QJsonObject secondAuthorization = observedAuthorizations.at(1);
            QCOMPARE(firstAuthorization.value(QStringLiteral("confirmationId")).toString(), confirmationId);
            QCOMPARE(secondAuthorization.value(QStringLiteral("confirmationId")).toString(), confirmationId);
            QCOMPARE(firstAuthorization.value(QStringLiteral("callId")).toString(),
                     secondAuthorization.value(QStringLiteral("callId")).toString());
            QCOMPARE(firstAuthorization.value(QStringLiteral("executionAuthorizationId")).toString(),
                     secondAuthorization.value(QStringLiteral("executionAuthorizationId")).toString());
            QCOMPARE(firstAuthorization.value(QStringLiteral("resolvedTarget")).toString(), canonicalTargetA);
            QCOMPARE(secondAuthorization.value(QStringLiteral("resolvedTarget")).toString(), canonicalTargetA);
        }
        const RunResult firstIteration = qvariant_cast<RunResult>(runFinished.first().at(0));
        QVERIFY2(firstIteration.success, qPrintable(firstIteration.errorMessage));
        const RunResult failedIteration = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!failedIteration.success);
        QCOMPARE(failedIteration.failedModule, instanceId);
        QCOMPARE(failedIteration.errorCode, ExecError::ConfirmedTargetChanged);
        QVERIFY(failedIteration.diagnostics.contains(QStringLiteral("CONFIRMED_TARGET_CHANGED")));
        QVERIFY(!QFileInfo::exists(markerA));
        QVERIFY(QFileInfo::exists(markerB));
        const QJsonObject failedSession =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), cycleSessionId}});
        QCOMPARE(failedSession.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(failedSession.value(QStringLiteral("completedCycles")).toInt(), 1);
        const QJsonObject lastError = failedSession.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(lastError.value(QStringLiteral("code")).toInt(), ExecError::ConfirmedTargetChanged);
        QCOMPARE(lastError.value(QStringLiteral("codeName")).toString(), QStringLiteral("CONFIRMED_TARGET_CHANGED"));
        QCOMPARE(lastError.value(QStringLiteral("moduleId")).toString(), instanceId);
        QElapsedTimer quietWindow;
        quietWindow.start();
        while (quietWindow.elapsed() < 350) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        QCOMPARE(moduleStarted.count(), 2);
        QCOMPARE(moduleFinished.count(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(executedToolCount(QStringLiteral("run_flow")), 1);
        QCOMPARE(auditActionCount(QStringLiteral("run_flow")), 1);
        QVERIFY(QFileInfo::exists(markerB));
        const QJsonObject stableFailedSession =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), cycleSessionId}});
        QCOMPARE(stableFailedSession.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(stableFailedSession.value(QStringLiteral("completedCycles")).toInt(), 1);
        QCOMPARE(
            stableFailedSession.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("code")).toInt(),
            ExecError::ConfirmedTargetChanged);
        QVERIFY(!engine.isBusy());
        QObject::disconnect(observeAuthorization);
        QFile::remove(alias);
        QFile::remove(replacementAlias);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
#endif
    }

    void testFolderCreateRechecksApprovedTargetAtExecutionBoundary() {
#ifndef Q_OS_UNIX
        QSKIP("The test requires POSIX directory symlink and rename semantics");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager& projects = ProjectManager::instance();
        FolderTestStateRestore stateRestore{controller, engine, m_targets, m_appDataDir};
        const QStringList entryPoints{QStringLiteral("run"), QStringLiteral("step"), QStringLiteral("resume")};

        for (const QString& entryPoint : entryPoints) {
            projects.closeProject();
            engine.stop();
            engine.clearModules();
            controller.clearConversation();
            controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

            const QString rootA = m_targets.filePath(QStringLiteral("slice126-%1-root-a").arg(entryPoint));
            const QString rootB = m_targets.filePath(QStringLiteral("slice126-%1-root-b").arg(entryPoint));
            const QString link = m_targets.filePath(QStringLiteral("slice126-%1-current-root").arg(entryPoint));
            const QString replacementLink = m_targets.filePath(QStringLiteral("slice126-%1-next-root").arg(entryPoint));
            QVERIFY(QDir().mkpath(rootA));
            QVERIFY(QDir().mkpath(rootB));
            if (!QFile::link(rootA, link) || !QFileInfo(link).isSymLink())
                QSKIP("The current filesystem does not provide directory symlink support");
            if (!QFile::link(rootB, replacementLink) || !QFileInfo(replacementLink).isSymLink())
                QSKIP("The current filesystem does not provide a second directory symlink");

            Project* project = projects.newProject();
            QVERIFY(project);
            const QString instanceId = QStringLiteral("slice126_folder_create_%1").arg(entryPoint);
            const QString configuredPath = QDir(link).filePath(QStringLiteral("created/nested"));
            const QString targetA = QDir(rootA).filePath(QStringLiteral("created/nested"));
            const QString targetB = QDir(rootB).filePath(QStringLiteral("created/nested"));
            ModuleInstance folder;
            folder.id = instanceId;
            folder.moduleId = QStringLiteral("Folder");
            folder.name = QStringLiteral("Slice 126 execution-boundary Folder create");
            folder.breakpoint = entryPoint == QLatin1String("resume");
            folder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                        {QStringLiteral("folderPath"), configuredPath}};
            project->addModule(folder);

            QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
            QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

            if (entryPoint == QLatin1String("resume")) {
                QVERIFY(engine.loadProject(project));
                QString pausedSessionId;
                QVERIFY(engine.startCycle(&pausedSessionId, 100, false, true, false, 1));
                QTRY_VERIFY_WITH_TIMEOUT(engine.isPausedAtBreakpoint(), 5000);
                moduleStarted.clear();
                moduleFinished.clear();
                runFinished.clear();
            }

            const QString toolName = entryPoint == QLatin1String("run")    ? QStringLiteral("run_flow")
                                     : entryPoint == QLatin1String("step") ? QStringLiteral("step_once")
                                                                           : QStringLiteral("resume_flow");
            const QJsonObject toolParams = toolName == QLatin1String("run_flow")
                                               ? QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}}
                                               : QJsonObject{};
            QJsonObject pending = controller.handleToolCall(toolName, toolParams);
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString confirmationId = controller.pendingConfirmationId();
            QVERIFY(!confirmationId.isEmpty());
            const QString expectedEffect =
                QStringLiteral("folder_create:%1@%2 resolved_target=%3")
                    .arg(instanceId, configuredPath, QDir(rootA).filePath(QStringLiteral("created/nested")));
            const QJsonArray effects = controller.pendingConfirmationPayload()
                                           .value(QStringLiteral("context"))
                                           .toObject()
                                           .value(QStringLiteral("sideEffects"))
                                           .toArray();
            QVERIFY(effects.contains(expectedEffect));
            QVERIFY(!QFileInfo::exists(targetA));
            QVERIFY(!QFileInfo::exists(targetB));

            QString swapError;
            QObject signalContext;
            const QMetaObject::Connection swapConnection = QObject::connect(
                &engine, &RunEngine::moduleStarted, &signalContext,
                [&](const QString& startedId) {
                    if (startedId != instanceId)
                        return;
                    const QByteArray replacementName = QFile::encodeName(replacementLink);
                    const QByteArray currentName = QFile::encodeName(link);
                    if (std::rename(replacementName.constData(), currentName.constData()) != 0)
                        swapError = QStringLiteral("atomic symlink replacement failed");
                },
                Qt::DirectConnection);
            QVERIFY(controller.confirmPendingTools(confirmationId));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QObject::disconnect(swapConnection);
            QVERIFY2(swapError.isEmpty(), qPrintable(swapError));
            QCOMPARE(QFileInfo(link).symLinkTarget(), QFileInfo(rootB).canonicalFilePath());
            const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY(!failed.success);
            QCOMPARE(failed.errorCode, ExecError::ConfirmedTargetChanged);
            QCOMPARE(failed.failedModule, instanceId);
            QVERIFY(failed.diagnostics.contains(QStringLiteral("CONFIRMED_TARGET_CHANGED")));
            QCOMPARE(moduleStarted.count(), 1);
            QCOMPARE(moduleFinished.count(), 1);
            QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
            QVERIFY(!moduleFinished.last().at(1).toBool());
            QVERIFY(!QFileInfo::exists(targetA));
            QVERIFY(!QFileInfo::exists(targetB));
            QCOMPARE(audit.count(), 1);
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, toolName);
            const QJsonObject failedQuery =
                controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), failed.runId}});
            QCOMPARE(failedQuery.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
            const QJsonObject lastError = failedQuery.value(QStringLiteral("lastError")).toObject();
            QCOMPARE(lastError.value(QStringLiteral("code")).toInt(), ExecError::ConfirmedTargetChanged);
            QCOMPARE(lastError.value(QStringLiteral("codeName")).toString(),
                     QStringLiteral("CONFIRMED_TARGET_CHANGED"));
            QVERIFY(!engine.isBusy());
            QFile::remove(link);
            QFile::remove(replacementLink);
        }

        // The accepted bounded-cycle authorization covers one static Create exactly once.
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        Project* singleCycleProject = projects.newProject();
        QVERIFY(singleCycleProject);
        const QString singleCycleId = QStringLiteral("slice126_folder_create_single_cycle");
        const QString singleCycleTarget = m_targets.filePath(QStringLiteral("slice126-single-cycle-create"));
        ModuleInstance singleCycleFolder;
        singleCycleFolder.id = singleCycleId;
        singleCycleFolder.moduleId = QStringLiteral("Folder");
        singleCycleFolder.name = QStringLiteral("Slice 126 maxCycles one Folder create");
        singleCycleFolder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                               {QStringLiteral("folderPath"), singleCycleTarget}};
        singleCycleProject->addModule(singleCycleFolder);
        QSignalSpy cycleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        const QJsonObject cyclePending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 1}});
        QCOMPARE(cyclePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 5000);
        QCOMPARE(countModuleSignal(cycleStarted, singleCycleId), 1);
        QVERIFY(QDir(singleCycleTarget).exists());
        QVERIFY(!engine.isBusy());

        // A direct GUI/engine run has no Agent authorization and keeps its ordinary Create behavior.
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        Project* guiProject = projects.newProject();
        QVERIFY(guiProject);
        const QString guiInstanceId = QStringLiteral("slice126_folder_create_gui");
        const QString guiTarget = m_targets.filePath(QStringLiteral("slice126-gui-create"));
        ModuleInstance guiFolder;
        guiFolder.id = guiInstanceId;
        guiFolder.moduleId = QStringLiteral("Folder");
        guiFolder.name = QStringLiteral("Slice 126 ordinary GUI Folder create");
        guiFolder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                       {QStringLiteral("folderPath"), guiTarget}};
        guiProject->addModule(guiFolder);
        QVERIFY(engine.loadProject(guiProject));
        QSignalSpy guiRunFinished(&engine, &RunEngine::runFinished);
        QJsonObject guiAuthorization;
        QObject guiSignalContext;
        connect(
            &engine, &RunEngine::moduleStarted, &guiSignalContext,
            [&](const QString& startedId) {
                if (startedId == guiInstanceId) {
                    if (ModuleBase* module = engine.getModule(guiInstanceId))
                        guiAuthorization = module->property(AgentFolderDeleteAuthorizationProperty).toJsonObject();
                }
            },
            Qt::DirectConnection);
        engine.runOnce();
        QTRY_COMPARE_WITH_TIMEOUT(guiRunFinished.count(), 1, 5000);
        const RunResult guiResult = qvariant_cast<RunResult>(guiRunFinished.last().at(0));
        QVERIFY2(guiResult.success, qPrintable(guiResult.errorMessage));
        QVERIFY(QDir(guiTarget).exists());
        QVERIFY(!guiAuthorization.value(QStringLiteral("active")).toBool());
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
#endif
    }

    void testFolderCreateBoundedCycleRechecksApprovedTargetAtExecutionBoundary() {
#ifndef Q_OS_UNIX
        QSKIP("The test requires POSIX directory symlink and rename semantics");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager& projects = ProjectManager::instance();
        AgentActor* actor = controller.actor();
        QVERIFY(actor);

        // A symlink switch after the Agent returns the accepted session but before its first tick fails closed.
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QString rootA = m_targets.filePath(QStringLiteral("slice126-cycle-first-root-a"));
        const QString rootB = m_targets.filePath(QStringLiteral("slice126-cycle-first-root-b"));
        const QString alias = m_targets.filePath(QStringLiteral("slice126-cycle-first-current"));
        const QString replacementAlias = m_targets.filePath(QStringLiteral("slice126-cycle-first-next"));
        QVERIFY(QDir().mkpath(rootA));
        QVERIFY(QDir().mkpath(rootB));
        if (!QFile::link(rootA, alias) || !QFileInfo(alias).isSymLink())
            QSKIP("The current filesystem does not provide directory symlink support");
        if (!QFile::link(rootB, replacementAlias) || !QFileInfo(replacementAlias).isSymLink())
            QSKIP("The current filesystem does not provide a second directory symlink");
        Project* firstTickProject = projects.newProject();
        QVERIFY(firstTickProject);
        const QString firstTickId = QStringLiteral("slice126_folder_create_cycle_first_tick");
        const QString configuredFirstTickPath = QDir(alias).filePath(QStringLiteral("created/nested"));
        const QString firstTickTargetA = QDir(rootA).filePath(QStringLiteral("created/nested"));
        const QString firstTickTargetB = QDir(rootB).filePath(QStringLiteral("created/nested"));
        ModuleInstance firstTickFolder;
        firstTickFolder.id = firstTickId;
        firstTickFolder.moduleId = QStringLiteral("Folder");
        firstTickFolder.name = QStringLiteral("Slice 126 first-tick Folder create");
        firstTickFolder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                             {QStringLiteral("folderPath"), configuredFirstTickPath}};
        firstTickProject->addModule(firstTickFolder);

        QSignalSpy firstTickStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy firstTickFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy firstTickRunFinished(&engine, &RunEngine::runFinished);
        QSignalSpy firstTickCycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy firstTickToolExecuted(actor, &AgentActor::toolExecuted);
        QSignalSpy firstTickAudit(&controller, &AgentController::actionLogEntryAdded);
        QString firstTickSessionId;
        QString firstTickSwapError;
        QObject firstTickSignalContext;
        const QMetaObject::Connection firstTickSwapConnection = QObject::connect(
            actor, &AgentActor::toolExecuted, &firstTickSignalContext,
            [&](const QString& toolName, const QJsonObject& result) {
                if (toolName != QLatin1String("run_flow") ||
                    result.value(QStringLiteral("status")).toString() != QLatin1String("started"))
                    return;
                firstTickSessionId = result.value(QStringLiteral("sessionId")).toString();
                const QByteArray replacementName = QFile::encodeName(replacementAlias);
                const QByteArray currentName = QFile::encodeName(alias);
                if (std::rename(replacementName.constData(), currentName.constData()) != 0)
                    firstTickSwapError = QStringLiteral("atomic symlink replacement failed");
            },
            Qt::DirectConnection);
        const QJsonObject firstTickPending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        QCOMPARE(firstTickPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString firstTickConfirmation = controller.pendingConfirmationId();
        QVERIFY(!firstTickConfirmation.isEmpty());
        QVERIFY(controller.confirmPendingTools(firstTickConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(firstTickCycleStopped.count(), 1, 10000);
        QObject::disconnect(firstTickSwapConnection);
        QVERIFY(!firstTickSessionId.isEmpty());
        QVERIFY2(firstTickSwapError.isEmpty(), qPrintable(firstTickSwapError));
        QCOMPARE(QFileInfo(alias).symLinkTarget(), QFileInfo(rootB).canonicalFilePath());
        if (firstTickRunFinished.count() > 1) {
            qWarning() << "bounded-cycle unexpected runFinished count:" << firstTickRunFinished.count();
            for (const QList<QVariant>& arguments : firstTickRunFinished) {
                const RunResult result = qvariant_cast<RunResult>(arguments.at(0));
                qWarning() << "bounded-cycle runFinished:" << result.runId << result.success << result.errorCode
                           << result.failedModule << result.errorMessage;
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(firstTickRunFinished.count(), 1, 5000);
        QCOMPARE(firstTickStarted.count(), 1);
        QCOMPARE(firstTickFinished.count(), 1);
        QVERIFY(!firstTickFinished.last().at(1).toBool());
        const RunResult firstTickFailure = qvariant_cast<RunResult>(firstTickRunFinished.last().at(0));
        QVERIFY(!firstTickFailure.success);
        QCOMPARE(firstTickFailure.errorCode, ExecError::ConfirmedTargetChanged);
        QCOMPARE(firstTickFailure.failedModule, firstTickId);
        QVERIFY(!QFileInfo::exists(firstTickTargetA));
        QVERIFY(!QFileInfo::exists(firstTickTargetB));
        QCOMPARE(firstTickToolExecuted.count(), 1);
        QCOMPARE(firstTickAudit.count(), 1);
        const QJsonObject firstTickSession = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                                       {{QStringLiteral("runId"), firstTickSessionId}});
        QCOMPARE(firstTickSession.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(firstTickSession.value(QStringLiteral("completedCycles")).toInt(), 0);
        const QJsonObject firstTickLastError = firstTickSession.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(firstTickLastError.value(QStringLiteral("code")).toInt(), ExecError::ConfirmedTargetChanged);
        QCOMPARE(firstTickLastError.value(QStringLiteral("codeName")).toString(),
                 QStringLiteral("CONFIRMED_TARGET_CHANGED"));
        QTest::qWait(350);
        QCOMPARE(firstTickStarted.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(firstTickRunFinished.count(), 1, 5000);
        QCOMPARE(firstTickCycleStopped.count(), 1);
        QVERIFY(!engine.isBusy());
        QFile::remove(alias);
        QFile::remove(replacementAlias);

        // The first approved Create succeeds at A; after it completes, switching the parent before tick 2
        // rejects the second iteration and permanently stops the bounded session.
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        const QString secondRootA = m_targets.filePath(QStringLiteral("slice126-cycle-second-root-a"));
        const QString secondRootB = m_targets.filePath(QStringLiteral("slice126-cycle-second-root-b"));
        const QString secondAlias = m_targets.filePath(QStringLiteral("slice126-cycle-second-current"));
        const QString secondReplacementAlias = m_targets.filePath(QStringLiteral("slice126-cycle-second-next"));
        QVERIFY(QDir().mkpath(secondRootA));
        QVERIFY(QDir().mkpath(secondRootB));
        if (!QFile::link(secondRootA, secondAlias) || !QFileInfo(secondAlias).isSymLink())
            QSKIP("The current filesystem does not provide directory symlink support");
        if (!QFile::link(secondRootB, secondReplacementAlias) || !QFileInfo(secondReplacementAlias).isSymLink())
            QSKIP("The current filesystem does not provide a second directory symlink");
        Project* secondTickProject = projects.newProject();
        QVERIFY(secondTickProject);
        const QString secondTickId = QStringLiteral("slice126_folder_create_cycle_second_tick");
        const QString configuredSecondPath = QDir(secondAlias).filePath(QStringLiteral("created/nested"));
        const QString secondTargetA = QDir(secondRootA).filePath(QStringLiteral("created/nested"));
        const QString secondTargetB = QDir(secondRootB).filePath(QStringLiteral("created/nested"));
        ModuleInstance secondTickFolder;
        secondTickFolder.id = secondTickId;
        secondTickFolder.moduleId = QStringLiteral("Folder");
        secondTickFolder.name = QStringLiteral("Slice 126 second-tick Folder create");
        secondTickFolder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                              {QStringLiteral("folderPath"), configuredSecondPath}};
        secondTickProject->addModule(secondTickFolder);

        QSignalSpy secondTickStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy secondTickFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy secondTickRunFinished(&engine, &RunEngine::runFinished);
        QSignalSpy secondTickCycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy secondTickToolExecuted(actor, &AgentActor::toolExecuted);
        QSignalSpy secondTickAudit(&controller, &AgentController::actionLogEntryAdded);
        QString secondTickSessionId;
        std::atomic_bool aliasSwapped{false};
        bool firstRunSawCreatedA = false;
        QMutex swapMutex;
        QString secondTickSwapError;
        QObject secondTickSignalContext;
        const QMetaObject::Connection replaceBetweenTicks = QObject::connect(
            &engine, &RunEngine::runFinished, &secondTickSignalContext,
            [&](const RunResult& result) {
                if (!result.success || aliasSwapped.exchange(true))
                    return;
                firstRunSawCreatedA = QDir(secondTargetA).exists();
                const QByteArray replacementName = QFile::encodeName(secondReplacementAlias);
                const QByteArray currentName = QFile::encodeName(secondAlias);
                if (std::rename(replacementName.constData(), currentName.constData()) != 0) {
                    QMutexLocker locker(&swapMutex);
                    secondTickSwapError = QStringLiteral("atomic symlink replacement failed");
                }
            },
            Qt::DirectConnection);
        QObject secondTickSessionContext;
        connect(actor, &AgentActor::toolExecuted, &secondTickSessionContext,
                [&secondTickSessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow") &&
                        result.value(QStringLiteral("status")).toString() == QLatin1String("started"))
                        secondTickSessionId = result.value(QStringLiteral("sessionId")).toString();
                });
        const QJsonObject secondTickPending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        QCOMPARE(secondTickPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString secondTickConfirmation = controller.pendingConfirmationId();
        QVERIFY(controller.confirmPendingTools(secondTickConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(secondTickCycleStopped.count(), 1, 10000);
        QObject::disconnect(replaceBetweenTicks);
        QVERIFY(aliasSwapped.load());
        QVERIFY(firstRunSawCreatedA);
        {
            QMutexLocker locker(&swapMutex);
            QVERIFY2(secondTickSwapError.isEmpty(), qPrintable(secondTickSwapError));
        }
        QCOMPARE(QFileInfo(secondAlias).symLinkTarget(), QFileInfo(secondRootB).canonicalFilePath());
        QTRY_COMPARE_WITH_TIMEOUT(secondTickRunFinished.count(), 2, 5000);
        QCOMPARE(secondTickStarted.count(), 2);
        QCOMPARE(secondTickFinished.count(), 2);
        QCOMPARE(secondTickToolExecuted.count(), 1);
        QCOMPARE(secondTickAudit.count(), 1);
        const RunResult secondTickSuccess = qvariant_cast<RunResult>(secondTickRunFinished.first().at(0));
        QVERIFY2(secondTickSuccess.success, qPrintable(secondTickSuccess.errorMessage));
        const RunResult secondTickFailure = qvariant_cast<RunResult>(secondTickRunFinished.last().at(0));
        QVERIFY(!secondTickFailure.success);
        QCOMPARE(secondTickFailure.failedModule, secondTickId);
        QCOMPARE(secondTickFailure.errorCode, ExecError::ConfirmedTargetChanged);
        QVERIFY(secondTickFailure.diagnostics.contains(QStringLiteral("CONFIRMED_TARGET_CHANGED")));
        QVERIFY(QDir(secondTargetA).exists());
        QVERIFY(!QFileInfo::exists(secondTargetB));
        const QJsonObject failedSession = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                                    {{QStringLiteral("runId"), secondTickSessionId}});
        QCOMPARE(failedSession.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(failedSession.value(QStringLiteral("completedCycles")).toInt(), 1);
        const QJsonObject secondTickLastError = failedSession.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(secondTickLastError.value(QStringLiteral("code")).toInt(), ExecError::ConfirmedTargetChanged);
        QCOMPARE(secondTickLastError.value(QStringLiteral("codeName")).toString(),
                 QStringLiteral("CONFIRMED_TARGET_CHANGED"));
        QCOMPARE(secondTickLastError.value(QStringLiteral("moduleId")).toString(), secondTickId);
        const QJsonObject failedCreateOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), secondTickFailure.runId},
                                                  {QStringLiteral("instanceId"), secondTickId},
                                                  {QStringLiteral("key"), QStringLiteral("operation_result")}});
        QVERIFY(failedCreateOutput.value(QStringLiteral("status")).toString() != QStringLiteral("available") ||
                !failedCreateOutput.value(QStringLiteral("value")).toBool());
        QElapsedTimer quietWindow;
        quietWindow.start();
        while (quietWindow.elapsed() < 350) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        QCOMPARE(secondTickStarted.count(), 2);
        QCOMPARE(secondTickFinished.count(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(secondTickRunFinished.count(), 2, 5000);
        QCOMPARE(secondTickCycleStopped.count(), 1);
        const QJsonObject stableSession = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                                    {{QStringLiteral("runId"), secondTickSessionId}});
        QCOMPARE(stableSession.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(stableSession.value(QStringLiteral("completedCycles")).toInt(), 1);
        QVERIFY(!engine.isBusy());
        QFile::remove(secondAlias);
        QFile::remove(secondReplacementAlias);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
#endif
    }

    void testFolderDeleteRechecksApprovedTargetAtExecutionBoundary() {
#ifndef Q_OS_UNIX
        QSKIP("The test requires POSIX symlink and rename semantics");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager& projects = ProjectManager::instance();
        const QStringList entryPoints{QStringLiteral("run"), QStringLiteral("step"), QStringLiteral("resume")};

        for (const QString& entryPoint : entryPoints) {
            projects.closeProject();
            engine.stop();
            engine.clearModules();
            controller.clearConversation();
            controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

            const QString rootA = m_targets.filePath(QStringLiteral("slice124-%1-root-a").arg(entryPoint));
            const QString rootB = m_targets.filePath(QStringLiteral("slice124-%1-root-b").arg(entryPoint));
            const QString targetA = QDir(rootA).filePath(QStringLiteral("target"));
            const QString targetB = QDir(rootB).filePath(QStringLiteral("target"));
            const QString link = m_targets.filePath(QStringLiteral("slice124-%1-current-root").arg(entryPoint));
            const QString replacementLink = m_targets.filePath(QStringLiteral("slice124-%1-next-root").arg(entryPoint));
            const QString instanceId = QStringLiteral("slice124_folder_delete_%1").arg(entryPoint);
            const QString markerA = QDir(targetA).filePath(QStringLiteral("marker.txt"));
            const QString markerB = QDir(targetB).filePath(QStringLiteral("marker.txt"));
            QVERIFY(QDir().mkpath(targetA));
            QVERIFY(QDir().mkpath(targetB));
            const auto writeMarker = [](const QString& path) {
                QFile file(path);
                return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write("sentinel") == 8 &&
                       file.flush();
            };
            QVERIFY(writeMarker(markerA));
            QVERIFY(writeMarker(markerB));
            if (!QFile::link(rootA, link) || !QFileInfo(link).isSymLink())
                QSKIP("The current filesystem does not provide directory symlink support");

            Project* project = projects.newProject();
            QVERIFY(project);
            ModuleInstance folder;
            folder.id = instanceId;
            folder.moduleId = QStringLiteral("Folder");
            folder.name = QStringLiteral("Slice 124 execution-boundary Folder delete");
            folder.breakpoint = entryPoint == QLatin1String("resume");
            folder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Delete")},
                                        {QStringLiteral("folderPath"), QDir(link).filePath(QStringLiteral("target"))}};
            project->addModule(folder);
            const QString configuredPath = folder.params.value(QStringLiteral("folderPath")).toString();

            QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
            QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
            QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

            if (entryPoint == QLatin1String("resume")) {
                QVERIFY(engine.loadProject(project));
                QString sessionId;
                QVERIFY(engine.startCycle(&sessionId, 100, false, true, false, 1));
                QTRY_VERIFY_WITH_TIMEOUT(engine.isPausedAtBreakpoint(), 5000);
                moduleStarted.clear();
                moduleFinished.clear();
                runFinished.clear();
            }

            QString toolName = entryPoint == QLatin1String("run")    ? QStringLiteral("run_flow")
                               : entryPoint == QLatin1String("step") ? QStringLiteral("step_once")
                                                                     : QStringLiteral("resume_flow");
            QJsonObject toolParams;
            if (toolName == QLatin1String("run_flow"))
                toolParams.insert(QStringLiteral("mode"), QStringLiteral("once"));
            QJsonObject pending = controller.handleToolCall(toolName, toolParams);
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString preApprovalDriftId = controller.pendingConfirmationId();
            QVERIFY(!preApprovalDriftId.isEmpty());
            QVERIFY(project->setModuleParam(instanceId, QStringLiteral("operation"), QStringLiteral("Create")));
            QVERIFY(controller.confirmPendingTools(preApprovalDriftId));
            QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
            QCOMPARE(moduleStarted.count(), 0);
            QCOMPARE(runFinished.count(), 0);
            QVERIFY(QFileInfo::exists(markerA));
            QVERIFY(QFileInfo::exists(markerB));
            QVERIFY(project->setModuleParam(instanceId, QStringLiteral("operation"), QStringLiteral("Delete")));

            if (entryPoint == QLatin1String("resume")) {
                // Rebuild the breakpoint snapshot after mutating and restoring project parameters above.
                // Project modification time is part of the resume fingerprint even when JSON values match.
                engine.stop();
                engine.clearModules();
                QVERIFY(engine.loadProject(project));
                QString refreshedSessionId;
                QVERIFY(engine.startCycle(&refreshedSessionId, 100, false, true, false, 1));
                QTRY_VERIFY_WITH_TIMEOUT(engine.isPausedAtBreakpoint(), 5000);
                moduleStarted.clear();
                moduleFinished.clear();
                runFinished.clear();
            }

            pending = controller.handleToolCall(toolName, toolParams);
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString rejectedConfirmationId = controller.pendingConfirmationId();
            QVERIFY(!rejectedConfirmationId.isEmpty());
            QVERIFY(QFileInfo::exists(markerA));
            QVERIFY(QFileInfo::exists(markerB));
            QCOMPARE(moduleStarted.count(), 0);
            QVERIFY(controller.rejectPendingTools(rejectedConfirmationId));
            QVERIFY(QFileInfo::exists(markerA));
            QVERIFY(QFileInfo::exists(markerB));
            QCOMPARE(moduleStarted.count(), 0);
            pending = controller.handleToolCall(toolName, toolParams);
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString operationDriftConfirmationId = controller.pendingConfirmationId();
            QVERIFY(!operationDriftConfirmationId.isEmpty());
            QVERIFY(operationDriftConfirmationId != rejectedConfirmationId);
            const QString expectedA = QStringLiteral("folder_delete:%1@%2 resolved_target=%3")
                                          .arg(instanceId, configuredPath, QDir(targetA).canonicalPath());
            const QJsonArray staleEffects = controller.pendingConfirmationPayload()
                                                .value(QStringLiteral("context"))
                                                .toObject()
                                                .value(QStringLiteral("sideEffects"))
                                                .toArray();
            QVERIFY(staleEffects.contains(expectedA));
            QCOMPARE(moduleStarted.count(), 0);
            QCOMPARE(runFinished.count(), 0);
            QVERIFY(QFileInfo::exists(markerA));
            QVERIFY(QFileInfo::exists(markerB));

            const QString unauthorizedCreatePath = QDir(rootB).filePath(QStringLiteral("unapproved-create"));
            std::atomic_bool operationMutated{false};
            QMutex operationAuthorizationMutex;
            QJsonObject operationDriftAuthorization;
            const QMetaObject::Connection operationDriftConnection = QObject::connect(
                &engine, &RunEngine::moduleStarted, &engine,
                [&](const QString& startedId) {
                    if (startedId != instanceId)
                        return;
                    ModuleBase* module = engine.getModule(instanceId);
                    if (!module)
                        return;
                    QJsonObject params = module->currentParams();
                    params.insert(QStringLiteral("operation"), QStringLiteral("Create"));
                    params.insert(QStringLiteral("folderPath"), unauthorizedCreatePath);
                    module->setParams(params);
                    QMutexLocker locker(&operationAuthorizationMutex);
                    operationDriftAuthorization =
                        module->property(AgentFolderDeleteAuthorizationProperty).toJsonObject();
                    operationMutated.store(true);
                },
                Qt::DirectConnection);
            QVERIFY(controller.confirmPendingTools(operationDriftConfirmationId));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QObject::disconnect(operationDriftConnection);
            QVERIFY(operationMutated.load());
            const RunResult operationDriftResult = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY(!operationDriftResult.success);
            QCOMPARE(operationDriftResult.errorCode, ExecError::ConfirmedTargetChanged);
            {
                QMutexLocker locker(&operationAuthorizationMutex);
                QCOMPARE(operationDriftAuthorization.value(QStringLiteral("approvedOperation")).toString(),
                         QStringLiteral("Delete"));
            }
            QVERIFY(!QDir(unauthorizedCreatePath).exists());
            QVERIFY(QFileInfo::exists(markerA));
            QVERIFY(QFileInfo::exists(markerB));
            const QJsonObject operationDriftQuery = controller.handleToolCall(
                QStringLiteral("get_run_results"), {{QStringLiteral("runId"), operationDriftResult.runId}});
            QCOMPARE(operationDriftQuery.value(QStringLiteral("lastError"))
                         .toObject()
                         .value(QStringLiteral("codeName"))
                         .toString(),
                     QStringLiteral("CONFIRMED_TARGET_CHANGED"));

            if (entryPoint == QLatin1String("step"))
                engine.resetStepState();
            if (entryPoint == QLatin1String("resume")) {
                engine.clearModules();
                QVERIFY(engine.loadProject(project));
                QString sessionId;
                QVERIFY(engine.startCycle(&sessionId, 100, false, true, false, 1));
                QTRY_VERIFY_WITH_TIMEOUT(engine.isPausedAtBreakpoint(), 5000);
            } else if (ModuleBase* module = engine.getModule(instanceId)) {
                QJsonObject params = module->currentParams();
                params.insert(QStringLiteral("operation"), QStringLiteral("Delete"));
                params.insert(QStringLiteral("folderPath"), configuredPath);
                module->setParams(params);
            }
            moduleStarted.clear();
            moduleFinished.clear();
            runFinished.clear();
            toolExecuted.clear();
            audit.clear();
            pending = controller.handleToolCall(toolName, toolParams);
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString staleConfirmationId = controller.pendingConfirmationId();
            QVERIFY(!staleConfirmationId.isEmpty());
            QVERIFY(staleConfirmationId != operationDriftConfirmationId);

            QMutex authorizationMutex;
            QJsonObject observedAuthorization;
            QString swapError;
            QString competingOrdinaryRunId;
            std::atomic_bool competingOrdinaryRunAccepted{false};
            QMutex competingOrdinaryRunMutex;
            std::atomic_bool swapped{false};
            QString staleAuthorizationId;
            const QMetaObject::Connection mutationConnection = QObject::connect(
                &engine, &RunEngine::moduleStarted, &engine,
                [&](const QString& startedId) {
                    if (startedId != instanceId || swapped.exchange(true))
                        return;
                    if (ModuleBase* module = engine.getModule(instanceId)) {
                        QMutexLocker locker(&authorizationMutex);
                        observedAuthorization = module->property(AgentFolderDeleteAuthorizationProperty).toJsonObject();
                    }
                    QString competingRunId;
                    const bool competingRunAccepted = engine.runOnceAsync(&competingRunId);
                    {
                        QMutexLocker locker(&competingOrdinaryRunMutex);
                        competingOrdinaryRunId = competingRunId;
                    }
                    competingOrdinaryRunAccepted.store(competingRunAccepted);
                    if (!QFile::link(rootB, replacementLink)) {
                        QMutexLocker locker(&authorizationMutex);
                        swapError = QStringLiteral("could not create replacement symlink");
                        return;
                    }
                    const QByteArray replacementName = QFile::encodeName(replacementLink);
                    const QByteArray currentName = QFile::encodeName(link);
                    if (std::rename(replacementName.constData(), currentName.constData()) != 0) {
                        QMutexLocker locker(&authorizationMutex);
                        swapError = QStringLiteral("could not atomically replace symlink");
                    }
                },
                Qt::DirectConnection);
            QVERIFY(controller.confirmPendingTools(staleConfirmationId));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QObject::disconnect(mutationConnection);
            QVERIFY(swapped.load());
            QVERIFY(!competingOrdinaryRunAccepted.load());
            {
                QMutexLocker locker(&competingOrdinaryRunMutex);
                QVERIFY(competingOrdinaryRunId.isEmpty());
            }
            {
                QMutexLocker locker(&authorizationMutex);
                QVERIFY2(swapError.isEmpty(), qPrintable(swapError));
            }
            QCOMPARE(QFileInfo(link).symLinkTarget(), QFileInfo(rootB).canonicalFilePath());
            {
                QMutexLocker locker(&authorizationMutex);
                QCOMPARE(observedAuthorization.value(QStringLiteral("confirmationId")).toString(), staleConfirmationId);
                QCOMPARE(observedAuthorization.value(QStringLiteral("callId")).toString().isEmpty(), false);
                QVERIFY(!observedAuthorization.value(QStringLiteral("executionAuthorizationId")).toString().isEmpty());
                staleAuthorizationId =
                    observedAuthorization.value(QStringLiteral("executionAuthorizationId")).toString();
                QCOMPARE(observedAuthorization.value(QStringLiteral("resolvedTarget")).toString(),
                         QDir(targetA).canonicalPath());
            }
            const RunResult staleResult = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY(!staleResult.success);
            QCOMPARE(staleResult.errorCode, ExecError::ConfirmedTargetChanged);
            QCOMPARE(staleResult.failedModule, instanceId);
            QVERIFY(staleResult.diagnostics.contains(QStringLiteral("CONFIRMED_TARGET_CHANGED")));
            QCOMPARE(moduleStarted.count(), 1);
            QCOMPARE(moduleFinished.count(), 1);
            QVERIFY(!moduleFinished.last().at(1).toBool());
            QVERIFY(QFileInfo::exists(markerA));
            QVERIFY(QFileInfo::exists(markerB));
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, toolName);
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result,
                     entryPoint == QLatin1String("run") ? QStringLiteral("error") : QStringLiteral("success"));
            const QJsonObject executionToolResult = toolExecuted.last().at(1).toJsonObject();
            if (entryPoint == QLatin1String("step")) {
                QCOMPARE(executionToolResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
                QCOMPARE(executionToolResult.value(QStringLiteral("runId")).toString(), staleResult.runId);
            } else {
                QCOMPARE(executionToolResult.value(QStringLiteral("status")).toString(),
                         entryPoint == QLatin1String("run") ? QStringLiteral("failed") : QStringLiteral("resumed"));
            }
            const QJsonObject failedQuery = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                                      {{QStringLiteral("runId"), staleResult.runId}});
            QCOMPARE(failedQuery.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
            const QJsonObject lastError = failedQuery.value(QStringLiteral("lastError")).toObject();
            QCOMPARE(lastError.value(QStringLiteral("code")).toInt(), ExecError::ConfirmedTargetChanged);
            QCOMPARE(lastError.value(QStringLiteral("codeName")).toString(),
                     QStringLiteral("CONFIRMED_TARGET_CHANGED"));

            if (entryPoint == QLatin1String("resume")) {
                engine.clearModules();
                QVERIFY(engine.loadProject(project));
                QString sessionId;
                QVERIFY(engine.startCycle(&sessionId, 100, false, true, false, 1));
                QTRY_VERIFY_WITH_TIMEOUT(engine.isPausedAtBreakpoint(), 5000);
                moduleStarted.clear();
                moduleFinished.clear();
                runFinished.clear();
                toolExecuted.clear();
                audit.clear();
                toolName = QStringLiteral("resume_flow");
                toolParams = {};
            } else {
                moduleStarted.clear();
                moduleFinished.clear();
                runFinished.clear();
                toolExecuted.clear();
                audit.clear();
                toolName = QStringLiteral("run_flow");
                toolParams = {{QStringLiteral("mode"), QStringLiteral("once")}};
            }
            pending = controller.handleToolCall(toolName, toolParams);
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString freshConfirmationId = controller.pendingConfirmationId();
            QVERIFY(!freshConfirmationId.isEmpty());
            QVERIFY(freshConfirmationId != staleConfirmationId);
            const QString canonicalTargetB = QDir(targetB).canonicalPath();
            const QString expectedB = QStringLiteral("folder_delete:%1@%2 resolved_target=%3")
                                          .arg(instanceId, configuredPath, canonicalTargetB);
            const QJsonArray freshEffects = controller.pendingConfirmationPayload()
                                                .value(QStringLiteral("context"))
                                                .toObject()
                                                .value(QStringLiteral("sideEffects"))
                                                .toArray();
            QVERIFY(freshEffects.contains(expectedB));
            QCOMPARE(moduleStarted.count(), 0);
            QVERIFY(QFileInfo::exists(markerA));
            QVERIFY(QFileInfo::exists(markerB));
            QMutex freshAuthorizationMutex;
            QJsonObject freshAuthorization;
            const QMetaObject::Connection freshAuthorizationConnection = QObject::connect(
                &engine, &RunEngine::moduleStarted, &engine,
                [&](const QString& startedId) {
                    if (startedId != instanceId)
                        return;
                    ModuleBase* module = engine.getModule(instanceId);
                    if (!module)
                        return;
                    QMutexLocker locker(&freshAuthorizationMutex);
                    freshAuthorization = module->property(AgentFolderDeleteAuthorizationProperty).toJsonObject();
                },
                Qt::DirectConnection);
            QVERIFY(controller.confirmPendingTools(freshConfirmationId));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QObject::disconnect(freshAuthorizationConnection);
            const RunResult freshResult = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY2(freshResult.success, qPrintable(freshResult.errorMessage));
            {
                QMutexLocker locker(&freshAuthorizationMutex);
                QCOMPARE(freshAuthorization.value(QStringLiteral("confirmationId")).toString(), freshConfirmationId);
                QVERIFY(!freshAuthorization.value(QStringLiteral("executionAuthorizationId")).toString().isEmpty());
                QVERIFY(freshAuthorization.value(QStringLiteral("executionAuthorizationId")).toString() !=
                        staleAuthorizationId);
                QCOMPARE(freshAuthorization.value(QStringLiteral("resolvedTarget")).toString(), canonicalTargetB);
            }
            QVERIFY(QFileInfo::exists(markerA));
            QVERIFY(!QFileInfo::exists(markerB));
            QCOMPARE(moduleStarted.count(), 1);
            QCOMPARE(moduleFinished.count(), 1);
            QVERIFY(moduleFinished.last().at(1).toBool());

            engine.setBreakpoint(instanceId, false);
            moduleStarted.clear();
            moduleFinished.clear();
            runFinished.clear();
            QMutex ordinaryAuthorizationMutex;
            QJsonObject ordinaryExecutionAuthorization;
            const QMetaObject::Connection ordinaryRunConnection = QObject::connect(
                &engine, &RunEngine::moduleStarted, &engine,
                [&](const QString& startedId) {
                    if (startedId != instanceId)
                        return;
                    ModuleBase* module = engine.getModule(instanceId);
                    if (!module)
                        return;
                    QMutexLocker locker(&ordinaryAuthorizationMutex);
                    ordinaryExecutionAuthorization =
                        module->property(AgentFolderDeleteAuthorizationProperty).toJsonObject();
                },
                Qt::DirectConnection);
            QString ordinaryRunId;
            QVERIFY(engine.runOnceAsync(&ordinaryRunId));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QObject::disconnect(ordinaryRunConnection);
            QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
            {
                QMutexLocker locker(&ordinaryAuthorizationMutex);
                QVERIFY(ordinaryExecutionAuthorization.isEmpty());
            }

            engine.stop();
            engine.clearModules();
            projects.closeProject();
            QFile::remove(link);
            QFile::remove(replacementLink);
        }
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
#endif
    }

    void testFolderDeleteRejectsTargetCreatedAfterApproval() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        FolderTestStateRestore stateRestore{controller, engine, m_targets, m_appDataDir};
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString instanceId = QStringLiteral("slice124_missing_target_created_after_approval");
        const QString target = m_targets.filePath(QStringLiteral("slice124-target-created-after-approval"));
        const QString marker = QDir(target).filePath(QStringLiteral("marker.txt"));
        QVERIFY(!QDir(target).exists());
        Project* project = projects.newProject();
        QVERIFY(project);
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 124 missing target boundary");
        folder.params = {{QStringLiteral("operation"), QStringLiteral("Delete")},
                         {QStringLiteral("folderPath"), target}};
        project->addModule(folder);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QMutex authorizationMutex;
        QJsonObject observedAuthorization;
        std::atomic_bool targetCreated{false};
        const QMetaObject::Connection creationConnection = QObject::connect(
            &engine, &RunEngine::moduleStarted, &engine,
            [&](const QString& startedId) {
                if (startedId != instanceId)
                    return;
                ModuleBase* module = engine.getModule(instanceId);
                if (!module)
                    return;
                {
                    QMutexLocker locker(&authorizationMutex);
                    observedAuthorization = module->property(AgentFolderDeleteAuthorizationProperty).toJsonObject();
                }
                if (!QDir().mkpath(target))
                    return;
                QFile file(marker);
                targetCreated.store(file.open(QIODevice::WriteOnly) && file.write("sentinel") == 8 && file.flush());
            },
            Qt::DirectConnection);

        QJsonObject pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        QVERIFY(!QDir(target).exists());
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QObject::disconnect(creationConnection);
        QVERIFY(targetCreated.load());
        QCOMPARE(moduleStarted.count(), 1);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!result.success);
        QCOMPARE(result.errorCode, ExecError::ConfirmedTargetChanged);
        QVERIFY(QFileInfo::exists(marker));
        {
            QMutexLocker locker(&authorizationMutex);
            QCOMPARE(observedAuthorization.value(QStringLiteral("confirmationId")).toString(), confirmationId);
            QVERIFY(!observedAuthorization.value(QStringLiteral("targetExistsAtApproval")).toBool());
            QVERIFY(observedAuthorization.value(QStringLiteral("resolvedTarget")).toString().isEmpty());
        }
        const QJsonObject query =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), result.runId}});
        QCOMPARE(query.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("codeName")).toString(),
                 QStringLiteral("CONFIRMED_TARGET_CHANGED"));
    }

    void testFolderDeleteBreakpointResumeRequiresFreshConfirmation() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        FolderTestStateRestore stateRestore{controller, engine, m_targets, m_appDataDir};
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString target = m_targets.filePath(QStringLiteral("slice124-breakpoint-resume-target"));
        const QString marker = QDir(target).filePath(QStringLiteral("marker.txt"));
        QVERIFY(QDir().mkpath(target));
        QFile sentinel(marker);
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write("sentinel"), qint64(8));
        QVERIFY(sentinel.flush());
        sentinel.close();

        Project* project = projects.newProject();
        QVERIFY(project);
        const QString probeId = QStringLiteral("slice124_breakpoint_previously_completed_probe");
        const QString instanceId = QStringLiteral("slice124_breakpoint_folder_delete");
        ModuleInstance probe;
        probe.id = probeId;
        probe.moduleId = QStringLiteral("Folder");
        probe.name = QStringLiteral("Slice 124 pre-breakpoint probe");
        probe.params = {{QStringLiteral("operation"), QStringLiteral("Exists")},
                        {QStringLiteral("folderPath"), target}};
        project->addModule(probe);
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 124 breakpoint Folder delete");
        folder.breakpoint = true;
        folder.params = {{QStringLiteral("operation"), QStringLiteral("Delete")},
                         {QStringLiteral("folderPath"), target}};
        project->addModule(folder);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QJsonObject pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString initialConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!initialConfirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(initialConfirmationId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.isPausedAtBreakpoint(), 5000);
        QCOMPARE(countModuleSignal(moduleStarted, probeId), 1);
        QVERIFY(QFileInfo::exists(marker));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        const RunResumeSnapshot pausedSnapshot = engine.resumeSnapshot();
        QVERIFY(!pausedSnapshot.runId.isEmpty());
        QVERIFY(!engine.resumeExpected(pausedSnapshot));
        QVERIFY(engine.isPausedAtBreakpoint());
        QCOMPARE(engine.resumeSnapshot().runId, pausedSnapshot.runId);
        QVERIFY(QFileInfo::exists(marker));
        QCOMPARE(countModuleSignal(moduleStarted, probeId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString freshConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!freshConfirmationId.isEmpty());
        QVERIFY(freshConfirmationId != initialConfirmationId);
        QMutex authorizationMutex;
        QJsonObject resumedAuthorization;
        const QMetaObject::Connection authorizationConnection = QObject::connect(
            &engine, &RunEngine::moduleStarted, &engine,
            [&](const QString& startedId) {
                if (startedId != instanceId)
                    return;
                if (ModuleBase* module = engine.getModule(instanceId)) {
                    QMutexLocker locker(&authorizationMutex);
                    resumedAuthorization = module->property(AgentFolderDeleteAuthorizationProperty).toJsonObject();
                }
            },
            Qt::DirectConnection);
        QVERIFY(controller.confirmPendingTools(freshConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QObject::disconnect(authorizationConnection);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(result.runId, pausedSnapshot.runId);
        QVERIFY(!QFileInfo::exists(marker));
        QCOMPARE(countModuleSignal(moduleStarted, probeId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        {
            QMutexLocker locker(&authorizationMutex);
            QCOMPARE(resumedAuthorization.value(QStringLiteral("confirmationId")).toString(), freshConfirmationId);
            QVERIFY(!resumedAuthorization.value(QStringLiteral("executionAuthorizationId")).toString().isEmpty());
        }
    }

    void testFolderCreateConfirmationExpiresWhenSymlinkParentChangesTarget() {
#ifndef Q_OS_UNIX
        QSKIP("The test requires POSIX directory symlink and rename semantics");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        FolderTestStateRestore stateRestore{controller, engine, m_targets, m_appDataDir};
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString rootA = m_targets.filePath(QStringLiteral("slice115-root-a"));
        const QString rootB = m_targets.filePath(QStringLiteral("slice115-root-b"));
        const QString link = m_targets.filePath(QStringLiteral("slice115-current-root"));
        const QString replacementLink = m_targets.filePath(QStringLiteral("slice115-next-root"));
        QVERIFY(QDir().mkpath(rootA));
        QVERIFY(QDir().mkpath(rootB));
        if (!QFile::link(rootA, link) || !QFileInfo(link).isSymLink())
            QSKIP("The current filesystem does not provide directory symlink support");

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice115_folder_create_symlink_target");
        const QString relativeTarget = QStringLiteral("created/nested");
        const QString configuredPath = QDir(link).filePath(relativeTarget);
        const QString targetA = QDir(rootA).filePath(relativeTarget);
        const QString targetB = QDir(rootB).filePath(relativeTarget);
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 115 symlink Folder create");
        folder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Create")},
                                    {QStringLiteral("folderPath"), configuredPath}};
        project->addModule(folder);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        QJsonObject pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        const QString oldEffect =
            QStringLiteral("folder_create:%1@%2 resolved_target=%3").arg(instanceId, configuredPath, targetA);
        const QJsonArray oldEffects = controller.pendingConfirmationPayload()
                                          .value(QStringLiteral("context"))
                                          .toObject()
                                          .value(QStringLiteral("sideEffects"))
                                          .toArray();
        QVERIFY(oldEffects.contains(oldEffect));
        QVERIFY(!QFileInfo::exists(targetA));
        QVERIFY(!QFileInfo::exists(targetB));

        if (!QFile::link(rootB, replacementLink) || !QFileInfo(replacementLink).isSymLink()) {
            QVERIFY(controller.rejectPendingTools(staleId));
            QSKIP("The current filesystem does not support a second directory symlink");
        }
        const QByteArray replacementName = QFile::encodeName(replacementLink);
        const QByteArray currentName = QFile::encodeName(link);
        if (std::rename(replacementName.constData(), currentName.constData()) != 0)
            QFAIL("Same-directory POSIX rename unexpectedly failed while replacing the symlink");
        QVERIFY(QFileInfo(link).isSymLink());
        QCOMPARE(QFileInfo(link).symLinkTarget(), QFileInfo(rootB).canonicalFilePath());

        const int auditCountBeforeExpiry = audit.count();
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeExpiry, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("run_flow"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QVERIFY(!engine.isBusy());
        QVERIFY(!QFileInfo::exists(targetA));
        QVERIFY(!QFileInfo::exists(targetB));

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString rejectId = controller.pendingConfirmationId();
        QVERIFY(!rejectId.isEmpty());
        QVERIFY(rejectId != staleId);
        const QString newEffect =
            QStringLiteral("folder_create:%1@%2 resolved_target=%3").arg(instanceId, configuredPath, targetB);
        const QJsonArray newEffects = controller.pendingConfirmationPayload()
                                          .value(QStringLiteral("context"))
                                          .toObject()
                                          .value(QStringLiteral("sideEffects"))
                                          .toArray();
        QVERIFY(newEffects.contains(newEffect));
        QVERIFY(controller.rejectPendingTools(rejectId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QVERIFY(!QFileInfo::exists(targetA));
        QVERIFY(!QFileInfo::exists(targetB));

        pending = controller.handleToolCall(
            QStringLiteral("run_flow"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString freshId = controller.pendingConfirmationId();
        QVERIFY(!freshId.isEmpty());
        QVERIFY(freshId != rejectId);
        const QJsonArray freshEffects = controller.pendingConfirmationPayload()
                                            .value(QStringLiteral("context"))
                                            .toObject()
                                            .value(QStringLiteral("sideEffects"))
                                            .toArray();
        QVERIFY(freshEffects.contains(newEffect));
        QVERIFY(controller.confirmPendingTools(freshId));
        QTRY_COMPARE_WITH_TIMEOUT(moduleStarted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(moduleFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(moduleStarted.first().at(0).toString(), instanceId);
        QCOMPARE(moduleFinished.first().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.first().at(1).toBool());
        const RunResult result = qvariant_cast<RunResult>(runFinished.first().at(0));
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(toolExecuted.count(), 1);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!QFileInfo::exists(targetA));
        QVERIFY(QFileInfo(targetB).isDir());
        QVERIFY(!engine.isBusy());

        const QJsonObject operationOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), result.runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("operation_result")}});
        QCOMPARE(operationOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(operationOutput.value(QStringLiteral("value")).toBool());
        const QJsonObject pathOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), result.runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("folder_path")}});
        QCOMPARE(pathOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(pathOutput.value(QStringLiteral("value")).toString(), configuredPath);
        QVERIFY(QFile::remove(link));
#endif
    }

    void testFolderDeletePartialFailureBoundedCycleStopsWithoutReplay() {
#ifndef Q_OS_UNIX
        QSKIP("The partial-delete permission fixture currently requires POSIX directory permissions");
#else
        const QString target = m_targets.filePath(QStringLiteral("slice112-partial-delete-cycle"));
        const QString writableDir = QDir(target).filePath(QStringLiteral("writable"));
        const QString protectedDir = QDir(target).filePath(QStringLiteral("protected"));
        QVERIFY(QDir().mkpath(writableDir));
        QVERIFY(QDir().mkpath(protectedDir));
        const auto writeMarker = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write("slice112\n");
            return file.flush();
        };
        const QString writableMarker = QDir(writableDir).filePath(QStringLiteral("marker.txt"));
        const QString protectedMarker = QDir(protectedDir).filePath(QStringLiteral("marker.txt"));
        const QString barrierProbe = QDir(protectedDir).filePath(QStringLiteral("barrier-probe.txt"));
        QVERIFY(writeMarker(writableMarker));
        QVERIFY(writeMarker(protectedMarker));
        QVERIFY(writeMarker(barrierProbe));

        FilePermissionRestore permissionRestore{protectedDir, QFileInfo(protectedDir).permissions()};
        if (!QFile::setPermissions(protectedDir, QFile::ReadOwner | QFile::ExeOwner))
            QSKIP("Could not remove owner-write permission from the protected test directory");
        if (QFile::remove(barrierProbe))
            QSKIP("This process or filesystem can delete inside a read/execute-only directory");
        QVERIFY(QFileInfo::exists(barrierProbe));

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        FolderTestStateRestore stateRestore{controller, engine, m_targets, m_appDataDir};
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice112_folder_delete_partial_cycle");
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 112 bounded-cycle partial Folder delete");
        folder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Delete")},
                                    {QStringLiteral("folderPath"), target}};
        project->addModule(folder);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        int runFlowStarts = 0;
        QString sessionId;
        QObject signalContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &signalContext,
                [&runFlowStarts, &sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow") &&
                        result.value(QStringLiteral("status")).toString() == QLatin1String("started")) {
                        ++runFlowStarts;
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                    }
                });
        const auto requestCycle = [&controller]() {
            return controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        };

        QJsonObject pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(QFileInfo::exists(writableMarker));
        QVERIFY(QFileInfo::exists(protectedMarker));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(QFileInfo::exists(writableMarker));
        QVERIFY(QFileInfo::exists(protectedMarker));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(runFlowStarts, 0);

        pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 5000);
        QVERIFY(!sessionId.isEmpty());
        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!failed.success);
        QVERIFY(!failed.runId.isEmpty());
        QCOMPARE(failed.failedModule, instanceId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QVERIFY(failed.diagnostics.contains(QStringLiteral("operation=Delete result=unconfirmed")));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(runFlowStarts, 1);
        QVERIFY(!QFileInfo::exists(writableDir));
        QVERIFY(QFileInfo::exists(protectedMarker));
        QVERIFY(!engine.isBusy());

        const QJsonObject session =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(session.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!session.value(QStringLiteral("retryable")).toBool());
        QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), 3);
        QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), 0);
        QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(), failed.runId);
        QCOMPARE(session.value(QStringLiteral("lastIterationRunId")).toString(), failed.runId);
        QCOMPARE(session.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("code")).toInt(),
                 ExecError::SideEffectUncertain);
        const QJsonObject output = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), failed.runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("operation_result")}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!output.contains(QStringLiteral("value")));

        QVERIFY(permissionRestore.restore());
        QElapsedTimer quietWindow;
        quietWindow.start();
        while (quietWindow.elapsed() < 550) {
            QCOMPARE(
                controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), sessionId}})
                    .value(QStringLiteral("status"))
                    .toString(),
                QStringLiteral("uncertain"));
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("get_module_output"),
                                         {{QStringLiteral("runId"), failed.runId},
                                          {QStringLiteral("instanceId"), instanceId},
                                          {QStringLiteral("key"), QStringLiteral("operation_result")}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("uncertain"));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        QVERIFY(QFileInfo::exists(target));
        QVERIFY(QFileInfo::exists(protectedMarker));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(runFlowStarts, 1);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testFolderDeletePartialFailureIsUncertainAndNeverReplayed() {
#ifndef Q_OS_UNIX
        QSKIP("The partial-delete permission fixture currently requires POSIX directory permissions");
#else
        const QString target = m_targets.filePath(QStringLiteral("slice87-partial-delete"));
        const QString writableDir = QDir(target).filePath(QStringLiteral("writable"));
        const QString protectedDir = QDir(target).filePath(QStringLiteral("protected"));
        QVERIFY(QDir().mkpath(writableDir));
        QVERIFY(QDir().mkpath(protectedDir));
        const auto writeMarker = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write("slice87\n");
            return file.flush();
        };
        const QString writableMarker = QDir(writableDir).filePath(QStringLiteral("marker.txt"));
        const QString protectedMarker = QDir(protectedDir).filePath(QStringLiteral("marker.txt"));
        const QString barrierProbe = QDir(protectedDir).filePath(QStringLiteral("barrier-probe.txt"));
        QVERIFY(writeMarker(writableMarker));
        QVERIFY(writeMarker(protectedMarker));
        QVERIFY(writeMarker(barrierProbe));

        FilePermissionRestore permissionRestore{protectedDir, QFileInfo(protectedDir).permissions()};
        if (!QFile::setPermissions(protectedDir, QFile::ReadOwner | QFile::ExeOwner))
            QSKIP("Could not remove owner-write permission from the protected test directory");
        if (QFile::remove(barrierProbe))
            QSKIP("This process or filesystem can delete inside a read/execute-only directory");
        QVERIFY(QFileInfo::exists(barrierProbe));

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice87_folder_delete_partial");
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 87 partial Folder delete");
        folder.params = QJsonObject{{QStringLiteral("operation"), QStringLiteral("Delete")},
                                    {QStringLiteral("folderPath"), target}};
        project->addModule(folder);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        QJsonObject pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(QFileInfo::exists(writableMarker));
        QVERIFY(QFileInfo::exists(protectedMarker));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(QFileInfo::exists(writableMarker));
        QVERIFY(QFileInfo::exists(protectedMarker));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!failed.runId.isEmpty());
        QVERIFY(!failed.success);
        QCOMPARE(failed.failedModule, instanceId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QVERIFY(failed.diagnostics.contains(QStringLiteral("operation=Delete result=unconfirmed")));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QVERIFY(!QFileInfo::exists(writableDir));
        QVERIFY(QFileInfo::exists(protectedMarker));

        const QJsonObject uncertain =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), failed.runId}});
        QCOMPARE(uncertain.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(uncertain.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!uncertain.value(QStringLiteral("retryable")).toBool());
        QVERIFY(uncertain.value(QStringLiteral("lastError"))
                    .toObject()
                    .value(QStringLiteral("diagnostics"))
                    .toString()
                    .contains(QStringLiteral("operation=Delete result=unconfirmed")));
        const QJsonObject output = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), failed.runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("operation_result")}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!output.value(QStringLiteral("retryable")).toBool());

        QVERIFY(permissionRestore.restore());
        const int moduleStartsAfterFailure = countModuleSignal(moduleStarted, instanceId);
        const int runFinishesAfterFailure = runFinished.count();
        QElapsedTimer quietWindow;
        quietWindow.start();
        while (quietWindow.elapsed() < 500) {
            QCOMPARE(
                controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), failed.runId}})
                    .value(QStringLiteral("status"))
                    .toString(),
                QStringLiteral("uncertain"));
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("get_module_output"),
                                         {{QStringLiteral("runId"), failed.runId},
                                          {QStringLiteral("instanceId"), instanceId},
                                          {QStringLiteral("key"), QStringLiteral("operation_result")}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("uncertain"));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        QVERIFY(QFileInfo::exists(target));
        QVERIFY(QFileInfo::exists(protectedMarker));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), moduleStartsAfterFailure);
        QCOMPARE(runFinished.count(), runFinishesAfterFailure);

        const QString missingTarget = m_targets.filePath(QStringLiteral("slice87-does-not-exist"));
        QVERIFY(!QFileInfo::exists(missingTarget));
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("folderPath"), missingTarget));
        pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult missingResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!missingResult.runId.isEmpty());
        QVERIFY(missingResult.success);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 2);
        const QJsonObject missingOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), missingResult.runId},
                                                  {QStringLiteral("instanceId"), instanceId},
                                                  {QStringLiteral("key"), QStringLiteral("operation_result")}});
        QCOMPARE(missingOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(missingOutput.value(QStringLiteral("value")).toBool(), false);
        QVERIFY(!QFileInfo::exists(missingTarget));

        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testSaveDataJsonWriteFailureIsUncertainAndNeverReplayed() {
#ifndef Q_OS_LINUX
        QSKIP("The JSON write-failure fixture currently requires Linux /dev/full");
#else
        struct stat fullDeviceStatus {};
        if (::stat("/dev/full", &fullDeviceStatus) != 0 || !S_ISCHR(fullDeviceStatus.st_mode))
            QSKIP("/dev/full is not available as a character device");

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice88_savedata_json_sink");
        ModuleInstance saveData;
        saveData.id = instanceId;
        saveData.moduleId = QStringLiteral("SaveData");
        saveData.name = QStringLiteral("Slice 88 SaveData JSON failure");
        saveData.params = QJsonObject{{QStringLiteral("filePath"), QStringLiteral("/dev/full")},
                                      {QStringLiteral("fileFormat"), QStringLiteral("json")},
                                      {QStringLiteral("appendMode"), false}};
        project->addModule(saveData);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        const QJsonArray expectedEffect{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                                        QStringLiteral("file_write:/dev/full format=json mode=overwrite")};
        QJsonObject pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedEffect);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult uncertainRun = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!uncertainRun.runId.isEmpty());
        QVERIFY(!uncertainRun.success);
        QCOMPARE(uncertainRun.failedModule, instanceId);
        QCOMPARE(uncertainRun.errorCode, ExecError::SideEffectUncertain);
        QVERIFY(uncertainRun.diagnostics.contains(QStringLiteral("fileFormat=json")));
        QVERIFY(uncertainRun.diagnostics.contains(QStringLiteral("flushOk=false")));
        const QRegularExpression bytesPattern(QStringLiteral("bytesWritten=(-?\\d+) expectedBytes=(\\d+)"));
        const QRegularExpressionMatch bytesMatch = bytesPattern.match(uncertainRun.diagnostics);
        QVERIFY(bytesMatch.hasMatch());
        QVERIFY(bytesMatch.captured(2).toLongLong() > 0);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QVERIFY(!moduleFinished.last().at(1).toBool());

        QJsonObject runStatus = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                          {{QStringLiteral("runId"), uncertainRun.runId}});
        QCOMPARE(runStatus.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(runStatus.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!runStatus.value(QStringLiteral("retryable")).toBool());
        QVERIFY(runStatus.value(QStringLiteral("lastError"))
                    .toObject()
                    .value(QStringLiteral("diagnostics"))
                    .toString()
                    .contains(QStringLiteral("fileFormat=json")));
        QJsonObject output = controller.handleToolCall(QStringLiteral("get_module_output"),
                                                       {{QStringLiteral("runId"), uncertainRun.runId},
                                                        {QStringLiteral("instanceId"), instanceId},
                                                        {QStringLiteral("key"), QStringLiteral("save_result")}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!output.value(QStringLiteral("retryable")).toBool());

        const int modulesAfterWriteFailure = countModuleSignal(moduleStarted, instanceId);
        const int runsAfterWriteFailure = runFinished.count();
        QElapsedTimer quietWindow;
        quietWindow.start();
        while (quietWindow.elapsed() < 500) {
            QCOMPARE(
                controller
                    .handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), uncertainRun.runId}})
                    .value(QStringLiteral("status"))
                    .toString(),
                QStringLiteral("uncertain"));
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("get_module_output"),
                                         {{QStringLiteral("runId"), uncertainRun.runId},
                                          {QStringLiteral("instanceId"), instanceId},
                                          {QStringLiteral("key"), QStringLiteral("save_result")}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("uncertain"));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), modulesAfterWriteFailure);
        QCOMPARE(runFinished.count(), runsAfterWriteFailure);

        const QString outputDirectory = m_targets.filePath(QStringLiteral("slice88-json-open-failure"));
        QVERIFY(QDir().mkpath(outputDirectory));
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("filePath"), outputDirectory));
        pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult openFailure = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!openFailure.success);
        QCOMPARE(openFailure.failedModule, instanceId);
        QCOMPARE(openFailure.errorCode, ExecError::Processing);
        runStatus = controller.handleToolCall(QStringLiteral("get_run_results"),
                                              {{QStringLiteral("runId"), openFailure.runId}});
        QCOMPARE(runStatus.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(!runStatus.contains(QStringLiteral("sideEffectStatus")));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 2);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 2);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testSaveImageWriteFailureIsUncertainAndNeverReplayed() {
#if !defined(Q_OS_LINUX) || !defined(DEEPLUX_HAS_OPENCV)
        QSKIP("The SaveImage write-failure fixture requires Linux and the OpenCV encoder path");
#else
        struct stat fullDeviceStatus {};
        if (::stat("/dev/full", &fullDeviceStatus) != 0 || !S_ISCHR(fullDeviceStatus.st_mode))
            QSKIP("/dev/full is not available as a character device");

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString imagePath = m_targets.filePath(QStringLiteral("slice103-source.png"));
        QImage sourceImage(8, 6, QImage::Format_RGB32);
        sourceImage.fill(qRgb(22, 74, 128));
        QVERIFY(sourceImage.save(imagePath, "PNG"));
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString sourceId = QStringLiteral("slice103_grab_source");
        const QString saveId = QStringLiteral("slice103_save_image");
        ModuleInstance source;
        source.id = sourceId;
        source.moduleId = QStringLiteral("GrabImage");
        source.name = QStringLiteral("Slice 103 image input");
        source.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                    {QStringLiteral("filePath"), imagePath}};
        project->addModule(source);
        ModuleInstance save;
        save.id = saveId;
        save.moduleId = QStringLiteral("SaveImage");
        save.name = QStringLiteral("Slice 103 SaveImage target");
        save.params = QJsonObject{{QStringLiteral("filePath"), QStringLiteral("/dev/full")},
                                  {QStringLiteral("format"), QStringLiteral("png")},
                                  {QStringLiteral("quality"), 95}};
        project->addModule(save);
        ModuleConnection edge;
        edge.fromModuleId = sourceId;
        edge.toModuleId = saveId;
        edge.fromPort = QStringLiteral("image");
        edge.toPort = QStringLiteral("image");
        edge.edgeType = QStringLiteral("data");
        project->addConnection(edge);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QString sessionId;
        int runFlowStarts = 0;
        QObject signalContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &signalContext,
                [&sessionId, &runFlowStarts](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow") &&
                        result.value(QStringLiteral("status")).toString() == QLatin1String("started")) {
                        ++runFlowStarts;
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                    }
                });
        auto requestCycle = [&controller]() {
            return controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        };

        QJsonObject pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(QStringLiteral("file_write:/dev/full")));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 0);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(runFlowStarts, 0);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, sourceId), 0);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);

        pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 5000);
        QVERIFY(!sessionId.isEmpty());
        const RunResult uncertainRun = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!uncertainRun.success);
        QCOMPARE(uncertainRun.failedModule, saveId);
        QCOMPARE(uncertainRun.errorCode, ExecError::SideEffectUncertain);
        QVERIFY(uncertainRun.diagnostics.contains(QStringLiteral("format=png")));
        QVERIFY(uncertainRun.diagnostics.contains(QStringLiteral("writeAttempted=true")));
        QVERIFY(uncertainRun.diagnostics.contains(QStringLiteral("expectedBytes=")));
        QVERIFY(uncertainRun.diagnostics.contains(QStringLiteral("bytesWritten=")));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, sourceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, saveId), 1);
        QVERIFY(moduleFinished.first().at(1).toBool());
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(runFlowStarts, 1);

        const QJsonObject session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                              QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(session.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!session.value(QStringLiteral("retryable")).toBool());
        QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), 3);
        QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), 0);
        QVERIFY(!session.contains(QStringLiteral("lastSuccessfulRunId")));
        QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(), uncertainRun.runId);
        QCOMPARE(session.value(QStringLiteral("lastIterationRunId")).toString(), uncertainRun.runId);
        QCOMPARE(session.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("code")).toInt(),
                 ExecError::SideEffectUncertain);
        const QJsonObject output = controller.handleToolCall(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), uncertainRun.runId}, {QStringLiteral("instanceId"), saveId}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(output.value(QStringLiteral("reason")).toString(), QStringLiteral("side_effect_outcome_unknown"));
        QCOMPARE(output.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!output.contains(QStringLiteral("value")));

        const int moduleStartsAfterFailure =
            countModuleSignal(moduleStarted, sourceId) + countModuleSignal(moduleStarted, saveId);
        const int runFlowStartsAfterFailure = runFlowStarts;
        QElapsedTimer quietWindow;
        quietWindow.start();
        while (quietWindow.elapsed() < 550) {
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("get_run_results"),
                                         QJsonObject{{QStringLiteral("runId"), sessionId}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("uncertain"));
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("get_module_output"),
                                         QJsonObject{{QStringLiteral("runId"), uncertainRun.runId},
                                                     {QStringLiteral("instanceId"), saveId}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("uncertain"));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        QCOMPARE(countModuleSignal(moduleStarted, sourceId) + countModuleSignal(moduleStarted, saveId),
                 moduleStartsAfterFailure);
        QCOMPARE(runFlowStarts, runFlowStartsAfterFailure);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(cycleStopped.count(), 1);

        const QString directoryTarget = m_targets.filePath(QStringLiteral("slice103-open-failure"));
        QVERIFY(QDir().mkpath(directoryTarget));
        QVERIFY(project->setModuleParam(saveId, QStringLiteral("filePath"), directoryTarget));
        const QJsonObject openPending = controller.handleToolCall(
            QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(openPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult openFailure = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!openFailure.success);
        QCOMPARE(openFailure.failedModule, saveId);
        QCOMPARE(openFailure.errorCode, ExecError::Processing);
        const QJsonObject openStatus = controller.handleToolCall(
            QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), openFailure.runId}});
        QCOMPARE(openStatus.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(!openStatus.contains(QStringLiteral("sideEffectStatus")));
        QCOMPARE(countModuleSignal(moduleStarted, sourceId), 2);
        QCOMPARE(countModuleSignal(moduleStarted, saveId), 2);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testSaveDataCsvAndTextWriteFailuresAreUncertainAndNeverReplayed() {
#ifndef Q_OS_LINUX
        QSKIP("The SaveData write-failure fixture currently requires Linux /dev/full");
#else
        struct stat fullDeviceStatus {};
        if (::stat("/dev/full", &fullDeviceStatus) != 0 || !S_ISCHR(fullDeviceStatus.st_mode))
            QSKIP("/dev/full is not available as a character device");

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QString imagePath = m_targets.filePath(QStringLiteral("slice102-input.png"));
        QImage sourceImage(8, 6, QImage::Format_RGB32);
        sourceImage.fill(qRgb(35, 90, 145));
        QVERIFY(sourceImage.save(imagePath, "PNG"));

        for (const QString& format : {QStringLiteral("csv"), QStringLiteral("text")}) {
            ProjectManager::instance().closeProject();
            engine.stop();
            engine.clearModules();
            controller.clearConversation();
            Project* project = ProjectManager::instance().newProject();
            QVERIFY(project);
            const QString sourceId = QStringLiteral("slice102_grab_%1").arg(format);
            const QString saveId = QStringLiteral("slice102_save_%1").arg(format);
            ModuleInstance source;
            source.id = sourceId;
            source.moduleId = QStringLiteral("GrabImage");
            source.name = QStringLiteral("Slice 102 non-empty SaveData source");
            source.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                        {QStringLiteral("filePath"), imagePath}};
            project->addModule(source);
            ModuleInstance save;
            save.id = saveId;
            save.moduleId = QStringLiteral("SaveData");
            save.name = QStringLiteral("Slice 102 SaveData %1 failure").arg(format);
            save.params = QJsonObject{{QStringLiteral("filePath"), QStringLiteral("/dev/full")},
                                      {QStringLiteral("fileFormat"), format},
                                      {QStringLiteral("appendMode"), false}};
            project->addModule(save);
            ModuleConnection edge;
            edge.fromModuleId = sourceId;
            edge.toModuleId = saveId;
            edge.fromPort = QStringLiteral("image");
            edge.toPort = QStringLiteral("image");
            edge.edgeType = QStringLiteral("data");
            project->addConnection(edge);

            QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
            QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
            QString sessionId;
            int runFlowStarts = 0;
            QObject signalContext;
            connect(controller.actor(), &AgentActor::toolExecuted, &signalContext,
                    [&sessionId, &runFlowStarts](const QString& toolName, const QJsonObject& result) {
                        if (toolName == QLatin1String("run_flow") &&
                            result.value(QStringLiteral("status")).toString() == QLatin1String("started")) {
                            ++runFlowStarts;
                            sessionId = result.value(QStringLiteral("sessionId")).toString();
                        }
                    });
            auto requestCycle = [&controller]() {
                return controller.handleToolCall(
                    QStringLiteral("run_flow"),
                    QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
            };

            QJsonObject pending = requestCycle();
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            QVERIFY(controller.pendingConfirmationPayload()
                        .value(QStringLiteral("context"))
                        .toObject()
                        .value(QStringLiteral("sideEffects"))
                        .toArray()
                        .contains(QStringLiteral("file_write:/dev/full format=%1 mode=overwrite").arg(format)));
            QCOMPARE(countModuleSignal(moduleStarted, sourceId), 0);
            QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
            QCOMPARE(runFinished.count(), 0);
            QCOMPARE(cycleStopped.count(), 0);
            QCOMPARE(runFlowStarts, 0);
            QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
            QCOMPARE(countModuleSignal(moduleStarted, sourceId), 0);
            QCOMPARE(countModuleSignal(moduleFinished, sourceId), 0);
            QCOMPARE(countModuleSignal(moduleStarted, saveId), 0);
            QCOMPARE(runFinished.count(), 0);
            QCOMPARE(cycleStopped.count(), 0);
            QCOMPARE(runFlowStarts, 0);

            pending = requestCycle();
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 5000);
            QVERIFY(!sessionId.isEmpty());
            const RunResult uncertainRun = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY(!uncertainRun.success);
            QCOMPARE(uncertainRun.failedModule, saveId);
            QCOMPARE(uncertainRun.errorCode, ExecError::SideEffectUncertain);
            QVERIFY(uncertainRun.diagnostics.contains(QStringLiteral("fileFormat=%1").arg(format)));
            QVERIFY(uncertainRun.diagnostics.contains(QStringLiteral("writeAttempted=true")));
            QVERIFY(uncertainRun.diagnostics.contains(QStringLiteral("dataKeys=source_file_name,source_path")));
            QCOMPARE(countModuleSignal(moduleStarted, sourceId), 1);
            QCOMPARE(countModuleSignal(moduleStarted, saveId), 1);
            QCOMPARE(countModuleSignal(moduleFinished, sourceId), 1);
            QCOMPARE(countModuleSignal(moduleFinished, saveId), 1);
            QVERIFY(moduleFinished.last().at(1).toBool() == false);
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QCOMPARE(cycleStopped.count(), 1);
            QCOMPARE(runFlowStarts, 1);

            const QJsonObject session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                                  QJsonObject{{QStringLiteral("runId"), sessionId}});
            QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QCOMPARE(session.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!session.value(QStringLiteral("retryable")).toBool());
            QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), 3);
            QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), 0);
            QVERIFY(!session.contains(QStringLiteral("lastSuccessfulRunId")));
            QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(), uncertainRun.runId);
            QCOMPARE(session.value(QStringLiteral("lastIterationRunId")).toString(), uncertainRun.runId);
            QCOMPARE(session.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("code")).toInt(),
                     ExecError::SideEffectUncertain);
            const QJsonObject output =
                controller.handleToolCall(QStringLiteral("get_module_output"),
                                          QJsonObject{{QStringLiteral("runId"), uncertainRun.runId},
                                                      {QStringLiteral("instanceId"), saveId},
                                                      {QStringLiteral("key"), QStringLiteral("save_result")}});
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QCOMPARE(output.value(QStringLiteral("reason")).toString(), QStringLiteral("side_effect_outcome_unknown"));
            QCOMPARE(output.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!output.contains(QStringLiteral("value")));

            const int startsAfterFailure = runFlowStarts;
            const int sourceStartsAfterFailure = countModuleSignal(moduleStarted, sourceId);
            const int saveStartsAfterFailure = countModuleSignal(moduleStarted, saveId);
            QElapsedTimer quietWindow;
            quietWindow.start();
            while (quietWindow.elapsed() < 550) {
                QCOMPARE(controller
                             .handleToolCall(QStringLiteral("get_run_results"),
                                             QJsonObject{{QStringLiteral("runId"), sessionId}})
                             .value(QStringLiteral("status"))
                             .toString(),
                         QStringLiteral("uncertain"));
                QCOMPARE(controller
                             .handleToolCall(QStringLiteral("get_module_output"),
                                             QJsonObject{{QStringLiteral("runId"), uncertainRun.runId},
                                                         {QStringLiteral("instanceId"), saveId},
                                                         {QStringLiteral("key"), QStringLiteral("save_result")}})
                             .value(QStringLiteral("status"))
                             .toString(),
                         QStringLiteral("uncertain"));
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QTest::qWait(10);
            }
            QCOMPARE(runFlowStarts, startsAfterFailure);
            QCOMPARE(countModuleSignal(moduleStarted, sourceId), sourceStartsAfterFailure);
            QCOMPARE(countModuleSignal(moduleStarted, saveId), saveStartsAfterFailure);
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QCOMPARE(cycleStopped.count(), 1);

            const QString directoryTarget = m_targets.filePath(QStringLiteral("slice102-open-failure-%1").arg(format));
            QVERIFY(QDir().mkpath(directoryTarget));
            QVERIFY(project->setModuleParam(saveId, QStringLiteral("filePath"), directoryTarget));
            const QJsonObject openPending = controller.handleToolCall(
                QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
            QCOMPARE(openPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
            const RunResult openFailure = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY(!openFailure.success);
            QCOMPARE(openFailure.failedModule, saveId);
            QCOMPARE(openFailure.errorCode, ExecError::Processing);
            const QJsonObject openStatus = controller.handleToolCall(
                QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), openFailure.runId}});
            QCOMPARE(openStatus.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
            QVERIFY(!openStatus.contains(QStringLiteral("sideEffectStatus")));
            QCOMPARE(countModuleSignal(moduleStarted, sourceId), 2);
            QCOMPARE(countModuleSignal(moduleStarted, saveId), 2);

            engine.clearModules();
            ProjectManager::instance().closeProject();
            controller.clearConversation();
        }
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
#endif
    }

    void testWriteTextFailureAfterOpenIsUncertainAndNeverReplayed() {
#ifndef Q_OS_LINUX
        QSKIP("The WriteText write-failure fixture currently requires Linux /dev/full");
#else
        struct stat fullDeviceStatus {};
        if (::stat("/dev/full", &fullDeviceStatus) != 0 || !S_ISCHR(fullDeviceStatus.st_mode))
            QSKIP("/dev/full is not available as a character device");

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice101_writetext_full_device");
        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 101 WriteText uncertain failure");
        writer.params = QJsonObject{{QStringLiteral("filePath"), QStringLiteral("/dev/full")},
                                    {QStringLiteral("textContent"), QStringLiteral("non-empty write payload")},
                                    {QStringLiteral("appendMode"), false}};
        project->addModule(writer);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QString sessionId;
        int runFlowStarts = 0;
        QObject signalContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &signalContext,
                [&sessionId, &runFlowStarts](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow") &&
                        result.value(QStringLiteral("status")).toString() == QLatin1String("started")) {
                        ++runFlowStarts;
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                    }
                });
        auto requestCycle = [&controller]() {
            return controller.handleToolCall(
                QStringLiteral("run_flow"),
                QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 3}});
        };

        QJsonObject pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(runFlowStarts, 0);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(runFlowStarts, 0);

        pending = requestCycle();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 5000);
        QVERIFY(!sessionId.isEmpty());

        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!failed.success);
        QVERIFY(!failed.runId.isEmpty());
        QCOMPARE(failed.failedModule, instanceId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QVERIFY(failed.diagnostics.contains(QStringLiteral("writeAttempted=true")));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(!moduleFinished.last().at(1).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        QVERIFY(!engine.isBusy());

        const QJsonObject session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                              QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(session.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!session.value(QStringLiteral("retryable")).toBool());
        QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), 3);
        QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), 0);
        QVERIFY(!session.contains(QStringLiteral("lastSuccessfulRunId")));
        QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(), failed.runId);
        QCOMPARE(session.value(QStringLiteral("lastIterationRunId")).toString(), failed.runId);
        QCOMPARE(session.value(QStringLiteral("lastError")).toObject().value(QStringLiteral("code")).toInt(),
                 ExecError::SideEffectUncertain);
        const QJsonObject runStatus = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                                QJsonObject{{QStringLiteral("runId"), failed.runId}});
        QCOMPARE(runStatus.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(runStatus.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!runStatus.value(QStringLiteral("retryable")).toBool());
        const QJsonObject output = controller.handleToolCall(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), failed.runId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("key"), QStringLiteral("write_result")}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(output.value(QStringLiteral("reason")).toString(), QStringLiteral("side_effect_outcome_unknown"));
        QCOMPARE(output.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!output.contains(QStringLiteral("value")));

        const int moduleStartsAfterFailure = countModuleSignal(moduleStarted, instanceId);
        const int moduleFinishesAfterFailure = countModuleSignal(moduleFinished, instanceId);
        const int runsAfterFailure = runFinished.count();
        const int runFlowStartsAfterFailure = runFlowStarts;
        QElapsedTimer quietWindow;
        quietWindow.start();
        while (quietWindow.elapsed() < 550) {
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("get_run_results"),
                                         QJsonObject{{QStringLiteral("runId"), sessionId}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("uncertain"));
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("get_module_output"),
                                         QJsonObject{{QStringLiteral("runId"), failed.runId},
                                                     {QStringLiteral("instanceId"), instanceId},
                                                     {QStringLiteral("key"), QStringLiteral("write_result")}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("uncertain"));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), moduleStartsAfterFailure);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), moduleFinishesAfterFailure);
        QCOMPARE(runFinished.count(), runsAfterFailure);
        QCOMPARE(cycleStopped.count(), 1);
        QCOMPARE(runFlowStarts, runFlowStartsAfterFailure);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testStaticFileOutputsRejectExecutionBoundarySymlinkDrift() {
#ifndef Q_OS_UNIX
        QSKIP("The test requires POSIX symlink and rename semantics");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager& projects = ProjectManager::instance();
        FolderTestStateRestore stateRestore{controller, engine, m_targets, m_appDataDir};
        struct Case {
            QString plugin;
            QString entry;
        };
        const QList<Case> cases{{QStringLiteral("WriteText"), QStringLiteral("run")},
                                {QStringLiteral("SaveData"), QStringLiteral("step")},
                                {QStringLiteral("SaveImage"), QStringLiteral("resume")}};

        for (const Case& testCase : cases) {
            projects.closeProject();
            engine.stop();
            engine.clearModules();
            controller.clearConversation();
            controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

            const QString suffix = testCase.plugin.toLower();
            const QString rootA = m_targets.filePath(QStringLiteral("slice127-%1-a").arg(suffix));
            const QString rootB = m_targets.filePath(QStringLiteral("slice127-%1-b").arg(suffix));
            const QString alias = m_targets.filePath(QStringLiteral("slice127-%1-current").arg(suffix));
            const QString replacement = m_targets.filePath(QStringLiteral("slice127-%1-next").arg(suffix));
            QVERIFY(QDir().mkpath(rootA));
            QVERIFY(QDir().mkpath(rootB));
            if (!QFile::link(rootA, alias) || !QFileInfo(alias).isSymLink())
                QSKIP("The current filesystem does not provide directory symlinks");
            if (!QFile::link(rootB, replacement) || !QFileInfo(replacement).isSymLink())
                QSKIP("The current filesystem does not provide a second directory symlink");

            const QString extension = testCase.plugin == QLatin1String("WriteText")  ? QStringLiteral("txt")
                                      : testCase.plugin == QLatin1String("SaveData") ? QStringLiteral("json")
                                                                                     : QStringLiteral("png");
            const QString configuredPath = QDir(alias).filePath(QStringLiteral("output.") + extension);
            const QString targetA = QDir(rootA).filePath(QStringLiteral("output.") + extension);
            const QString targetB = QDir(rootB).filePath(QStringLiteral("output.") + extension);
            const QByteArray sentinelA("slice127-A-original-bytes\n");
            const QByteArray sentinelB("slice127-B-original-bytes\n");
            auto writeBytes = [](const QString& path, const QByteArray& bytes) {
                QFile file(path);
                return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
            };
            auto readBytes = [](const QString& path) {
                QFile file(path);
                return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
            };
            QVERIFY(writeBytes(targetA, sentinelA));
            QVERIFY(writeBytes(targetB, sentinelB));

            Project* project = projects.newProject();
            QVERIFY(project);
            const QString outputId = QStringLiteral("slice127_%1_output").arg(suffix);
            QString sourceId;
            if (testCase.plugin == QLatin1String("SaveImage")) {
                sourceId = QStringLiteral("slice127_saveimage_source");
                const QString inputPath = m_targets.filePath(QStringLiteral("slice127-input.png"));
                QImage image(3, 2, QImage::Format_RGB32);
                image.fill(qRgb(80, 40, 20));
                QVERIFY(image.save(inputPath, "PNG"));
                ModuleInstance source;
                source.id = sourceId;
                source.moduleId = QStringLiteral("GrabImage");
                source.name = QStringLiteral("Slice 127 image source");
                source.params = {{QStringLiteral("grabSource"), QStringLiteral("File")},
                                 {QStringLiteral("filePath"), inputPath}};
                project->addModule(source);
            }
            ModuleInstance output;
            output.id = outputId;
            output.moduleId = testCase.plugin;
            output.name = QStringLiteral("Slice 127 output boundary %1").arg(testCase.plugin);
            output.breakpoint = testCase.entry == QLatin1String("resume");
            if (testCase.plugin == QLatin1String("WriteText")) {
                output.params = {{QStringLiteral("filePath"), configuredPath},
                                 {QStringLiteral("textContent"), QStringLiteral("slice127 payload")},
                                 {QStringLiteral("appendMode"), false}};
            } else if (testCase.plugin == QLatin1String("SaveData")) {
                output.params = {{QStringLiteral("filePath"), configuredPath},
                                 {QStringLiteral("fileFormat"), QStringLiteral("json")},
                                 {QStringLiteral("appendMode"), false}};
            } else {
                output.params = {{QStringLiteral("filePath"), configuredPath},
                                 {QStringLiteral("format"), QStringLiteral("png")},
                                 {QStringLiteral("quality"), 95}};
            }
            project->addModule(output);
            if (!sourceId.isEmpty()) {
                ModuleConnection connection;
                connection.fromModuleId = sourceId;
                connection.toModuleId = outputId;
                connection.fromPort = QStringLiteral("image");
                connection.toPort = QStringLiteral("image");
                connection.edgeType = QStringLiteral("data");
                project->addConnection(connection);
            }

            QSignalSpy started(&engine, &RunEngine::moduleStarted);
            QSignalSpy finished(&engine, &RunEngine::moduleFinished);
            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
            QJsonObject pending;
            if (testCase.entry == QLatin1String("resume")) {
                pending = controller.handleToolCall(
                    QStringLiteral("run_flow"),
                    {{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
                QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
                QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
                QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Paused, 5000);
                const QString pausedRunId = engine.runObservation().runId;
                QVERIFY(!pausedRunId.isEmpty());
                engine.resume();
                QCOMPARE(engine.state(), RunState::Paused);
                QCOMPARE(engine.runObservation().runId, pausedRunId);
                QCOMPARE(countModuleSignal(started, outputId), 0);
                QCOMPARE(runFinished.count(), 0);
            }

            const QString tool = testCase.entry == QLatin1String("run")    ? QStringLiteral("run_flow")
                                 : testCase.entry == QLatin1String("step") ? QStringLiteral("step_once")
                                                                           : QStringLiteral("resume_flow");
            const QJsonObject args = tool == QLatin1String("run_flow")
                                         ? QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}}
                                         : QJsonObject{};
            pending = controller.handleToolCall(tool, args);
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString confirmationId = controller.pendingConfirmationId();
            QVERIFY(!confirmationId.isEmpty());

            QString swapError;
            QMutex swapMutex;
            QObject signalContext;
            const QMetaObject::Connection swap = QObject::connect(
                &engine, &RunEngine::moduleStarted, &signalContext,
                [&](const QString& id) {
                    if (id != outputId)
                        return;
                    const QByteArray from = QFile::encodeName(replacement);
                    const QByteArray to = QFile::encodeName(alias);
                    if (std::rename(from.constData(), to.constData()) != 0) {
                        QMutexLocker locker(&swapMutex);
                        swapError = QStringLiteral("atomic symlink rename failed");
                    }
                },
                Qt::DirectConnection);
            QVERIFY(controller.confirmPendingTools(confirmationId));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QObject::disconnect(swap);
            {
                QMutexLocker locker(&swapMutex);
                QVERIFY2(swapError.isEmpty(), qPrintable(swapError));
            }
            const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY(!result.success);
            QCOMPARE(result.errorCode, ExecError::ConfirmedTargetChanged);
            QCOMPARE(result.failedModule, outputId);
            QVERIFY(result.diagnostics.contains(QStringLiteral("CONFIRMED_TARGET_CHANGED")));
            QCOMPARE(readBytes(targetA), sentinelA);
            QCOMPARE(readBytes(targetB), sentinelB);
            QCOMPARE(countModuleSignal(started, outputId), 1);
            QCOMPARE(countModuleSignal(finished, outputId), 1);
            QVERIFY(!finished.last().at(1).toBool());
            QCOMPARE(audit.count(), testCase.entry == QLatin1String("resume") ? 2 : 1);
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, tool);
            const QJsonObject businessOutput = controller.handleToolCall(
                QStringLiteral("get_module_output"), {{QStringLiteral("runId"), result.runId},
                                                      {QStringLiteral("instanceId"), outputId},
                                                      {QStringLiteral("key"), QStringLiteral("write_result")}});
            QCOMPARE(businessOutput.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
            QVERIFY(!engine.isBusy());
            QFile::remove(alias);
            QFile::remove(replacement);
        }
#endif
    }

    void testPausedAgentSideEffectsRequireFreshResumeAuthorization() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager& projects = ProjectManager::instance();
        FolderTestStateRestore stateRestore{controller, engine, m_targets, m_appDataDir};
        const QStringList targetKinds{QStringLiteral("file"), QStringLiteral("folder")};

        for (const QString& kind : targetKinds) {
            projects.closeProject();
            engine.stop();
            engine.clearModules();
            controller.clearConversation();
            controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

            Project* project = projects.newProject();
            QVERIFY(project);
            const QString probeId = QStringLiteral("slice127_%1_probe").arg(kind);
            const QString targetId = QStringLiteral("slice127_%1_target").arg(kind);
            const QString probePath = m_targets.path();
            ModuleInstance probe;
            probe.id = probeId;
            probe.moduleId = QStringLiteral("Folder");
            probe.name = QStringLiteral("Slice 127 prior side-effect probe");
            probe.params = {{QStringLiteral("operation"), QStringLiteral("Exists")},
                            {QStringLiteral("folderPath"), probePath}};
            project->addModule(probe);

            const QString targetPath = m_targets.filePath(QStringLiteral("slice127-paused-%1").arg(kind));
            ModuleInstance target;
            target.id = targetId;
            target.name = QStringLiteral("Slice 127 paused side-effect target");
            target.breakpoint = true;
            if (kind == QLatin1String("file")) {
                target.moduleId = QStringLiteral("WriteText");
                target.params = {{QStringLiteral("filePath"), targetPath},
                                 {QStringLiteral("textContent"), QStringLiteral("fresh authorization")},
                                 {QStringLiteral("appendMode"), false}};
            } else {
                target.moduleId = QStringLiteral("Folder");
                target.params = {{QStringLiteral("operation"), QStringLiteral("Create")},
                                 {QStringLiteral("folderPath"), targetPath}};
            }
            project->addModule(target);

            QSignalSpy started(&engine, &RunEngine::moduleStarted);
            QSignalSpy finished(&engine, &RunEngine::moduleFinished);
            QSignalSpy runFinished(&engine, &RunEngine::runFinished);
            QString originalAuthorizationId;
            QMutex originalAuthorizationMutex;
            QObject probeContext;
            const QMetaObject::Connection probeObserver = QObject::connect(
                &engine, &RunEngine::moduleStarted, &probeContext,
                [&](const QString& id) {
                    if (id != probeId)
                        return;
                    if (ModuleBase* module = engine.getModule(probeId)) {
                        const char* propertyName = kind == QLatin1String("file")
                                                       ? AgentFileOutputAuthorizationProperty
                                                       : AgentFolderDeleteAuthorizationProperty;
                        QMutexLocker locker(&originalAuthorizationMutex);
                        originalAuthorizationId = module->property(propertyName)
                                                      .toJsonObject()
                                                      .value(QStringLiteral("executionAuthorizationId"))
                                                      .toString();
                    }
                },
                Qt::DirectConnection);
            QJsonObject pending =
                controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                       {QStringLiteral("wait"), false}});
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString initialConfirmation = controller.pendingConfirmationId();
            QVERIFY(controller.confirmPendingTools(initialConfirmation));
            QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Paused, 5000);
            QObject::disconnect(probeObserver);
            QString originalAuthorizationIdSnapshot;
            {
                QMutexLocker locker(&originalAuthorizationMutex);
                originalAuthorizationIdSnapshot = originalAuthorizationId;
            }
            QVERIFY(!originalAuthorizationIdSnapshot.isEmpty());
            const QString runId = engine.runObservation().runId;
            QVERIFY(!runId.isEmpty());
            QCOMPARE(countModuleSignal(started, probeId), 1);
            QCOMPARE(countModuleSignal(started, targetId), 0);
            QCOMPARE(countModuleSignal(finished, targetId), 0);
            QCOMPARE(runFinished.count(), 0);
            QVERIFY(!QFileInfo::exists(targetPath));

            engine.resume();
            QCOMPARE(engine.state(), RunState::Paused);
            QCOMPARE(engine.runObservation().runId, runId);
            QCOMPARE(countModuleSignal(started, targetId), 0);
            QCOMPARE(runFinished.count(), 0);
            QVERIFY(!QFileInfo::exists(targetPath));

            pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString freshConfirmation = controller.pendingConfirmationId();
            QVERIFY(!freshConfirmation.isEmpty());
            QVERIFY(freshConfirmation != initialConfirmation);
            QJsonObject resumedAuthorization;
            QMutex resumedAuthorizationMutex;
            const char* propertyName = kind == QLatin1String("file") ? AgentFileOutputAuthorizationProperty
                                                                     : AgentFolderDeleteAuthorizationProperty;
            QObject targetContext;
            const QMetaObject::Connection targetObserver = QObject::connect(
                &engine, &RunEngine::moduleStarted, &targetContext,
                [&](const QString& id) {
                    if (id == targetId) {
                        if (ModuleBase* module = engine.getModule(targetId)) {
                            QMutexLocker locker(&resumedAuthorizationMutex);
                            resumedAuthorization = module->property(propertyName).toJsonObject();
                        }
                    }
                },
                Qt::DirectConnection);
            QVERIFY(controller.confirmPendingTools(freshConfirmation));
            QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
            QObject::disconnect(targetObserver);
            const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
            QVERIFY2(result.success, qPrintable(result.errorMessage));
            QCOMPARE(result.runId, runId);
            QCOMPARE(countModuleSignal(started, probeId), 1);
            QCOMPARE(countModuleSignal(started, targetId), 1);
            QCOMPARE(countModuleSignal(finished, targetId), 1);
            QVERIFY(finished.last().at(1).toBool());
            QJsonObject resumedAuthorizationSnapshot;
            {
                QMutexLocker locker(&resumedAuthorizationMutex);
                resumedAuthorizationSnapshot = resumedAuthorization;
            }
            QCOMPARE(resumedAuthorizationSnapshot.value(QStringLiteral("confirmationId")).toString(),
                     freshConfirmation);
            QVERIFY(
                !resumedAuthorizationSnapshot.value(QStringLiteral("executionAuthorizationId")).toString().isEmpty());
            QVERIFY(resumedAuthorizationSnapshot.value(QStringLiteral("executionAuthorizationId")).toString() !=
                    originalAuthorizationIdSnapshot);
            QVERIFY(QFileInfo::exists(targetPath));
        }

        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        Project* cycleProject = projects.newProject();
        QVERIFY(cycleProject);
        const QString cycleId = QStringLiteral("slice127_cycle_write");
        const QString cyclePath = m_targets.filePath(QStringLiteral("slice127-cycle-output.txt"));
        ModuleInstance cycleWriter;
        cycleWriter.id = cycleId;
        cycleWriter.moduleId = QStringLiteral("WriteText");
        cycleWriter.name = QStringLiteral("Slice 127 bounded-cycle output");
        cycleWriter.params = {{QStringLiteral("filePath"), cyclePath},
                              {QStringLiteral("textContent"), QStringLiteral("cycle write")},
                              {QStringLiteral("appendMode"), true}};
        cycleProject->addModule(cycleWriter);

        QSignalSpy cycleRunFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy cycleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy cycleToolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QStringList cycleAuthorizationIds;
        QMutex cycleAuthorizationMutex;
        bool pausedFirstIteration = false;
        QObject cycleSignalContext;
        const QMetaObject::Connection captureCycleAuth = QObject::connect(
            &engine, &RunEngine::moduleStarted, &cycleSignalContext,
            [&](const QString& id) {
                if (id != cycleId)
                    return;
                if (ModuleBase* module = engine.getModule(cycleId)) {
                    QMutexLocker locker(&cycleAuthorizationMutex);
                    cycleAuthorizationIds.append(module->property(AgentFileOutputAuthorizationProperty)
                                                     .toJsonObject()
                                                     .value(QStringLiteral("executionAuthorizationId"))
                                                     .toString());
                }
            },
            Qt::DirectConnection);
        const QMetaObject::Connection pauseCycle = QObject::connect(
            &engine, &RunEngine::runFinished, &cycleSignalContext,
            [&](const RunResult&) {
                if (!pausedFirstIteration) {
                    pausedFirstIteration = true;
                    FlowExecutionService::instance().pauseRun();
                }
            },
            Qt::DirectConnection);
        QJsonObject cyclePending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(cyclePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString initialCycleConfirmation = controller.pendingConfirmationId();
        QVERIFY(controller.confirmPendingTools(initialCycleConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(cycleToolExecuted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(FlowExecutionService::instance().state(), FlowExecutionService::State::Paused, 5000);
        const QString sessionId =
            cycleToolExecuted.last().at(1).toJsonObject().value(QStringLiteral("sessionId")).toString();
        QVERIFY(!sessionId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(cycleRunFinished.count(), 1, 5000);
        QCOMPARE(countModuleSignal(cycleStarted, cycleId), 1);
        const QJsonObject pausedCycle =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), sessionId}});
        QCOMPARE(pausedCycle.value(QStringLiteral("completedCycles")).toInt(), 1);
        QCOMPARE(pausedCycle.value(QStringLiteral("status")).toString(), QStringLiteral("paused"));
        QFile cycleOutput(cyclePath);
        QVERIFY(cycleOutput.open(QIODevice::ReadOnly));
        const QByteArray firstCycleBytes = cycleOutput.readAll();
        cycleOutput.close();
        QVERIFY(!firstCycleBytes.isEmpty());

        engine.resume();
        QCOMPARE(FlowExecutionService::instance().state(), FlowExecutionService::State::Paused);
        QCOMPARE(countModuleSignal(cycleStarted, cycleId), 1);
        QTRY_COMPARE_WITH_TIMEOUT(cycleRunFinished.count(), 1, 5000);
        QCOMPARE(cycleStopped.count(), 0);
        QTest::qWait(250);
        QCOMPARE(FlowExecutionService::instance().state(), FlowExecutionService::State::Paused);
        QCOMPARE(countModuleSignal(cycleStarted, cycleId), 1);
        QTRY_COMPARE_WITH_TIMEOUT(cycleRunFinished.count(), 1, 5000);
        QVERIFY(cycleOutput.open(QIODevice::ReadOnly));
        QCOMPARE(cycleOutput.readAll(), firstCycleBytes);
        cycleOutput.close();

        cyclePending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(cyclePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString freshCycleConfirmation = controller.pendingConfirmationId();
        QVERIFY(!freshCycleConfirmation.isEmpty());
        QVERIFY(freshCycleConfirmation != initialCycleConfirmation);
        QVERIFY(controller.confirmPendingTools(freshCycleConfirmation));
        QTRY_VERIFY_WITH_TIMEOUT(cycleStopped.count() == 1 || !FlowExecutionService::instance().isActive(), 5000);
        QVERIFY2(cycleStopped.count() == 1,
                 qPrintable(QStringLiteral("cycleStopped missing: active=%1 state=%2 job=%3 runFinished=%4")
                                .arg(FlowExecutionService::instance().isActive())
                                .arg(static_cast<int>(FlowExecutionService::instance().state()))
                                .arg(static_cast<int>(FlowExecutionService::instance().currentJob()))
                                .arg(cycleRunFinished.count())));
        QObject::disconnect(captureCycleAuth);
        QObject::disconnect(pauseCycle);
        const RunResult resumedCycleResult = qvariant_cast<RunResult>(cycleRunFinished.last().at(0));
        QVERIFY2(resumedCycleResult.success, qPrintable(resumedCycleResult.errorMessage));
        const QJsonObject completedCycle =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), sessionId}});
        QCOMPARE(completedCycle.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(completedCycle.value(QStringLiteral("completedCycles")).toInt(), 2);
        QCOMPARE(completedCycle.value(QStringLiteral("lastCompletedRunId")).toString(), resumedCycleResult.runId);
        QFile resumedCycleOutput(cyclePath);
        QVERIFY(resumedCycleOutput.open(QIODevice::ReadOnly));
        QCOMPARE(resumedCycleOutput.readAll(), firstCycleBytes + QByteArray("cycle write\n"));
        resumedCycleOutput.close();
        QCOMPARE(countModuleSignal(cycleStarted, cycleId), 2);
        QTRY_COMPARE_WITH_TIMEOUT(cycleRunFinished.count(), 2, 5000);
        QStringList cycleAuthorizationIdsSnapshot;
        {
            QMutexLocker locker(&cycleAuthorizationMutex);
            cycleAuthorizationIdsSnapshot = cycleAuthorizationIds;
        }
        QCOMPARE(cycleAuthorizationIdsSnapshot.size(), 2);
        QVERIFY(!cycleAuthorizationIdsSnapshot[0].isEmpty());
        QVERIFY(!cycleAuthorizationIdsSnapshot[1].isEmpty());
        QVERIFY(cycleAuthorizationIdsSnapshot[0] != cycleAuthorizationIdsSnapshot[1]);
        QTest::qWait(250);
        QCOMPARE(countModuleSignal(cycleStarted, cycleId), 2);
        QTRY_COMPARE_WITH_TIMEOUT(cycleRunFinished.count(), 2, 5000);
        QCOMPARE(cycleStopped.count(), 1);
        QVERIFY(resumedCycleOutput.open(QIODevice::ReadOnly));
        QCOMPARE(resumedCycleOutput.readAll(), firstCycleBytes + QByteArray("cycle write\n"));
        resumedCycleOutput.close();
        QVERIFY(!engine.isBusy());
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        projects.closeProject();
    }

    void testStaticFileOutputFreshTargetConfirmationAndCycleNoReplay() {
#ifndef Q_OS_UNIX
        QSKIP("The symlink retargeting portion requires POSIX semantics");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager& projects = ProjectManager::instance();
        FolderTestStateRestore stateRestore{controller, engine, m_targets, m_appDataDir};
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const QString rootA = m_targets.filePath(QStringLiteral("slice127-fresh-root-a"));
        const QString rootB = m_targets.filePath(QStringLiteral("slice127-fresh-root-b"));
        const QString alias = m_targets.filePath(QStringLiteral("slice127-fresh-current"));
        const QString replacement = m_targets.filePath(QStringLiteral("slice127-fresh-next"));
        QVERIFY(QDir().mkpath(rootA));
        QVERIFY(QDir().mkpath(rootB));
        if (!QFile::link(rootA, alias) || !QFileInfo(alias).isSymLink())
            QSKIP("The current filesystem does not provide directory symlinks");
        if (!QFile::link(rootB, replacement) || !QFileInfo(replacement).isSymLink())
            QSKIP("The current filesystem does not provide a second directory symlink");
        const QString outputPath = QDir(alias).filePath(QStringLiteral("report.txt"));
        const QString pathA = QDir(rootA).filePath(QStringLiteral("report.txt"));
        const QString pathB = QDir(rootB).filePath(QStringLiteral("report.txt"));
        const QByteArray sentinelA("A stays intact\n");
        const QByteArray sentinelB("B stays intact until approved\n");
        auto writeBytes = [](const QString& path, const QByteArray& bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
        };
        auto readBytes = [](const QString& path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        QVERIFY(writeBytes(pathA, sentinelA));
        QVERIFY(writeBytes(pathB, sentinelB));
        Project* project = projects.newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice127_fresh_confirmation_writer");
        ModuleInstance writer;
        writer.id = instanceId;
        writer.moduleId = QStringLiteral("WriteText");
        writer.name = QStringLiteral("Slice 127 fresh approved output target");
        writer.params = {{QStringLiteral("filePath"), outputPath},
                         {QStringLiteral("textContent"), QStringLiteral("freshly approved B")},
                         {QStringLiteral("appendMode"), false}};
        project->addModule(writer);
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QJsonObject pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleConfirmation = controller.pendingConfirmationId();
        QString renameError;
        QMutex renameMutex;
        QObject signalContext;
        const auto renameAtExecution = QObject::connect(
            &engine, &RunEngine::moduleStarted, &signalContext,
            [&](const QString& id) {
                if (id != instanceId)
                    return;
                const QByteArray from = QFile::encodeName(replacement);
                const QByteArray to = QFile::encodeName(alias);
                if (std::rename(from.constData(), to.constData()) != 0) {
                    QMutexLocker locker(&renameMutex);
                    renameError = QStringLiteral("atomic symlink rename failed");
                }
            },
            Qt::DirectConnection);
        QVERIFY(controller.confirmPendingTools(staleConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QObject::disconnect(renameAtExecution);
        {
            QMutexLocker locker(&renameMutex);
            QVERIFY2(renameError.isEmpty(), qPrintable(renameError));
        }
        const RunResult staleResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!staleResult.success);
        QCOMPARE(staleResult.errorCode, ExecError::ConfirmedTargetChanged);
        QCOMPARE(readBytes(pathA), sentinelA);
        QCOMPARE(readBytes(pathB), sentinelB);

        pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString rejectedConfirmation = controller.pendingConfirmationId();
        QVERIFY(rejectedConfirmation != staleConfirmation);
        const QJsonArray effects = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray();
        QVERIFY2(effects.last().toString().startsWith(QStringLiteral("file_write:%1 ").arg(outputPath)) &&
                     effects.last().toString().contains(QStringLiteral("resolved_target=%1").arg(pathB)) &&
                     effects.last().toString().endsWith(QStringLiteral("format=text mode=overwrite")),
                 qPrintable(QJsonDocument(effects).toJson(QJsonDocument::Compact)));
        QVERIFY(controller.rejectPendingTools(rejectedConfirmation));
        QCOMPARE(readBytes(pathA), sentinelA);
        QCOMPARE(readBytes(pathB), sentinelB);

        pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString freshConfirmation = controller.pendingConfirmationId();
        QVERIFY(freshConfirmation != rejectedConfirmation);
        QVERIFY(controller.confirmPendingTools(freshConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 5000);
        const RunResult freshResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY2(freshResult.success, qPrintable(freshResult.errorMessage));
        QCOMPARE(readBytes(pathA), sentinelA);
        QVERIFY(readBytes(pathB) != sentinelB);
        QVERIFY(readBytes(pathB).contains("freshly approved B"));
        QCOMPARE(audit.count(), 3);
        QCOMPARE(audit.at(1).at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(audit.at(2).at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        // A runtime effective-parameter change at the execution boundary is rejected before the file is opened.
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        project = projects.newProject();
        QVERIFY(project);
        const QString parameterId = QStringLiteral("slice127_parameter_drift_writer");
        const QString parameterPath = m_targets.filePath(QStringLiteral("slice127-parameter-drift.txt"));
        const QByteArray parameterSentinel("parameter target sentinel\n");
        QObject driftContext;
        QVERIFY(writeBytes(parameterPath, parameterSentinel));
        writer.id = parameterId;
        writer.params = {{QStringLiteral("filePath"), parameterPath},
                         {QStringLiteral("textContent"), QStringLiteral("approved value")},
                         {QStringLiteral("appendMode"), false}};
        project->addModule(writer);
        QSignalSpy parameterRunFinished(&engine, &RunEngine::runFinished);
        pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const auto drift = QObject::connect(
            &engine, &RunEngine::moduleStarted, &driftContext,
            [&](const QString& id) {
                if (id == parameterId) {
                    if (ModuleBase* module = engine.getModule(parameterId))
                        module->setParam(QStringLiteral("textContent"), QStringLiteral("drifted value"));
                }
            },
            Qt::DirectConnection);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(parameterRunFinished.count(), 1, 5000);
        QObject::disconnect(drift);
        const RunResult parameterResult = qvariant_cast<RunResult>(parameterRunFinished.last().at(0));
        QVERIFY(!parameterResult.success);
        QCOMPARE(parameterResult.errorCode, ExecError::ConfirmedTargetChanged);
        QCOMPARE(parameterResult.failedModule, parameterId);
        QCOMPARE(readBytes(parameterPath), parameterSentinel);

        // A fixed target in a maxCycles=1 session is authorized and executed exactly once.
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        project = projects.newProject();
        QVERIFY(project);
        const QString singleCycleId = QStringLiteral("slice127_static_cycle_once_writer");
        const QString singleCyclePath = m_targets.filePath(QStringLiteral("slice127-static-cycle-once.txt"));
        writer.id = singleCycleId;
        writer.params = {{QStringLiteral("filePath"), singleCyclePath},
                         {QStringLiteral("textContent"), QStringLiteral("one cycle")},
                         {QStringLiteral("appendMode"), true}};
        project->addModule(writer);
        QSignalSpy singleCycleStarts(&engine, &RunEngine::moduleStarted);
        QSignalSpy singleCycleStops(&engine, &RunEngine::cycleStopped);
        pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 1},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(singleCycleStops.count(), 1, 5000);
        QCOMPARE(countModuleSignal(singleCycleStarts, singleCycleId), 1);
        QFile singleCycleOutput(singleCyclePath);
        QVERIFY(singleCycleOutput.open(QIODevice::ReadOnly));
        QCOMPARE(singleCycleOutput.readAll(), QByteArray("one cycle\n"));

        // A second bounded-cycle iteration must revalidate the same frozen target and stop without touching B.
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        const QString cycleRootA = m_targets.filePath(QStringLiteral("slice127-multicycle-root-a"));
        const QString cycleRootB = m_targets.filePath(QStringLiteral("slice127-multicycle-root-b"));
        const QString cycleAlias = m_targets.filePath(QStringLiteral("slice127-multicycle-current"));
        const QString cycleReplacement = m_targets.filePath(QStringLiteral("slice127-multicycle-next"));
        QVERIFY(QDir().mkpath(cycleRootA));
        QVERIFY(QDir().mkpath(cycleRootB));
        if (!QFile::link(cycleRootA, cycleAlias) || !QFileInfo(cycleAlias).isSymLink())
            QSKIP("The current filesystem does not provide directory symlinks");
        if (!QFile::link(cycleRootB, cycleReplacement) || !QFileInfo(cycleReplacement).isSymLink())
            QSKIP("The current filesystem does not provide a second directory symlink");
        const QString cycleConfiguredPath = QDir(cycleAlias).filePath(QStringLiteral("cycle.txt"));
        const QString cyclePathA = QDir(cycleRootA).filePath(QStringLiteral("cycle.txt"));
        const QString cyclePathB = QDir(cycleRootB).filePath(QStringLiteral("cycle.txt"));
        const QByteArray cycleSentinelA("cycle A sentinel\n");
        const QByteArray cycleSentinelB("cycle B sentinel\n");
        QVERIFY(writeBytes(cyclePathA, cycleSentinelA));
        QVERIFY(writeBytes(cyclePathB, cycleSentinelB));
        project = projects.newProject();
        QVERIFY(project);
        const QString multiCycleId = QStringLiteral("slice127_multicycle_writer");
        writer.id = multiCycleId;
        writer.params = {{QStringLiteral("filePath"), cycleConfiguredPath},
                         {QStringLiteral("textContent"), QStringLiteral("one approved write")},
                         {QStringLiteral("appendMode"), true}};
        project->addModule(writer);
        QSignalSpy multiCycleStarts(&engine, &RunEngine::moduleStarted);
        QSignalSpy multiCycleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy multiCycleRunFinished(&engine, &RunEngine::runFinished);
        QSignalSpy multiCycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy multiCycleTools(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy multiCycleAudit(&controller, &AgentController::actionLogEntryAdded);
        QString swapError;
        QMutex swapMutex;
        QObject swapContext;
        const QMetaObject::Connection swapBetweenIterations = QObject::connect(
            &engine, &RunEngine::runFinished, &swapContext,
            [&](const RunResult& result) {
                if (!result.success)
                    return;
                const QByteArray from = QFile::encodeName(cycleReplacement);
                const QByteArray to = QFile::encodeName(cycleAlias);
                if (std::rename(from.constData(), to.constData()) != 0) {
                    QMutexLocker locker(&swapMutex);
                    swapError = QStringLiteral("atomic symlink rename failed");
                }
            },
            Qt::DirectConnection);
        pending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 3},
                                                                   {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(multiCycleTools.count(), 1, 5000);
        const QString multiCycleSession =
            multiCycleTools.last().at(1).toJsonObject().value(QStringLiteral("sessionId")).toString();
        QVERIFY(!multiCycleSession.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(multiCycleStopped.count(), 1, 10000);
        QObject::disconnect(swapBetweenIterations);
        {
            QMutexLocker locker(&swapMutex);
            QVERIFY2(swapError.isEmpty(), qPrintable(swapError));
        }
        QCOMPARE(multiCycleStarts.count(), 2);
        QCOMPARE(multiCycleFinished.count(), 2);
        QVERIFY(multiCycleFinished.first().at(1).toBool());
        QVERIFY(!multiCycleFinished.last().at(1).toBool());
        const RunResult secondIteration = qvariant_cast<RunResult>(multiCycleRunFinished.last().at(0));
        QVERIFY(!secondIteration.success);
        QCOMPARE(secondIteration.errorCode, ExecError::ConfirmedTargetChanged);
        QCOMPARE(secondIteration.failedModule, multiCycleId);
        QCOMPARE(readBytes(cyclePathA), cycleSentinelA + QByteArray("one approved write\n"));
        QCOMPARE(readBytes(cyclePathB), cycleSentinelB);
        QTRY_COMPARE_WITH_TIMEOUT(multiCycleRunFinished.count(), 2, 5000);
        QVERIFY(multiCycleAudit.count() >= 1);
        const QJsonObject failedSession = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                                    {{QStringLiteral("runId"), multiCycleSession}});
        QCOMPARE(failedSession.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(failedSession.value(QStringLiteral("completedCycles")).toInt(), 1);
        const int toolsAfterFailure = multiCycleTools.count();
        const int auditAfterFailure = multiCycleAudit.count();
        QVERIFY(toolsAfterFailure >= 1);
        const QJsonObject cycleError = failedSession.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(cycleError.value(QStringLiteral("code")).toInt(), ExecError::ConfirmedTargetChanged);
        QCOMPARE(cycleError.value(QStringLiteral("codeName")).toString(), QStringLiteral("CONFIRMED_TARGET_CHANGED"));
        QCOMPARE(cycleError.value(QStringLiteral("moduleId")).toString(), multiCycleId);
        QTest::qWait(350);
        QCOMPARE(multiCycleStarts.count(), 2);
        QCOMPARE(multiCycleFinished.count(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(multiCycleRunFinished.count(), 2, 5000);
        QCOMPARE(multiCycleStopped.count(), 1);
        QCOMPARE(multiCycleTools.count(), toolsAfterFailure);
        QCOMPARE(multiCycleAudit.count(), auditAfterFailure);
        QCOMPARE(readBytes(cyclePathB), cycleSentinelB);

        // An ordinary RunEngine call without Agent authorization keeps its existing output behavior.
        projects.closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        project = projects.newProject();
        QVERIFY(project);
        const QString ordinaryId = QStringLiteral("slice127_ordinary_writer");
        const QString ordinaryPath = m_targets.filePath(QStringLiteral("slice127-ordinary-output.txt"));
        writer.id = ordinaryId;
        writer.params = {{QStringLiteral("filePath"), ordinaryPath},
                         {QStringLiteral("textContent"), QStringLiteral("ordinary output")},
                         {QStringLiteral("appendMode"), false}};
        project->addModule(writer);
        QVERIFY(engine.loadProject(project));
        QSignalSpy ordinaryFinished(&engine, &RunEngine::runFinished);
        QJsonObject ordinaryAuthorization;
        QObject ordinaryContext;
        const QMetaObject::Connection ordinaryObserver = QObject::connect(
            &engine, &RunEngine::moduleStarted, &ordinaryContext,
            [&](const QString& id) {
                if (id == ordinaryId) {
                    if (ModuleBase* module = engine.getModule(ordinaryId))
                        ordinaryAuthorization = module->property(AgentFileOutputAuthorizationProperty).toJsonObject();
                }
            },
            Qt::DirectConnection);
        engine.runOnce();
        QTRY_COMPARE_WITH_TIMEOUT(ordinaryFinished.count(), 1, 5000);
        QObject::disconnect(ordinaryObserver);
        const RunResult ordinaryResult = qvariant_cast<RunResult>(ordinaryFinished.last().at(0));
        QVERIFY2(ordinaryResult.success, qPrintable(ordinaryResult.errorMessage));
        QVERIFY(!ordinaryAuthorization.value(QStringLiteral("active")).toBool());
        QFile ordinaryOutput(ordinaryPath);
        QVERIFY(ordinaryOutput.open(QIODevice::ReadOnly));
        QCOMPARE(ordinaryOutput.readAll(), QByteArray("ordinary output\n"));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        projects.closeProject();
#endif
    }

    void testFolderDeleteRequiresConfirmationForResumeFlow() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice34_folder_resume_delete");
        const QString target = makeDeleteTarget(QStringLiteral("resume-flow-target"));
        QVERIFY(!target.isEmpty());
        ModuleInstance folder;
        folder.id = instanceId;
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Slice 34 temporary Folder delete");
        folder.breakpoint = true;
        folder.params.insert(QStringLiteral("operation"), QStringLiteral("Delete"));
        folder.params.insert(QStringLiteral("folderPath"), target);
        project->addModule(folder);

        QVERIFY(engine.loadProject(project));
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QVERIFY(QFileInfo::exists(QDir(target).filePath(QStringLiteral("probe.txt"))));
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);

        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString rejectedConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!rejectedConfirmationId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedConfirmationId));
        QCOMPARE(engine.state(), RunState::Paused);
        QVERIFY(QFileInfo::exists(QDir(target).filePath(QStringLiteral("probe.txt"))));
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 0);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 0);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!approvedConfirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedConfirmationId));

        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(engine.state(), RunState::Idle);
        QVERIFY(!engine.isBusy());
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QVERIFY(!QFileInfo::exists(target));
        QCOMPARE(countModuleSignal(moduleStarted, instanceId), 1);
        QCOMPARE(countModuleSignal(moduleFinished, instanceId), 1);
        QCOMPARE(moduleFinished.last().at(0).toString(), instanceId);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QVERIFY(!audit.isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
    }
};

QTEST_MAIN(TestAgentFolderExecutionG5)
#include "test_agent_folder_execution_g5.moc"
