#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/agent/AgentActor.h>
#include <core/agent/AgentController.h>
#include <core/agent/ToolSchema.h>
#include <core/manager/ConfigManager.h>
#include <memory>
#include <thread>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentActionLogWidget.h>

using namespace DeepLux;

class TestAgentSetTheme : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_appDataDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_appDataDir->isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
    }

    void testSetThemePermissionPersistenceAndFailures() {
        ConfigManager& config = ConfigManager::instance();
        QVERIFY(!config.isInitialized());

        ToolSchema::instance().registerDefaultTools();
        const QJsonArray definitions = ToolSchema::instance().toOpenAIFunctions();
        QJsonObject themeDefinition;
        for (const QJsonValue& value : definitions) {
            const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
            if (function.value(QStringLiteral("name")) == QLatin1String("set_theme")) {
                themeDefinition = function;
                break;
            }
        }
        QVERIFY(!themeDefinition.isEmpty());
        QCOMPARE(themeDefinition.value(QStringLiteral("parameters"))
                     .toObject()
                     .value(QStringLiteral("properties"))
                     .toObject()
                     .value(QStringLiteral("theme"))
                     .toObject()
                     .value(QStringLiteral("enum"))
                     .toArray(),
                 QJsonArray({QStringLiteral("light"), QStringLiteral("dark")}));
        QVERIFY(!themeDefinition.value(QStringLiteral("dangerous")).toBool());

        AgentController& controller = AgentController::instance();
        const QJsonObject noGui = controller.executeDataSourceViewAction(
            QStringLiteral("set_theme"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("dark")}});
        QCOMPARE(noGui.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));

        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        QVERIFY(window.styleSheet().contains(QStringLiteral("#f5f5f5")));

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject denied = controller.handleToolCall(
            QStringLiteral("set_theme"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("dark")}});
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(window.styleSheet().contains(QStringLiteral("#f5f5f5")));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject invalid = controller.actor()->executeTool(
            QStringLiteral("set_theme"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("sepia")}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(window.styleSheet().contains(QStringLiteral("#f5f5f5")));

        const QJsonObject runtimeOnly = controller.handleToolCall(
            QStringLiteral("set_theme"),
            QJsonObject{{QStringLiteral("theme"), QStringLiteral("dark")},
                        {QStringLiteral("unexpected"), QStringLiteral("must-not-be-audited")}});
        QCOMPARE(runtimeOnly.value(QStringLiteral("status")).toString(), QStringLiteral("applied"));
        QCOMPARE(runtimeOnly.value(QStringLiteral("persisted")).toBool(), false);
        QVERIFY(window.styleSheet().contains(QStringLiteral("#1e1e1e")));
        AgentActionLogWidget* actionLog = window.findChild<AgentActionLogWidget*>();
        QVERIFY(actionLog);
        QVERIFY(!actionLog->entries().isEmpty());
        const AgentActionLogEntry runtimeOnlyAudit = actionLog->entries().last();
        QCOMPARE(runtimeOnlyAudit.action, QStringLiteral("set_theme"));
        const QJsonObject runtimeOnlyAuditParams = QJsonDocument::fromJson(runtimeOnlyAudit.params.toUtf8()).object();
        QCOMPARE(runtimeOnlyAuditParams.value(QStringLiteral("theme")).toString(), QStringLiteral("dark"));
        QVERIFY(!runtimeOnlyAuditParams.contains(QStringLiteral("unexpected")));
        QVERIFY(!runtimeOnlyAudit.params.contains(QStringLiteral("must-not-be-audited")));
        const QJsonObject idempotent = controller.handleToolCall(
            QStringLiteral("set_theme"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("dark")}});
        QCOMPARE(idempotent.value(QStringLiteral("theme")).toString(), QStringLiteral("dark"));
        QVERIFY(window.styleSheet().contains(QStringLiteral("#1e1e1e")));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject pending = controller.handleToolCall(
            QStringLiteral("set_theme"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("light")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(window.styleSheet().contains(QStringLiteral("#1e1e1e")));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QVERIFY(window.styleSheet().contains(QStringLiteral("#f5f5f5")));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(config.initialize());
        QSignalSpy savedSpy(&config, &ConfigManager::configSaved);
        const QJsonObject persisted = controller.handleToolCall(
            QStringLiteral("set_theme"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("dark")}});
        QCOMPARE(persisted.value(QStringLiteral("status")).toString(), QStringLiteral("applied"));
        QCOMPARE(persisted.value(QStringLiteral("persisted")).toBool(), true);
        QVERIFY(config.groupBool(QStringLiteral("appearance"), QStringLiteral("darkTheme"), false));

        const QString target = config.configFilePath();
        QFile savedFile(target);
        QVERIFY(savedFile.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        const QJsonDocument savedDocument = QJsonDocument::fromJson(savedFile.readAll(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(savedDocument.object()
                    .value(QStringLiteral("appearance"))
                    .toObject()
                    .value(QStringLiteral("darkTheme"))
                    .toBool());
        savedFile.close();

        const QString realConfigPath = m_appDataDir->filePath(QStringLiteral("real-config.json"));
        QVERIFY(QFile::rename(target, realConfigPath));
        QVERIFY(QFile::link(realConfigPath, target));
        QVERIFY(QFileInfo(target).isSymLink());
        const QJsonObject throughSymlink = controller.handleToolCall(
            QStringLiteral("set_theme"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("light")}});
        QCOMPARE(throughSymlink.value(QStringLiteral("persisted")).toBool(), true);
        QVERIFY(QFileInfo(target).isSymLink());
        QFile symlinkTarget(realConfigPath);
        QVERIFY(symlinkTarget.open(QIODevice::ReadOnly));
        const QJsonDocument symlinkDocument = QJsonDocument::fromJson(symlinkTarget.readAll(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(!symlinkDocument.object()
                     .value(QStringLiteral("appearance"))
                     .toObject()
                     .value(QStringLiteral("darkTheme"))
                     .toBool());

        const QString backup = m_appDataDir->filePath(QStringLiteral("config-backup.json"));
        QVERIFY(QFile::remove(target));
        QVERIFY(QFile::rename(realConfigPath, backup));
        QVERIFY(QDir().mkdir(target));
        const int savesBeforeFailure = savedSpy.size();
        const QJsonObject failedSave = controller.handleToolCall(
            QStringLiteral("set_theme"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("light")}});
        QCOMPARE(failedSave.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CONFIG_SAVE_FAILED"));
        const QJsonObject details = failedSave.value(QStringLiteral("details")).toObject();
        QCOMPARE(details.value(QStringLiteral("applied")).toBool(), true);
        QCOMPARE(details.value(QStringLiteral("theme")).toString(), QStringLiteral("light"));
        QCOMPARE(details.value(QStringLiteral("persisted")).toBool(), false);
        QVERIFY(!config.groupBool(QStringLiteral("appearance"), QStringLiteral("darkTheme"), true));
        QVERIFY(window.styleSheet().contains(QStringLiteral("#f5f5f5")));
        QCOMPARE(savedSpy.size(), savesBeforeFailure);
        QVERIFY(QFileInfo(target).isDir());

        QVERIFY(QDir(target).removeRecursively());
        QVERIFY(QFile::rename(backup, target));
        QVERIFY(config.save());
        QFile finalFile(target);
        QVERIFY(finalFile.open(QIODevice::ReadOnly));
        const QJsonDocument finalDocument = QJsonDocument::fromJson(finalFile.readAll(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(!finalDocument.object()
                     .value(QStringLiteral("appearance"))
                     .toObject()
                     .value(QStringLiteral("darkTheme"))
                     .toBool());

        QJsonObject affinityResult;
        std::thread worker([&]() {
            affinityResult = controller.executeDataSourceViewAction(
                QStringLiteral("set_theme"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("dark")}});
        });
        worker.join();
        QCOMPARE(affinityResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QVERIFY(window.styleSheet().contains(QStringLiteral("#f5f5f5")));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

private:
    std::unique_ptr<QTemporaryDir> m_appDataDir;
};

QTEST_MAIN(TestAgentSetTheme)
#include "test_agent_set_theme.moc"
