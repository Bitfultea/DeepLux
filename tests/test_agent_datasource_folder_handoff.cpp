#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/agent/AgentActor.h>
#include <core/agent/AgentController.h>
#include <core/agent/GuiEvent.h>
#include <core/agent/ToolSchema.h>
#include <core/manager/ProjectManager.h>
#include <core/model/DataSource.h>
#include <core/model/Project.h>
#include <thread>
#include <ui/views/MainWindow.h>

using namespace DeepLux;

class RecordingFolderMainWindow : public MainWindow {
public:
    using MainWindow::MainWindow;

    int openCallCount() const {
        return m_openCallCount;
    }
    QUrl lastOpenedUrl() const {
        return m_lastOpenedUrl;
    }
    void setOpenAccepted(bool accepted) {
        m_openAccepted = accepted;
    }

protected:
    bool openDesktopUrl(const QUrl& url) const override {
        ++m_openCallCount;
        m_lastOpenedUrl = url;
        return m_openAccepted;
    }

private:
    mutable int m_openCallCount = 0;
    mutable QUrl m_lastOpenedUrl;
    bool m_openAccepted = true;
};

class AgentControllerStateRestore {
public:
    explicit AgentControllerStateRestore(AgentController& controller)
        : m_controller(controller), m_permission(controller.permissionLevel()) {}
    ~AgentControllerStateRestore() {
        m_controller.setPermissionLevel(m_permission);
        m_controller.clearConversation();
    }

private:
    AgentController& m_controller;
    AgentController::PermissionLevel m_permission;
};

class TestAgentDataSourceFolderHandoff : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
        ProjectManager::instance().closeProject();
    }

    void cleanup() {
        AgentController::instance().clearConversation();
        ProjectManager::instance().closeProject();
    }

    void testSchemaPermissionAndStrictParameters() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        RecordingFolderMainWindow window;
        window.show();
        QCoreApplication::processEvents();

        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_data_source_folder_for_user"));
        QCOMPARE(tool.name, QStringLiteral("open_data_source_folder_for_user"));
        QVERIFY(tool.dangerous);
        QCOMPARE(tool.params.size(), 1);
        QCOMPARE(tool.params.first().name, QStringLiteral("dataSourceId"));

        QSignalSpy confirmationSpy(&controller, &AgentController::toolsPendingConfirmation);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject invalid = controller.handleToolCall(
            QStringLiteral("open_data_source_folder_for_user"),
            QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("source")},
                        {QStringLiteral("path"), QStringLiteral("/tmp/SLICE47_PRIVATE_PATH")}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QCOMPARE(confirmationSpy.count(), 0);
        QCOMPARE(auditSpy.count(), 0);
        QCOMPARE(window.openCallCount(), 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject denied =
            controller.handleToolCall(QStringLiteral("open_data_source_folder_for_user"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("source")}});
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QCOMPARE(window.openCallCount(), 0);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
    }

    void testConfirmedRequestOpensExactParentAndKeepsPathPrivate() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString sourcePath = files.filePath(QStringLiteral("SLICE47_PRIVATE_SOURCE.bin"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        QVERIFY(sourceFile.write("fixture bytes") > 0);
        sourceFile.close();

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        DataSource source;
        source.id = QStringLiteral("folder-source-id");
        source.name = QStringLiteral("SLICE47_PRIVATE_NAME.bin");
        source.filePath = sourcePath;
        source.type = QStringLiteral("image");
        project->addDataSource(source);
        const QJsonObject projectBefore = project->toJson();

        RecordingFolderMainWindow window;
        window.show();
        QCoreApplication::processEvents();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);

        const QJsonObject rejectedPending =
            controller.handleToolCall(QStringLiteral("open_data_source_folder_for_user"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), source.id}});
        QCOMPARE(rejectedPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(window.openCallCount(), 0);

        const QJsonObject pending = controller.handleToolCall(QStringLiteral("open_data_source_folder_for_user"),
                                                              QJsonObject{{QStringLiteral("dataSourceId"), source.id}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(window.openCallCount() == 0);
        const QByteArray confirmation = QJsonDocument(controller.pendingConfirmationPayload()).toJson();
        QVERIFY(!confirmation.contains("SLICE47_PRIVATE_SOURCE.bin"));
        QVERIFY(!confirmation.contains(files.path().toUtf8()));

        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(window.openCallCount(), 1, 5000);
        QCOMPARE(window.openCallCount(), 1);
        QCOMPARE(window.lastOpenedUrl(), QUrl::fromLocalFile(files.path()));
        QVERIFY(!toolSpy.isEmpty());
        const QJsonObject result = toolSpy.takeLast().at(1).toJsonObject();
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("open_requested"));
        QVERIFY(!QJsonDocument(result).toJson().contains("SLICE47_PRIVATE_SOURCE.bin"));
        QVERIFY(!QJsonDocument(result).toJson().contains(files.path().toUtf8()));
        QCOMPARE(project->toJson(), projectBefore);

        bool safeAuditFound = false;
        bool rejectedAuditFound = false;
        for (const QList<QVariant>& row : auditSpy) {
            const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
            if (entry.action != QLatin1String("open_data_source_folder_for_user"))
                continue;
            if (entry.result == QLatin1String("rejected"))
                rejectedAuditFound = true;
            else {
                QCOMPARE(entry.result, QStringLiteral("open_requested"));
                safeAuditFound = true;
            }
            QVERIFY(!entry.undoable);
            const QJsonObject expectedAudit{{QStringLiteral("dataSourceId"), source.id}};
            QCOMPARE(QJsonDocument::fromJson(entry.params.toUtf8()).object(), expectedAudit);
            const QByteArray audit = (entry.actor + entry.action + entry.params + entry.result).toUtf8();
            QVERIFY(!audit.contains("SLICE47_PRIVATE_SOURCE.bin"));
            QVERIFY(!audit.contains(files.path().toUtf8()));
        }
        QVERIFY(safeAuditFound);
        QVERIFY(rejectedAuditFound);

        const int openCallsBeforeToolbar = window.openCallCount();
        QVERIFY(QMetaObject::invokeMethod(&window, "onShowDataSourceInFolder", Qt::DirectConnection,
                                          Q_ARG(QString, source.id)));
        QCOMPARE(window.openCallCount(), openCallsBeforeToolbar + 1);
        QCOMPARE(window.lastOpenedUrl(), QUrl::fromLocalFile(files.path()));
    }

    void testConfirmationExpiresOnOwnerOrCollectionReplacement() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        QTemporaryDir files;
        QVERIFY(files.isValid());
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        auto addSource = [project](const QString& id, const QString& name) {
            DataSource source;
            source.id = id;
            source.name = name;
            source.filePath = QStringLiteral("/virtual/") + name;
            source.type = QStringLiteral("image");
            project->addDataSource(source);
        };
        addSource(QStringLiteral("folder-source"), QStringLiteral("source.png"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        RecordingFolderMainWindow originalWindow;
        originalWindow.show();
        QCoreApplication::processEvents();
        const QJsonObject pendingOwner =
            controller.handleToolCall(QStringLiteral("open_data_source_folder_for_user"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("folder-source")}});
        QCOMPARE(pendingOwner.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString ownerConfirmationId = controller.pendingConfirmationId();

        RecordingFolderMainWindow replacementWindow;
        replacementWindow.show();
        QCoreApplication::processEvents();
        QVERIFY(controller.confirmPendingTools(ownerConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(originalWindow.openCallCount(), 0);
        QCOMPARE(replacementWindow.openCallCount(), 0);

        const QJsonObject pendingGeneration =
            controller.handleToolCall(QStringLiteral("open_data_source_folder_for_user"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("folder-source")}});
        QCOMPARE(pendingGeneration.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString generationConfirmationId = controller.pendingConfirmationId();
        addSource(QStringLiteral("another-source"), QStringLiteral("other.png"));
        QVERIFY(controller.confirmPendingTools(generationConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(replacementWindow.openCallCount(), 0);
    }

    void testAmbiguousMissingPathAndPlatformFailureAreStructured() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        auto addSource = [project](const QString& id, const QString& path) {
            DataSource source;
            source.id = id;
            source.name = id;
            source.filePath = path;
            source.type = QStringLiteral("image");
            project->addDataSource(source);
        };
        addSource(QStringLiteral("duplicate"), QStringLiteral("/virtual/first.png"));
        addSource(QStringLiteral("duplicate"), QStringLiteral("/virtual/second.png"));
        addSource(QStringLiteral("empty-path"), QString());
        addSource(QStringLiteral("unique"), QStringLiteral("/virtual/unique.png"));

        RecordingFolderMainWindow window;
        window.show();
        QCoreApplication::processEvents();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);

        auto confirm = [&controller](const QString& id) {
            const QJsonObject pending = controller.handleToolCall(QStringLiteral("open_data_source_folder_for_user"),
                                                                  QJsonObject{{QStringLiteral("dataSourceId"), id}});
            if (pending.value(QStringLiteral("status")) != QLatin1String("pending_confirmation"))
                return false;
            return controller.confirmPendingTools(controller.pendingConfirmationId());
        };

        QVERIFY(confirm(QStringLiteral("duplicate")));
        QTRY_COMPARE_WITH_TIMEOUT(toolSpy.count(), 1, 5000);
        QCOMPARE(toolSpy.takeLast().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));
        QCOMPARE(window.openCallCount(), 0);
        QVERIFY(confirm(QStringLiteral("empty-path")));
        QTRY_COMPARE_WITH_TIMEOUT(toolSpy.count(), 1, 5000);
        QCOMPARE(toolSpy.takeLast().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("DATA_SOURCE_PATH_UNAVAILABLE"));
        QCOMPARE(window.openCallCount(), 0);

        QVERIFY(confirm(QStringLiteral("unknown-source")));
        QTRY_COMPARE_WITH_TIMEOUT(toolSpy.count(), 1, 5000);
        QCOMPARE(toolSpy.takeLast().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("DATA_SOURCE_NOT_FOUND"));
        QCOMPARE(window.openCallCount(), 0);

        window.setOpenAccepted(false);
        QVERIFY(confirm(QStringLiteral("unique")));
        QTRY_COMPARE_WITH_TIMEOUT(toolSpy.count(), 1, 5000);
        QCOMPARE(window.openCallCount(), 1);
        QCOMPARE(toolSpy.takeLast().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("DESKTOP_OPEN_FAILED"));
    }

    void testNoProjectInvalidIdAndWrongThreadFailClosed() {
        RecordingFolderMainWindow window;
        window.show();
        QCoreApplication::processEvents();

        AgentController& controller = AgentController::instance();
        const QJsonObject noProject = controller.executeDataSourceViewAction(
            QStringLiteral("open_data_source_folder_for_user"),
            QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("source")}});
        QCOMPARE(noProject.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_PROJECT"));
        const QJsonObject invalid =
            controller.executeDataSourceViewAction(QStringLiteral("open_data_source_folder_for_user"),
                                                   QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral(" ")}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QCOMPARE(window.openCallCount(), 0);

        QJsonObject wrongThread;
        std::thread worker([&controller, &wrongThread]() {
            wrongThread = controller.executeDataSourceViewAction(
                QStringLiteral("open_data_source_folder_for_user"),
                QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("source")}});
        });
        worker.join();
        QCOMPARE(wrongThread.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QCOMPARE(window.openCallCount(), 0);
    }
};

QTEST_MAIN(TestAgentDataSourceFolderHandoff)

#include "test_agent_datasource_folder_handoff.moc"
