#include <QApplication>
#include <QDir>
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
#include <core/common/UserManager.h>
#include <core/manager/ProjectManager.h>
#include <thread>
#include <ui/views/MainWindow.h>

using namespace DeepLux;

class RecordingKnowledgeFolderMainWindow : public MainWindow {
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

class TestAgentKnowledgeFolderHandoff : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_appDataDir = new QTemporaryDir();
        QVERIFY(m_appDataDir->isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
        ToolSchema::instance().registerDefaultTools();
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
        delete m_appDataDir;
        m_appDataDir = nullptr;
    }

    void cleanup() {
        AgentController::instance().clearConversation();
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        ProjectManager::instance().closeProject();
        UserManager::instance().login(QStringLiteral("developer"), QString());
    }

    void testSchemaStrictParametersAndObserverDeny() {
        AgentController& controller = AgentController::instance();
        RecordingKnowledgeFolderMainWindow window;
        window.show();
        QCoreApplication::processEvents();
        const ToolDefinition tool =
            ToolSchema::instance().findTool(QStringLiteral("open_user_knowledge_folder_for_user"));
        QCOMPARE(tool.name, QStringLiteral("open_user_knowledge_folder_for_user"));
        QVERIFY(tool.dangerous);
        QVERIFY(tool.params.isEmpty());

        QSignalSpy confirmationSpy(&controller, &AgentController::toolsPendingConfirmation);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject invalid = controller.handleToolCall(
            QStringLiteral("open_user_knowledge_folder_for_user"),
            QJsonObject{{QStringLiteral("path"), m_appDataDir->filePath(QStringLiteral("PRIVATE_KB_PATH"))}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QCOMPARE(confirmationSpy.count(), 0);
        QCOMPARE(auditSpy.count(), 0);
        QCOMPARE(window.openCallCount(), 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject denied = controller.handleToolCall(QStringLiteral("open_user_knowledge_folder_for_user"), {});
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QCOMPARE(window.openCallCount(), 0);
    }

    void testConfirmedOpenInitializesDirectoryAndHidesKnowledgePath() {
        AgentController& controller = AgentController::instance();
        RecordingKnowledgeFolderMainWindow window;
        window.show();
        QCoreApplication::processEvents();
        const QString knowledgeDir = m_appDataDir->filePath(QStringLiteral("knowledge"));
        QDir(knowledgeDir).removeRecursively();
        QVERIFY(!QFileInfo::exists(knowledgeDir));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject pending =
            controller.handleToolCall(QStringLiteral("open_user_knowledge_folder_for_user"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!QFileInfo::exists(knowledgeDir));
        QCOMPARE(window.openCallCount(), 0);
        const QByteArray confirmation = QJsonDocument(controller.pendingConfirmationPayload()).toJson();
        QVERIFY(!confirmation.contains(knowledgeDir.toUtf8()));
        QVERIFY(!confirmation.contains("PRIVATE_KB_PATH"));

        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QVERIFY(QFileInfo(knowledgeDir).isDir());
        QCOMPARE(window.openCallCount(), 1);
        QCOMPARE(window.lastOpenedUrl(), QUrl::fromLocalFile(knowledgeDir));
        QVERIFY(!toolSpy.isEmpty());
        const QJsonObject result = toolSpy.takeLast().at(1).toJsonObject();
        const QJsonObject expectedResult{{QStringLiteral("status"), QStringLiteral("open_requested")}};
        QCOMPARE(result, expectedResult);
        QVERIFY(!QJsonDocument(result).toJson().contains(knowledgeDir.toUtf8()));

        bool successAuditFound = false;
        for (const QList<QVariant>& row : auditSpy) {
            const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
            if (entry.action != QLatin1String("open_user_knowledge_folder_for_user"))
                continue;
            QCOMPARE(QJsonDocument::fromJson(entry.params.toUtf8()).object(), QJsonObject());
            QVERIFY(!entry.undoable);
            const QByteArray audit = (entry.actor + entry.action + entry.params + entry.result).toUtf8();
            QVERIFY(!audit.contains(knowledgeDir.toUtf8()));
            if (entry.result == QLatin1String("open_requested"))
                successAuditFound = true;
        }
        QVERIFY(successAuditFound);
    }

    void testRejectionOwnerReplacementAndPlatformFailure() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QString knowledgeDir = m_appDataDir->filePath(QStringLiteral("knowledge"));
        QDir(knowledgeDir).removeRecursively();

        RecordingKnowledgeFolderMainWindow original;
        original.show();
        QCoreApplication::processEvents();
        const QJsonObject rejectedPending =
            controller.handleToolCall(QStringLiteral("open_user_knowledge_folder_for_user"), {});
        QCOMPARE(rejectedPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(!QFileInfo::exists(knowledgeDir));
        QCOMPARE(original.openCallCount(), 0);

        const QJsonObject projectPending =
            controller.handleToolCall(QStringLiteral("open_user_knowledge_folder_for_user"), {});
        QCOMPARE(projectPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString projectConfirmationId = controller.pendingConfirmationId();
        QVERIFY(ProjectManager::instance().newProject());
        QVERIFY(controller.confirmPendingTools(projectConfirmationId));
        QCOMPARE(original.openCallCount(), 0);
        QVERIFY(!QFileInfo::exists(knowledgeDir));
        ProjectManager::instance().closeProject();

        const QJsonObject ownerPending =
            controller.handleToolCall(QStringLiteral("open_user_knowledge_folder_for_user"), {});
        QCOMPARE(ownerPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString ownerConfirmationId = controller.pendingConfirmationId();
        RecordingKnowledgeFolderMainWindow replacement;
        replacement.show();
        QCoreApplication::processEvents();
        QVERIFY(controller.confirmPendingTools(ownerConfirmationId));
        QCOMPARE(original.openCallCount(), 0);
        QCOMPARE(replacement.openCallCount(), 0);
        QVERIFY(!QFileInfo::exists(knowledgeDir));

        replacement.setOpenAccepted(false);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);
        const QJsonObject failedPending =
            controller.handleToolCall(QStringLiteral("open_user_knowledge_folder_for_user"), {});
        QCOMPARE(failedPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(replacement.openCallCount(), 1);
        QVERIFY(!toolSpy.isEmpty());
        QCOMPARE(toolSpy.takeLast().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("DESKTOP_OPEN_FAILED"));
    }

    void testRequiresGuiAndWrongThreadFailClosed() {
        AgentController& controller = AgentController::instance();
        // No registered MainWindow owner returns REQUIRES_GUI before any confirmation or filesystem access.
        const QJsonObject noOwner =
            controller.executeDataSourceViewAction(QStringLiteral("open_user_knowledge_folder_for_user"), {});
        QCOMPARE(noOwner.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));

        RecordingKnowledgeFolderMainWindow window;
        window.show();
        QCoreApplication::processEvents();
        QJsonObject wrongThread;
        std::thread worker([&]() {
            wrongThread =
                controller.executeDataSourceViewAction(QStringLiteral("open_user_knowledge_folder_for_user"), {});
        });
        worker.join();
        QCOMPARE(wrongThread.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QCOMPARE(window.openCallCount(), 0);
    }

    void testKnowledgeDocumentToolsRequireProductFilePermissions() {
        AgentController& controller = AgentController::instance();
        AgentActor* actor = controller.actor();
        QVERIFY(actor);
        const QString sourcePath = m_appDataDir->filePath(QStringLiteral("source.md"));
        const QByteArray sourceBytes("private source contents\n");
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(sourceBytes), sourceBytes.size());
        source.close();

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QJsonObject imported =
            actor->executeTool(QStringLiteral("add_knowledge_document"), {{QStringLiteral("sourcePath"), sourcePath}});
        QCOMPARE(imported.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString documentId = imported.value(QStringLiteral("documentId")).toString();
        const QString libraryPath = m_appDataDir->filePath(QStringLiteral("knowledge/source.md"));
        QVERIFY(QFileInfo::exists(libraryPath));
        QCOMPARE(QFileInfo(libraryPath).fileName(), QStringLiteral("source.md"));

        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("OpenFile")));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("SaveFile")));
        const QJsonObject listed = controller.handleToolCall(QStringLiteral("list_knowledge_documents"), {});
        QVERIFY(!listed.contains(QStringLiteral("error")));
        const QJsonObject preview = controller.handleToolCall(QStringLiteral("preview_knowledge_document"),
                                                              {{QStringLiteral("documentId"), documentId}});
        QCOMPARE(preview.value(QStringLiteral("content")).toString(), QString::fromUtf8(sourceBytes));
        const QJsonObject documentation =
            controller.handleToolCall(QStringLiteral("read_documentation"), {{QStringLiteral("topic"), "GrabImage"}});
        QVERIFY(!documentation.contains(QStringLiteral("error")));

        const auto assertDenied = [&controller, &sourceBytes](const QString& tool, const QJsonObject& params,
                                                              const QString& permission) {
            const QJsonObject result = controller.handleToolCall(tool, params);
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
            QCOMPARE(result.value(QStringLiteral("retryable")).toBool(), false);
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("denied"));
            QCOMPARE(result.value(QStringLiteral("details"))
                         .toObject()
                         .value(QStringLiteral("requiredPermission"))
                         .toString(),
                     permission);
            QVERIFY(!QJsonDocument(result).toJson().contains(sourceBytes));
        };
        assertDenied(QStringLiteral("add_knowledge_document"), {{QStringLiteral("sourcePath"), sourcePath}},
                     QStringLiteral("SaveFile"));
        assertDenied(QStringLiteral("remove_knowledge_document"), {{QStringLiteral("documentId"), documentId}},
                     QStringLiteral("SaveFile"));
        QCOMPARE(QFile(libraryPath).size(), sourceBytes.size());
        QFile sourceRead(sourcePath);
        QVERIFY(sourceRead.open(QIODevice::ReadOnly));
        QCOMPARE(sourceRead.readAll(), sourceBytes);

        QVERIFY(UserManager::instance().login(QStringLiteral("administrator"), QString()));
        const QJsonObject adminList = controller.handleToolCall(QStringLiteral("list_knowledge_documents"), {});
        QVERIFY(!adminList.contains(QStringLiteral("error")));
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("SaveFile")));
        const QString adminSourcePath = m_appDataDir->filePath(QStringLiteral("admin.txt"));
        QFile adminSource(adminSourcePath);
        QVERIFY(adminSource.open(QIODevice::WriteOnly));
        QCOMPARE(adminSource.write("admin document\n"), qint64(15));
        adminSource.close();
        const QJsonObject adminImport = actor->executeTool(QStringLiteral("add_knowledge_document"),
                                                           {{QStringLiteral("sourcePath"), adminSourcePath}});
        QCOMPARE(adminImport.value(QStringLiteral("status")).toString(), QStringLiteral("added"));

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("OpenFile")));
        QVERIFY(UserManager::instance().hasPermission(QStringLiteral("SaveFile")));
        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        QCOMPARE(UserManager::instance().currentUser().type, UserType::Guest);
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("OpenFile")));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("SaveFile")));
        assertDenied(QStringLiteral("preview_knowledge_document"), {{QStringLiteral("documentId"), documentId}},
                     QStringLiteral("OpenFile"));
        assertDenied(QStringLiteral("read_documentation"), {{QStringLiteral("topic"), "GrabImage"}},
                     QStringLiteral("OpenFile"));
        assertDenied(QStringLiteral("remove_knowledge_document"), {{QStringLiteral("documentId"), documentId}},
                     QStringLiteral("SaveFile"));
        UserManager::instance().logout();
        assertDenied(QStringLiteral("list_knowledge_documents"), {}, QStringLiteral("OpenFile"));
        QVERIFY(QFileInfo::exists(libraryPath));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QString pendingSourcePath = m_appDataDir->filePath(QStringLiteral("pending.md"));
        QFile pendingSource(pendingSourcePath);
        QVERIFY(pendingSource.open(QIODevice::WriteOnly));
        const QByteArray pendingBytes("pending source\n");
        QCOMPARE(pendingSource.write(pendingBytes), pendingBytes.size());
        pendingSource.close();
        QSignalSpy responseSpy(&controller, &AgentController::llmResponseReceived);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject pending = controller.handleToolCall(QStringLiteral("add_knowledge_document"),
                                                              {{QStringLiteral("sourcePath"), pendingSourcePath}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QVERIFY(!QFileInfo::exists(m_appDataDir->filePath(QStringLiteral("knowledge/pending.md"))));
        QVERIFY(responseSpy.size() == 1);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.size(), 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
        QCOMPARE(controller.conversationHistorySize(), 0);
        QFile pendingSourceAfter(pendingSourcePath);
        QVERIFY(pendingSourceAfter.open(QIODevice::ReadOnly));
        QCOMPARE(pendingSourceAfter.readAll(), pendingBytes);

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QJsonObject removePending = controller.handleToolCall(QStringLiteral("remove_knowledge_document"),
                                                                    {{QStringLiteral("documentId"), documentId}});
        QCOMPARE(removePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString removeConfirmationId = controller.pendingConfirmationId();
        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(controller.confirmPendingTools(removeConfirmationId));
        QVERIFY(QFileInfo::exists(libraryPath));
        QCOMPARE(QFile(libraryPath).size(), sourceBytes.size());
        QCOMPARE(responseSpy.size(), 2);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.size(), 2);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
        QCOMPARE(controller.conversationHistorySize(), 0);

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QJsonObject logoutRemovePending = controller.handleToolCall(QStringLiteral("remove_knowledge_document"),
                                                                          {{QStringLiteral("documentId"), documentId}});
        QCOMPARE(logoutRemovePending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        const QString logoutConfirmationId = controller.pendingConfirmationId();
        UserManager::instance().logout();
        QVERIFY(controller.confirmPendingTools(logoutConfirmationId));
        QVERIFY(QFileInfo::exists(libraryPath));
        QFile libraryAfterLogout(libraryPath);
        QVERIFY(libraryAfterLogout.open(QIODevice::ReadOnly));
        QCOMPARE(libraryAfterLogout.readAll(), sourceBytes);
        QCOMPARE(responseSpy.size(), 3);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.size(), 3);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
        QCOMPARE(controller.conversationHistorySize(), 0);
    }

private:
    QTemporaryDir* m_appDataDir = nullptr;
};

QTEST_MAIN(TestAgentKnowledgeFolderHandoff)
#include "test_agent_knowledge_folder_handoff.moc"
