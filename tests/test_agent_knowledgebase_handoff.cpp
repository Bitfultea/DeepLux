#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <core/agent/AgentController.h>
#include <core/agent/GuiEvent.h>
#include <core/agent/ToolSchema.h>
#include <core/manager/ConfigManager.h>
#include <thread>
#include <ui/dialogs/AgentSettingsDialog.h>
#include <ui/dialogs/KnowledgeBaseDialog.h>
#include <ui/views/MainWindow.h>

using namespace DeepLux;

class TestAgentKnowledgeBaseHandoff : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_appDataDir = new QTemporaryDir();
        QVERIFY(m_appDataDir->isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
        QVERIFY(ConfigManager::instance().initialize());
        ToolSchema::instance().registerDefaultTools();
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
        delete m_appDataDir;
        m_appDataDir = nullptr;
    }

    void testRequiresGui() {
        const QJsonObject result =
            AgentController::instance().handleToolCall(QStringLiteral("open_knowledge_base_dialog"), {});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
    }

    void testNavigationLifecycleAndModalSettingsReuse() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        const QJsonObject invalid = controller.executeDataSourceViewAction(
            QStringLiteral("open_knowledge_base_dialog"), QJsonObject{{QStringLiteral("unexpected"), true}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(window.findChildren<KnowledgeBaseDialog*>(QStringLiteral("KnowledgeBaseDialog")).isEmpty());

        QJsonObject wrongThread;
        std::thread worker([&]() {
            wrongThread = controller.executeDataSourceViewAction(QStringLiteral("open_knowledge_base_dialog"), {});
        });
        worker.join();
        QCOMPARE(wrongThread.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QVERIFY(window.findChildren<KnowledgeBaseDialog*>(QStringLiteral("KnowledgeBaseDialog")).isEmpty());

        for (const AgentController::PermissionLevel level :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(level);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_knowledge_base_dialog")),
                     AgentController::ToolGate::Allow);
        }
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);

        const QString knowledgeDir = m_appDataDir->filePath(QStringLiteral("knowledge"));
        QDir(knowledgeDir).removeRecursively();
        QVERIFY(!QFileInfo::exists(knowledgeDir));
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject opened = controller.handleToolCall(QStringLiteral("open_knowledge_base_dialog"), {});
        QCOMPARE(opened.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
        QCOMPARE(opened.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        QVERIFY(opened.value(QStringLiteral("userActionRequired")).toBool());
        QCOMPARE(opened.size(), 3);
        QVERIFY(QFileInfo(knowledgeDir).isDir());
        QVERIFY(QDir(knowledgeDir).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
        const QByteArray openedBytes = QJsonDocument(opened).toJson(QJsonDocument::Compact);
        QVERIFY(!openedBytes.contains("DEEPLUX.md"));
        QVERIFY(!openedBytes.contains(QFile::encodeName(knowledgeDir)));

        KnowledgeBaseDialog* first = window.findChild<KnowledgeBaseDialog*>(QStringLiteral("KnowledgeBaseDialog"));
        QVERIFY(first);
        QVERIFY(first->isVisible());
        QVERIFY(!first->isModal());
        QListWidget* documents = first->findChild<QListWidget*>(QStringLiteral("KnowledgeDocList"));
        QVERIFY(documents);
        QVERIFY(documents->count() > 0);
        for (int row = 0; row < documents->count(); ++row) {
            QListWidgetItem* item = documents->item(row);
            QVERIFY(item->data(Qt::UserRole).toString().startsWith(QStringLiteral("builtin:")));
            QVERIFY(!item->data(Qt::UserRole + 1).toBool());
        }
        QPointer<KnowledgeBaseDialog> firstGuard(first);

        const QByteArray syntheticDocument = "# Private test fixture\nSLICE44_SECRET_MARKER_MUST_NOT_ESCAPE\n";
        const QString syntheticDocumentPath = QDir(knowledgeDir).filePath(QStringLiteral("Slice44Private.md"));
        QFile syntheticDocumentFile(syntheticDocumentPath);
        QVERIFY(syntheticDocumentFile.open(QIODevice::WriteOnly));
        QCOMPARE(syntheticDocumentFile.write(syntheticDocument), qint64(syntheticDocument.size()));
        syntheticDocumentFile.close();

        const QJsonObject reused = controller.handleToolCall(QStringLiteral("open_knowledge_base_dialog"), {});
        QCOMPARE(reused.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));
        QCOMPARE(window.findChildren<KnowledgeBaseDialog*>(QStringLiteral("KnowledgeBaseDialog")).size(), 1);

        first->close();
        const QJsonObject reopened = controller.handleToolCall(QStringLiteral("open_knowledge_base_dialog"), {});
        QCOMPARE(reopened.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        KnowledgeBaseDialog* active = nullptr;
        for (KnowledgeBaseDialog* candidate : window.findChildren<KnowledgeBaseDialog*>()) {
            if (candidate->isVisible())
                active = candidate;
        }
        QVERIFY(active);
        QVERIFY(active != first);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(firstGuard.isNull());
        QCOMPARE(window.findChildren<KnowledgeBaseDialog*>(QStringLiteral("KnowledgeBaseDialog")).size(), 1);
        const QJsonObject reusedAfterOldDelete =
            controller.handleToolCall(QStringLiteral("open_knowledge_base_dialog"), {});
        QCOMPARE(reusedAfterOldDelete.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));

        active->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(window.findChildren<KnowledgeBaseDialog*>(QStringLiteral("KnowledgeBaseDialog")).isEmpty());

        QCOMPARE(controller.handleToolCall(QStringLiteral("open_agent_settings_dialog"), {})
                     .value(QStringLiteral("dialogAction"))
                     .toString(),
                 QStringLiteral("opened"));
        AgentSettingsDialog* settings = window.findChild<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog"));
        QVERIFY(settings);
        auto* knowledgeButton = settings->findChild<QPushButton*>(QStringLiteral("AgentSettingsKnowledgeButton"));
        QVERIFY(knowledgeButton);
        bool modalWasReused = false;
        QJsonObject modalToolResult;
        QTimer::singleShot(0, settings, [&]() {
            QTimer::singleShot(0, settings, [&]() {
                auto* modal = qobject_cast<KnowledgeBaseDialog*>(QApplication::activeModalWidget());
                if (!modal || !modal->isVisible()) {
                    QTest::qFail("Settings Knowledge Base action did not open a visible modal dialog", __FILE__,
                                 __LINE__);
                    return;
                }
                const QString windowTitle = modal->windowTitle();
                modalToolResult = controller.handleToolCall(QStringLiteral("open_knowledge_base_dialog"), {});
                modalWasReused =
                    modalToolResult.value(QStringLiteral("dialogAction")).toString() == QStringLiteral("reused") &&
                    modal->isModal() && modal->windowTitle() == windowTitle &&
                    window.findChildren<KnowledgeBaseDialog*>(QStringLiteral("KnowledgeBaseDialog")).size() == 1;
                if (!modalWasReused)
                    QTest::qFail("Agent did not reuse the existing modal Knowledge Base dialog", __FILE__, __LINE__);
                modal->accept();
            });
            knowledgeButton->click();
        });
        QCoreApplication::processEvents();
        QVERIFY(modalWasReused);
        settings->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        QVERIFY(auditSpy.count() >= 5);
        const AgentActionLogEntry firstAudit = qvariant_cast<AgentActionLogEntry>(auditSpy.first().at(0));
        QCOMPARE(firstAudit.action, QStringLiteral("open_knowledge_base_dialog"));
        QVERIFY(QJsonDocument::fromJson(firstAudit.params.toUtf8()).object().isEmpty());
        for (const QList<QVariant>& row : auditSpy) {
            const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
            const QByteArray auditText = (entry.actor + entry.action + entry.params + entry.result).toUtf8();
            QVERIFY(!auditText.contains("DEEPLUX.md"));
            QVERIFY(!auditText.contains(QFile::encodeName(knowledgeDir)));
            QVERIFY(!auditText.contains("SLICE44_SECRET_MARKER_MUST_NOT_ESCAPE"));
            QVERIFY(!auditText.contains("Slice44Private.md"));
        }
        const QStringList userFiles = QDir(knowledgeDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
        QCOMPARE(userFiles, QStringList{QStringLiteral("Slice44Private.md")});
        QVERIFY(syntheticDocumentFile.open(QIODevice::ReadOnly));
        QCOMPARE(syntheticDocumentFile.readAll(), syntheticDocument);
        syntheticDocumentFile.close();
        for (const QJsonObject& result : {opened, reused, reopened, reusedAfterOldDelete, modalToolResult}) {
            const QByteArray resultBytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
            QVERIFY(!resultBytes.contains("SLICE44_SECRET_MARKER_MUST_NOT_ESCAPE"));
            QVERIFY(!resultBytes.contains("Slice44Private.md"));
            QVERIFY(!resultBytes.contains(QFile::encodeName(syntheticDocumentPath)));
        }
    }

    void testOwnerDestructionDeletesKnowledgeBaseWindow() {
        AgentController& controller = AgentController::instance();
        auto* window = new MainWindow();
        window->show();
        QCoreApplication::processEvents();
        const QJsonObject result = controller.handleToolCall(QStringLiteral("open_knowledge_base_dialog"), {});
        QCOMPARE(result.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        QPointer<KnowledgeBaseDialog> dialog(
            window->findChild<KnowledgeBaseDialog*>(QStringLiteral("KnowledgeBaseDialog")));
        QVERIFY(dialog);
        delete window;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(dialog.isNull());
    }

private:
    QTemporaryDir* m_appDataDir = nullptr;
};

QTEST_MAIN(TestAgentKnowledgeBaseHandoff)
#include "test_agent_knowledgebase_handoff.moc"
