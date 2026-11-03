#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestAgentPluginG5 : public QObject {
    Q_OBJECT

private:
    struct EnvironmentRestore {
        QByteArray appDataDir = qgetenv("DEEPLUX_APP_DATA_DIR");
        QByteArray schemaVersion = qgetenv("DEEPLUX_TEST_SCHEMA_VERSION");
        bool hadAppDataDir = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
        bool hadSchemaVersion = qEnvironmentVariableIsSet("DEEPLUX_TEST_SCHEMA_VERSION");

        ~EnvironmentRestore() {
            if (hadAppDataDir)
                qputenv("DEEPLUX_APP_DATA_DIR", appDataDir);
            else
                qunsetenv("DEEPLUX_APP_DATA_DIR");
            if (hadSchemaVersion)
                qputenv("DEEPLUX_TEST_SCHEMA_VERSION", schemaVersion);
            else
                qunsetenv("DEEPLUX_TEST_SCHEMA_VERSION");
        }
    };

    QTemporaryDir m_appDataDir;
    EnvironmentRestore m_environmentRestore;

    static bool installFixture(const QString& pluginRoot, const QString& name, const QString& libraryPath,
                               const QString& metadataPath) {
        const QDir root(pluginRoot);
        if (!QDir().mkpath(root.filePath(name)))
            return false;
        const QDir pluginDir(root.filePath(name));
        return QFile::copy(metadataPath, pluginDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(libraryPath, pluginDir.filePath(QFileInfo(libraryPath).fileName()));
    }

private slots:
    void init() {
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
    }

    void initTestCase() {
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        qputenv("DEEPLUX_TEST_SCHEMA_VERSION", "v1");

        const QString pluginRoot = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins"));
        const QString fixtures = QStringLiteral(AGENT_G5_FIXTURE_DIR);
        QVERIFY(installFixture(pluginRoot, QStringLiteral("AgentAbiMismatchFixture"),
                               QStringLiteral(AGENT_G5_ABI_FIXTURE),
                               QDir(fixtures).filePath(QStringLiteral("abi_metadata.json"))));
        QVERIFY(installFixture(pluginRoot, QStringLiteral("AgentSchemaCacheFixture"),
                               QStringLiteral(AGENT_G5_SCHEMA_FIXTURE),
                               QDir(fixtures).filePath(QStringLiteral("schema_metadata.json"))));
        QVERIFY(installFixture(pluginRoot, QStringLiteral("AgentInterfaceVersionMismatchFixture"),
                               QStringLiteral(AGENT_G5_VERSION_FIXTURE),
                               QDir(fixtures).filePath(QStringLiteral("version_mismatch_metadata.json"))));

        PluginManager& manager = PluginManager::instance();
        QVERIFY(manager.initialize());
        QCOMPARE(manager.pluginPaths(), QStringList{QDir(pluginRoot).absolutePath()});
        QStringList discovered = manager.availableModules();
        discovered.sort();
        QCOMPARE(discovered, QStringList({QStringLiteral("AgentAbiMismatchFixture"),
                                          QStringLiteral("AgentInterfaceVersionMismatchFixture"),
                                          QStringLiteral("AgentSchemaCacheFixture")}));
        QVERIFY(manager.availableCameras().isEmpty());
        UserManager::instance().login(QStringLiteral("developer"), QString());
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
    }

    void cleanupTestCase() {
        PluginManager& manager = PluginManager::instance();
        manager.shutdown();
        QVERIFY(!manager.isPluginLoaded(QStringLiteral("AgentAbiMismatchFixture")));
        QVERIFY(!manager.isPluginLoaded(QStringLiteral("AgentSchemaCacheFixture")));
        QVERIFY(!manager.isPluginLoaded(QStringLiteral("AgentInterfaceVersionMismatchFixture")));
    }

    void testAbiMismatchRejectedThroughAgentTool() {
        ToolSchema::instance().registerDefaultTools();
        AgentActor actor;
        PluginManager& manager = PluginManager::instance();
        QSignalSpy failedSpy(&manager, &PluginManager::pluginLoadFailed);
        QSignalSpy loadedSpy(&manager, &PluginManager::pluginLoaded);

        const QJsonObject result =
            actor.executeTool(QStringLiteral("load_module_plugin"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("AgentAbiMismatchFixture")}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PLUGIN_LOAD_FAILED"));
        const QString reason =
            result.value(QStringLiteral("details")).toObject().value(QStringLiteral("reason")).toString();
        QVERIFY2(reason.contains(QStringLiteral("does not implement IModule ABI v2")), qPrintable(reason));
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("AgentAbiMismatchFixture"));
        QCOMPARE(loadedSpy.count(), 0);
        QVERIFY(!manager.isPluginLoaded(QStringLiteral("AgentAbiMismatchFixture")));
        QVERIFY(!manager.pluginInfo(QStringLiteral("AgentAbiMismatchFixture")).loaded);
        QVERIFY(manager.pluginInfo(QStringLiteral("AgentSchemaCacheFixture")).name ==
                QStringLiteral("AgentSchemaCacheFixture"));
    }

    void testInterfaceVersionMismatchRejectedAndValidLoadContinues() {
        ToolSchema::instance().registerDefaultTools();
        AgentActor actor;
        PluginManager& manager = PluginManager::instance();
        QSignalSpy failedSpy(&manager, &PluginManager::pluginLoadFailed);
        QSignalSpy loadedSpy(&manager, &PluginManager::pluginLoaded);
        QSignalSpy unloadedSpy(&manager, &PluginManager::pluginUnloaded);

        const QJsonObject rejected = actor.executeTool(
            QStringLiteral("load_module_plugin"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("AgentInterfaceVersionMismatchFixture")}});
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PLUGIN_LOAD_FAILED"));
        const QString reason =
            rejected.value(QStringLiteral("details")).toObject().value(QStringLiteral("reason")).toString();
        QVERIFY2(reason.contains(QStringLiteral("Interface version mismatch")), qPrintable(reason));
        QVERIFY2(reason.contains(QStringLiteral("expected=2")), qPrintable(reason));
        QVERIFY2(reason.contains(QStringLiteral("actual=1")), qPrintable(reason));
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("AgentInterfaceVersionMismatchFixture"));
        QCOMPARE(loadedSpy.count(), 0);
        QVERIFY(!manager.isPluginLoaded(QStringLiteral("AgentInterfaceVersionMismatchFixture")));
        QVERIFY(!manager.pluginInfo(QStringLiteral("AgentInterfaceVersionMismatchFixture")).loaded);

        const QJsonObject accepted =
            actor.executeTool(QStringLiteral("load_module_plugin"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("AgentSchemaCacheFixture")}});
        QCOMPARE(accepted.value(QStringLiteral("status")).toString(), QStringLiteral("loaded"));
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(loadedSpy.at(0).at(0).toString(), QStringLiteral("AgentSchemaCacheFixture"));
        QVERIFY(manager.isPluginLoaded(QStringLiteral("AgentSchemaCacheFixture")));
        QString unloadError;
        QVERIFY2(manager.tryUnloadPlugin(QStringLiteral("AgentSchemaCacheFixture"), &unloadError),
                 qPrintable(unloadError));
        QCOMPARE(unloadedSpy.count(), 1);
        QVERIFY(!manager.isPluginLoaded(QStringLiteral("AgentSchemaCacheFixture")));
    }

    void testSchemaCacheInvalidatedAcrossUnloadReload() {
        ToolSchema::instance().registerDefaultTools();
        AgentActor actor;
        PluginManager& manager = PluginManager::instance();
        QSignalSpy loadedSpy(&manager, &PluginManager::pluginLoaded);
        QSignalSpy unloadedSpy(&manager, &PluginManager::pluginUnloaded);
        const QJsonObject args{{QStringLiteral("plugin"), QStringLiteral("AgentSchemaCacheFixture")}};

        QVERIFY(manager.loadPlugin(QStringLiteral("AgentSchemaCacheFixture")));
        QCOMPARE(loadedSpy.count(), 1);
        const QJsonObject v1 = actor.executeTool(QStringLiteral("get_module_params_schema"), args);
        const QJsonObject defaultsV1 = v1.value(QStringLiteral("defaultParams")).toObject();
        QCOMPARE(defaultsV1.value(QStringLiteral("oldField")).toString(), QStringLiteral("schema-v1"));
        QCOMPARE(defaultsV1.value(QStringLiteral("sharedField")).toInt(), 1);
        QVERIFY(!defaultsV1.contains(QStringLiteral("newField")));

        QString unloadError;
        QVERIFY2(manager.tryUnloadPlugin(QStringLiteral("AgentSchemaCacheFixture"), &unloadError),
                 qPrintable(unloadError));
        QCOMPARE(unloadedSpy.count(), 1);
        qputenv("DEEPLUX_TEST_SCHEMA_VERSION", "v2");
        QVERIFY(manager.loadPlugin(QStringLiteral("AgentSchemaCacheFixture")));
        QCOMPARE(loadedSpy.count(), 2);

        const QJsonObject v2 = actor.executeTool(QStringLiteral("get_module_params_schema"), args);
        const QJsonObject defaultsV2 = v2.value(QStringLiteral("defaultParams")).toObject();
        QCOMPARE(defaultsV2.value(QStringLiteral("newField")).toString(), QStringLiteral("schema-v2"));
        QCOMPARE(defaultsV2.value(QStringLiteral("sharedField")).toInt(), 2);
        QVERIFY(!defaultsV2.contains(QStringLiteral("oldField")));
        const QJsonObject parametersV2 = v2.value(QStringLiteral("parameters")).toObject();
        QVERIFY(parametersV2.contains(QStringLiteral("newField")));
        QVERIFY(!parametersV2.contains(QStringLiteral("oldField")));

        QVERIFY2(manager.tryUnloadPlugin(QStringLiteral("AgentSchemaCacheFixture"), &unloadError),
                 qPrintable(unloadError));
        QCOMPARE(unloadedSpy.count(), 2);
        QVERIFY(!manager.isPluginLoaded(QStringLiteral("AgentSchemaCacheFixture")));
    }

    void testPluginLoadConfirmationRejectsSystemConfigRevocation() {
        ToolSchema::instance().registerDefaultTools();
        PluginManager& manager = PluginManager::instance();
        const QString pluginName = QStringLiteral("AgentSchemaCacheFixture");
        QVERIFY(!manager.isPluginLoaded(pluginName));
        UserManager::instance().login(QStringLiteral("developer"), QString());
        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        QSignalSpy loadedSpy(&manager, &PluginManager::pluginLoaded);
        QSignalSpy unloadedSpy(&manager, &PluginManager::pluginUnloaded);
        QSignalSpy failedSpy(&manager, &PluginManager::pluginLoadFailed);
        QSignalSpy executedSpy(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy responseSpy(&controller, &AgentController::llmResponseReceived);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);

        const QJsonObject pending =
            controller.handleToolCall(QStringLiteral("load_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!controller.pendingConfirmationId().isEmpty());
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 0);

        UserManager::instance().login(QStringLiteral("guest-session"), QString());
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("SystemConfig")));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(unloadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 0);
        QCOMPARE(responseSpy.size(), 1);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.size(), 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
        QCOMPARE(controller.conversationHistorySize(), 0);

        UserManager::instance().login(QStringLiteral("developer"), QString());
        const QJsonObject operatorPending =
            controller.handleToolCall(QStringLiteral("load_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(operatorPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const int loadedBeforeOperator = loadedSpy.count();
        const int unloadedBeforeOperator = unloadedSpy.count();
        const int failedBeforeOperator = failedSpy.count();
        const int executedBeforeOperator = executedSpy.count();
        const int responsesBeforeOperator = responseSpy.size();
        const int auditsBeforeOperator = auditSpy.size();
        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("SystemConfig")));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), loadedBeforeOperator);
        QCOMPARE(unloadedSpy.count(), unloadedBeforeOperator);
        QCOMPARE(failedSpy.count(), failedBeforeOperator);
        QCOMPARE(executedSpy.count(), executedBeforeOperator);
        QCOMPARE(responseSpy.size(), responsesBeforeOperator + 1);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.size(), auditsBeforeOperator + 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
        QCOMPARE(controller.conversationHistorySize(), 0);

        UserManager::instance().login(QStringLiteral("developer"), QString());
        const QJsonObject logoutPending =
            controller.handleToolCall(QStringLiteral("load_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(logoutPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const int loadedBeforeLogout = loadedSpy.count();
        const int unloadedBeforeLogout = unloadedSpy.count();
        const int failedBeforeLogout = failedSpy.count();
        const int executedBeforeLogout = executedSpy.count();
        const int responsesBeforeLogout = responseSpy.size();
        const int auditsBeforeLogout = auditSpy.size();
        UserManager::instance().logout();
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("SystemConfig")));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), loadedBeforeLogout);
        QCOMPARE(unloadedSpy.count(), unloadedBeforeLogout);
        QCOMPARE(failedSpy.count(), failedBeforeLogout);
        QCOMPARE(executedSpy.count(), executedBeforeLogout);
        QCOMPARE(responseSpy.size(), responsesBeforeLogout + 1);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.size(), auditsBeforeLogout + 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
        QCOMPARE(controller.conversationHistorySize(), 0);

        UserManager::instance().login(QStringLiteral("developer"), QString());
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
    }

    void testPluginLifecycleApprovalLoadsAndUnloadsExactlyOnce() {
        ToolSchema::instance().registerDefaultTools();
        PluginManager& manager = PluginManager::instance();
        const QString pluginName = QStringLiteral("AgentSchemaCacheFixture");
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QVERIFY(!RunEngine::instance().isBusy());

        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy loadedSpy(&manager, &PluginManager::pluginLoaded);
        QSignalSpy unloadedSpy(&manager, &PluginManager::pluginUnloaded);
        QSignalSpy failedSpy(&manager, &PluginManager::pluginLoadFailed);
        QSignalSpy executedSpy(controller.actor(), &AgentActor::toolExecuted);

        const QJsonObject loadPending =
            controller.handleToolCall(QStringLiteral("load_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(loadPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString rejectedLoadId = controller.pendingConfirmationId();
        QVERIFY(!rejectedLoadId.isEmpty());
        const QString loadEffect = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray()
                                       .first()
                                       .toString();
        QVERIFY(loadEffect.startsWith(QStringLiteral("plugin_load:%1 target=").arg(pluginName)));
        QVERIFY(loadEffect.contains(QStringLiteral("loaded=false")));
        QVERIFY(loadEffect.contains(QStringLiteral("sha256=")));
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(unloadedSpy.count(), 0);
        QVERIFY(controller.rejectPendingTools(rejectedLoadId));
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(unloadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 0);

        const QJsonObject loadRetry =
            controller.handleToolCall(QStringLiteral("load_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(loadRetry.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedLoadId = controller.pendingConfirmationId();
        QVERIFY(!approvedLoadId.isEmpty());
        QVERIFY(approvedLoadId != rejectedLoadId);
        QVERIFY(!controller.confirmPendingTools(rejectedLoadId));
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QCOMPARE(controller.pendingConfirmationId(), approvedLoadId);
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(unloadedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 0);
        QVERIFY(controller.confirmPendingTools(approvedLoadId));
        QTRY_COMPARE(executedSpy.count(), 1);
        QCOMPARE(executedSpy.last().at(0).toString(), QStringLiteral("load_module_plugin"));
        QCOMPARE(executedSpy.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("loaded"));
        QVERIFY(manager.isPluginLoaded(pluginName));
        QVERIFY(manager.pluginInfo(pluginName).loaded);
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(loadedSpy.first().first().toString(), pluginName);
        QCOMPARE(unloadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);

        const QJsonObject unloadPending =
            controller.handleToolCall(QStringLiteral("unload_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(unloadPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString rejectedUnloadId = controller.pendingConfirmationId();
        QVERIFY(!rejectedUnloadId.isEmpty());
        const QString unloadEffect = controller.pendingConfirmationPayload()
                                         .value(QStringLiteral("context"))
                                         .toObject()
                                         .value(QStringLiteral("sideEffects"))
                                         .toArray()
                                         .first()
                                         .toString();
        QVERIFY(unloadEffect.startsWith(QStringLiteral("plugin_unload:%1 target=").arg(pluginName)));
        QVERIFY(unloadEffect.contains(QStringLiteral("loaded=true")));
        QVERIFY(unloadEffect.contains(QStringLiteral("sha256=")));
        QVERIFY(manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(unloadedSpy.count(), 0);
        QVERIFY(controller.rejectPendingTools(rejectedUnloadId));
        QVERIFY(manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(unloadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 1);

        const QJsonObject unloadRetry =
            controller.handleToolCall(QStringLiteral("unload_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(unloadRetry.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedUnloadId = controller.pendingConfirmationId();
        QVERIFY(!approvedUnloadId.isEmpty());
        QVERIFY(approvedUnloadId != rejectedUnloadId);
        QVERIFY(!controller.confirmPendingTools(rejectedUnloadId));
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QCOMPARE(controller.pendingConfirmationId(), approvedUnloadId);
        QVERIFY(manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(unloadedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 1);
        QVERIFY(controller.confirmPendingTools(approvedUnloadId));
        QTRY_COMPARE(executedSpy.count(), 2);
        QCOMPARE(executedSpy.last().at(0).toString(), QStringLiteral("unload_module_plugin"));
        QCOMPARE(executedSpy.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("unloaded"));
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QVERIFY(!manager.pluginInfo(pluginName).loaded);
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(unloadedSpy.count(), 1);
        QCOMPARE(unloadedSpy.first().first().toString(), pluginName);
        QCOMPARE(failedSpy.count(), 0);
        QVERIFY(!controller.confirmPendingTools(approvedUnloadId));
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(unloadedSpy.count(), 1);
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QVERIFY(!RunEngine::instance().isBusy());

        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testPluginLifecycleConfirmationExpiresOnBinaryAndLoadedStateDrift() {
        ToolSchema::instance().registerDefaultTools();
        PluginManager& manager = PluginManager::instance();
        const QString pluginName = QStringLiteral("AgentSchemaCacheFixture");
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QVERIFY(!RunEngine::instance().isBusy());

        const PluginInfo info = manager.pluginInfo(pluginName);
        QVERIFY(!info.path.isEmpty());
        const QDir pluginDirectory = QFileInfo(info.path).absoluteDir();
#ifdef Q_OS_WIN
        const QStringList libraries = pluginDirectory.entryList({QStringLiteral("*.dll")}, QDir::Files, QDir::Name);
#else
        const QStringList libraries = pluginDirectory.entryList({QStringLiteral("*.so")}, QDir::Files, QDir::Name);
#endif
        QCOMPARE(libraries.size(), 1);
        const QString binaryPath = pluginDirectory.filePath(libraries.first());
        QFile originalBinary(binaryPath);
        QVERIFY(originalBinary.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = originalBinary.readAll();
        QVERIFY(!originalBytes.isEmpty());
        originalBinary.close();
        const QFile::Permissions originalPermissions = QFileInfo(binaryPath).permissions();
        const QByteArray originalSha = QCryptographicHash::hash(originalBytes, QCryptographicHash::Sha256).toHex();
        QFile metadataFile(info.path);
        QVERIFY(metadataFile.open(QIODevice::ReadOnly));
        const QByteArray metadataSha =
            QCryptographicHash::hash(metadataFile.readAll(), QCryptographicHash::Sha256).toHex();
        metadataFile.close();

        struct BinaryRestore {
            QString path;
            QByteArray bytes;
            QFile::Permissions permissions;

            bool restore() {
                if (path.isEmpty())
                    return true;
                QFile file(path);
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
                    return false;
                file.close();
                if (!QFile::setPermissions(path, permissions))
                    return false;
                path.clear();
                return true;
            }

            ~BinaryRestore() {
                restore();
            }
        } binaryRestore{binaryPath, originalBytes, originalPermissions};

        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy loadedSpy(&manager, &PluginManager::pluginLoaded);
        QSignalSpy unloadedSpy(&manager, &PluginManager::pluginUnloaded);
        QSignalSpy failedSpy(&manager, &PluginManager::pluginLoadFailed);
        QSignalSpy executedSpy(controller.actor(), &AgentActor::toolExecuted);
        QString lastAuditResult;
        QObject auditContext;
        connect(&controller, &AgentController::actionLogEntryAdded, &auditContext,
                [&lastAuditResult](const AgentActionLogEntry& entry) { lastAuditResult = entry.result; });

        const QJsonObject loadPending =
            controller.handleToolCall(QStringLiteral("load_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(loadPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleLoadId = controller.pendingConfirmationId();
        QVERIFY(!staleLoadId.isEmpty());
        const QString loadEffect = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray()
                                       .first()
                                       .toString();
        QVERIFY(loadEffect.startsWith(QStringLiteral("plugin_load:%1 target=").arg(pluginName)));
        QVERIFY(loadEffect.contains(info.path));
        QVERIFY(loadEffect.contains(binaryPath));
        QVERIFY(
            loadEffect.contains(QStringLiteral("metadataSha256=%1").arg(QString::fromLatin1(metadataSha.left(12)))));
        QVERIFY(loadEffect.contains(QStringLiteral("loaded=false")));
        QVERIFY(loadEffect.contains(QString::fromLatin1(originalSha.left(12))));
        QVERIFY(!manager.isPluginLoaded(pluginName));

        QFile changedFixture(QStringLiteral(AGENT_G5_VERSION_FIXTURE));
        QVERIFY(changedFixture.open(QIODevice::ReadOnly));
        const QByteArray changedBytes = changedFixture.readAll();
        changedFixture.close();
        QSaveFile replacement(binaryPath);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        QCOMPARE(replacement.write(changedBytes), changedBytes.size());
        QVERIFY(replacement.commit());
        QFile changedBinary(binaryPath);
        QVERIFY(changedBinary.open(QIODevice::ReadOnly));
        const QByteArray changedSha =
            QCryptographicHash::hash(changedBinary.readAll(), QCryptographicHash::Sha256).toHex();
        changedBinary.close();
        QVERIFY(changedSha != originalSha);
        QVERIFY(controller.confirmPendingTools(staleLoadId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(unloadedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 0);
        QCOMPARE(lastAuditResult, QStringLiteral("expired"));

        QVERIFY(binaryRestore.restore());
        QFile restoredBinary(binaryPath);
        QVERIFY(restoredBinary.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(restoredBinary.readAll(), QCryptographicHash::Sha256).toHex(), originalSha);
        restoredBinary.close();
        controller.clearConversation();
        const QJsonObject validLoad =
            controller.handleToolCall(QStringLiteral("load_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(validLoad.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedLoadId = controller.pendingConfirmationId();
        QVERIFY(!approvedLoadId.isEmpty());
        QVERIFY(approvedLoadId != staleLoadId);
        QVERIFY(controller.confirmPendingTools(approvedLoadId));
        QTRY_COMPARE(executedSpy.count(), 1);
        QCOMPARE(executedSpy.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("loaded"));
        QVERIFY(manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 0);

        const QJsonObject unloadPending =
            controller.handleToolCall(QStringLiteral("unload_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(unloadPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleUnloadId = controller.pendingConfirmationId();
        QVERIFY(!staleUnloadId.isEmpty());
        const QString unloadEffect = controller.pendingConfirmationPayload()
                                         .value(QStringLiteral("context"))
                                         .toObject()
                                         .value(QStringLiteral("sideEffects"))
                                         .toArray()
                                         .first()
                                         .toString();
        QVERIFY(unloadEffect.startsWith(QStringLiteral("plugin_unload:%1 target=").arg(pluginName)));
        QVERIFY(unloadEffect.contains(binaryPath));
        QVERIFY(unloadEffect.contains(QStringLiteral("loaded=true")));
        QVERIFY(unloadEffect.contains(QString::fromLatin1(originalSha.left(12))));
        QString unloadError;
        QVERIFY2(manager.tryUnloadPlugin(pluginName, &unloadError), qPrintable(unloadError));
        QCOMPARE(unloadedSpy.count(), 1);
        QVERIFY(!manager.isPluginLoaded(pluginName));

        QVERIFY(controller.confirmPendingTools(staleUnloadId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 1);
        QCOMPARE(unloadedSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 1);
        QCOMPARE(lastAuditResult, QStringLiteral("expired"));
        QVERIFY(!RunEngine::instance().isBusy());

        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testPluginLifecycleConfirmationRejectsUnreadableMetadataHash() {
        ToolSchema::instance().registerDefaultTools();
        PluginManager& manager = PluginManager::instance();
        const QString pluginName = QStringLiteral("AgentSchemaCacheFixture");
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QVERIFY(!RunEngine::instance().isBusy());
        const QString metadataPath = manager.pluginInfo(pluginName).path;
        QVERIFY(!metadataPath.isEmpty());
        const QString metadataBackup = metadataPath + QStringLiteral(".slice105-backup");
        QVERIFY(!QFileInfo::exists(metadataBackup));
        QVERIFY(QFile::rename(metadataPath, metadataBackup));

        struct MetadataRestore {
            QString path;
            QString backup;
            bool restore() {
                if (path.isEmpty())
                    return true;
                QFile::remove(path);
                const bool restored = QFile::rename(backup, path);
                if (restored)
                    path.clear();
                return restored;
            }
            ~MetadataRestore() {
                restore();
            }
        } metadataRestore{metadataPath, metadataBackup};
        QVERIFY(QFile::link(QFileInfo(metadataPath).absolutePath(), metadataPath));

        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy loadedSpy(&manager, &PluginManager::pluginLoaded);
        QSignalSpy failedSpy(&manager, &PluginManager::pluginLoadFailed);
        QSignalSpy executedSpy(controller.actor(), &AgentActor::toolExecuted);
        QString lastAuditResult;
        QObject auditContext;
        connect(&controller, &AgentController::actionLogEntryAdded, &auditContext,
                [&lastAuditResult](const AgentActionLogEntry& entry) { lastAuditResult = entry.result; });

        const QJsonObject pending =
            controller.handleToolCall(QStringLiteral("load_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        const QString effect = controller.pendingConfirmationPayload()
                                   .value(QStringLiteral("context"))
                                   .toObject()
                                   .value(QStringLiteral("sideEffects"))
                                   .toArray()
                                   .first()
                                   .toString();
        QVERIFY(effect.contains(QStringLiteral("metadataSha256=unavailable")));
        QVERIFY(effect.contains(metadataPath));
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 0);
        QCOMPARE(lastAuditResult, QStringLiteral("expired"));
        QVERIFY(metadataRestore.restore());

#ifdef Q_OS_LINUX
        const QString binaryPath =
            QFileInfo(metadataPath).absoluteDir().filePath(QFileInfo(AGENT_G5_SCHEMA_FIXTURE).fileName());
        QFile kcoreProbe(QStringLiteral("/proc/kcore"));
        bool kcoreReadable = false;
        if (kcoreProbe.open(QIODevice::ReadOnly)) {
            char byte = 0;
            kcoreReadable = kcoreProbe.read(&byte, 1) > 0;
            kcoreProbe.close();
        }
        if (!kcoreReadable) {
            const QString binaryBackup = binaryPath + QStringLiteral(".slice105-backup");
            QVERIFY(!QFileInfo::exists(binaryBackup));
            lastAuditResult.clear();
            controller.clearConversation();
            const QJsonObject pending = controller.handleToolCall(QStringLiteral("load_module_plugin"),
                                                                  {{ QStringLiteral("plugin"),
                                                                     pluginName }});
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QString confirmationId = controller.pendingConfirmationId();
            QVERIFY(!confirmationId.isEmpty());
            QVERIFY(QFile::rename(binaryPath, binaryBackup));
            struct CandidateRestore {
                QString path;
                QString backup;
                bool restore() {
                    if (path.isEmpty())
                        return true;
                    QFile::remove(path);
                    const bool restored = QFile::rename(backup, path);
                    if (restored)
                        path.clear();
                    return restored;
                }
                ~CandidateRestore() {
                    restore();
                }
            } candidateRestore{binaryPath, binaryBackup};
            QVERIFY(QFile::link(QStringLiteral("/proc/kcore"), binaryPath));
            QVERIFY(controller.confirmPendingTools(confirmationId));
            QCOMPARE(controller.state(), AgentController::AgentState::Idle);
            QVERIFY(controller.pendingConfirmationId().isEmpty());
            QVERIFY(!manager.isPluginLoaded(pluginName));
            QCOMPARE(loadedSpy.count(), 0);
            QCOMPARE(failedSpy.count(), 0);
            QCOMPARE(executedSpy.count(), 0);
            QCOMPARE(lastAuditResult, QStringLiteral("expired"));
            QVERIFY(candidateRestore.restore());
        } else {
            qWarning("Skipping /proc/kcore candidate drift regression because its first byte is readable");
        }
#endif
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testPluginLifecycleConfirmationExpiresAfterRegistryRescan() {
        ToolSchema::instance().registerDefaultTools();
        PluginManager& manager = PluginManager::instance();
        const QString pluginName = QStringLiteral("AgentSchemaCacheFixture");
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QVERIFY(!RunEngine::instance().isBusy());
        const PluginInfo originalInfo = manager.pluginInfo(pluginName);
        QVERIFY(!originalInfo.path.isEmpty());

        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy loadedSpy(&manager, &PluginManager::pluginLoaded);
        QSignalSpy failedSpy(&manager, &PluginManager::pluginLoadFailed);
        QSignalSpy executedSpy(controller.actor(), &AgentActor::toolExecuted);
        QString lastAuditResult;
        QObject auditContext;
        connect(&controller, &AgentController::actionLogEntryAdded, &auditContext,
                [&lastAuditResult](const AgentActionLogEntry& entry) { lastAuditResult = entry.result; });

        const QJsonObject pending =
            controller.handleToolCall(QStringLiteral("load_module_plugin"), {{QStringLiteral("plugin"), pluginName}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!staleConfirmationId.isEmpty());
        const QString originalTarget = controller.pendingConfirmationPayload()
                                           .value(QStringLiteral("context"))
                                           .toObject()
                                           .value(QStringLiteral("sideEffects"))
                                           .toArray()
                                           .first()
                                           .toString();
        QVERIFY(originalTarget.contains(originalInfo.path));

        QTemporaryDir replacementRoot;
        QVERIFY(replacementRoot.isValid());
        struct RegistryRestore {
            PluginManager* manager;
            QTemporaryDir* root;
            bool active = false;
            ~RegistryRestore() {
                if (active) {
                    root->remove();
                    manager->scanPlugins();
                }
            }
        } registryRestore{&manager, &replacementRoot};

        QFile schemaMetadata(QStringLiteral(AGENT_G5_FIXTURE_DIR) + QStringLiteral("/schema_metadata.json"));
        QVERIFY(schemaMetadata.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        QJsonDocument metadataDocument = QJsonDocument::fromJson(schemaMetadata.readAll(), &parseError);
        schemaMetadata.close();
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QJsonObject replacementMetadata = metadataDocument.object();
        replacementMetadata.insert(QStringLiteral("id"),
                                   QStringLiteral("com.deeplux.test.agent_schema_cache_replacement"));

        const QString replacementPluginDir = QDir(replacementRoot.path()).filePath(pluginName);
        QVERIFY(QDir().mkpath(replacementPluginDir));
        QFile metadataOutput(QDir(replacementPluginDir).filePath(QStringLiteral("metadata.json")));
        QVERIFY(metadataOutput.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray metadataBytes = QJsonDocument(replacementMetadata).toJson(QJsonDocument::Indented);
        QCOMPARE(metadataOutput.write(metadataBytes), metadataBytes.size());
        metadataOutput.close();
        QVERIFY(QFile::copy(QStringLiteral(AGENT_G5_VERSION_FIXTURE),
                            QDir(replacementPluginDir).filePath(QFileInfo(AGENT_G5_VERSION_FIXTURE).fileName())));

        manager.addPluginPath(QDir(replacementRoot.path()).absolutePath());
        manager.scanPlugins();
        registryRestore.active = true;
        const PluginInfo replacementInfo = manager.pluginInfo(pluginName);
        QCOMPARE(replacementInfo.id, replacementMetadata.value(QStringLiteral("id")).toString());
        QCOMPARE(replacementInfo.path, QDir(replacementPluginDir).filePath(QStringLiteral("metadata.json")));
        QVERIFY(!manager.isPluginLoaded(pluginName));

        QVERIFY(controller.confirmPendingTools(staleConfirmationId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QVERIFY(!manager.isPluginLoaded(pluginName));
        QCOMPARE(loadedSpy.count(), 0);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(executedSpy.count(), 0);
        QCOMPARE(lastAuditResult, QStringLiteral("expired"));
        QVERIFY(!RunEngine::instance().isBusy());

        QVERIFY(replacementRoot.remove());
        manager.scanPlugins();
        registryRestore.active = false;
        const PluginInfo restoredInfo = manager.pluginInfo(pluginName);
        QCOMPARE(restoredInfo.id, originalInfo.id);
        QCOMPARE(restoredInfo.path, originalInfo.path);
        QVERIFY(!manager.isPluginLoaded(pluginName));
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }
};

QTEST_MAIN(TestAgentPluginG5)
#include "test_agent_plugin_g5.moc"
