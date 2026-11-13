#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QJsonDocument>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <core/agent/AgentController.h>
#include <core/agent/GuiEvent.h>
#include <core/agent/ToolSchema.h>
#include <thread>
#include <ui/views/MainWindow.h>

using namespace DeepLux;

class TestAgentAboutHandoff : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_appDataDir = new QTemporaryDir();
        QVERIFY(m_appDataDir->isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
        ToolSchema::instance().registerDefaultTools();
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
        delete m_appDataDir;
        m_appDataDir = nullptr;
    }

    void testRequiresGui() {
        const QJsonObject result = AgentController::instance().handleToolCall(QStringLiteral("open_about_dialog"), {});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
    }

    void testNavigationLifecycleAndModalMenuReuse() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        const QJsonObject invalid = controller.executeDataSourceViewAction(
            QStringLiteral("open_about_dialog"), QJsonObject{{QStringLiteral("unexpected"), true}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(window.findChildren<QMessageBox*>(QStringLiteral("AgentAboutDialog")).isEmpty());

        QJsonObject wrongThread;
        std::thread worker(
            [&]() { wrongThread = controller.executeDataSourceViewAction(QStringLiteral("open_about_dialog"), {}); });
        worker.join();
        QCOMPARE(wrongThread.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QVERIFY(window.findChildren<QMessageBox*>(QStringLiteral("AgentAboutDialog")).isEmpty());

        for (const AgentController::PermissionLevel level :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(level);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_about_dialog")),
                     AgentController::ToolGate::Allow);
        }
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);

        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject opened = controller.handleToolCall(QStringLiteral("open_about_dialog"), {});
        QCOMPARE(opened.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
        QCOMPARE(opened.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        QVERIFY(opened.value(QStringLiteral("userActionRequired")).toBool());
        QCOMPARE(opened.size(), 3);
        QVERIFY(!QJsonDocument(opened).toJson(QJsonDocument::Compact).contains("DeepLux Vision"));

        QMessageBox* first = window.findChild<QMessageBox*>(QStringLiteral("AgentAboutDialog"));
        QVERIFY(first);
        QVERIFY(first->isVisible());
        QVERIFY(!first->isModal());
        QCOMPARE(first->windowTitle(), QStringLiteral("关于 DeepLux"));
        QVERIFY(first->text().contains(QStringLiteral("DeepLux Vision 1.0.0")));
        QPointer<QMessageBox> firstGuard(first);

        const QJsonObject reused = controller.handleToolCall(QStringLiteral("open_about_dialog"), {});
        QCOMPARE(reused.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));
        QCOMPARE(window.findChildren<QMessageBox*>(QStringLiteral("AgentAboutDialog")).size(), 1);

        first->close();
        const QJsonObject reopened = controller.handleToolCall(QStringLiteral("open_about_dialog"), {});
        QCOMPARE(reopened.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        QMessageBox* active = nullptr;
        for (QMessageBox* candidate : window.findChildren<QMessageBox*>(QStringLiteral("AgentAboutDialog"))) {
            if (candidate->isVisible())
                active = candidate;
        }
        QVERIFY(active);
        QVERIFY(active != first);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(firstGuard.isNull());
        QCOMPARE(window.findChildren<QMessageBox*>(QStringLiteral("AgentAboutDialog")).size(), 1);
        const QJsonObject reusedAfterOldDelete = controller.handleToolCall(QStringLiteral("open_about_dialog"), {});
        QCOMPARE(reusedAfterOldDelete.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));

        active->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(window.findChildren<QMessageBox*>(QStringLiteral("AgentAboutDialog")).isEmpty());

        QAction* aboutAction = window.findChild<QAction*>(QStringLiteral("AboutMenuAction"));
        QVERIFY(aboutAction);
        bool menuModalReused = false;
        QTimer::singleShot(0, &window, [&]() {
            QTimer::singleShot(0, &window, [&]() {
                auto* modal = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (!modal || !modal->isVisible()) {
                    QTest::qFail("Help menu About message box was not modal and visible", __FILE__, __LINE__);
                    return;
                }
                const QString title = modal->windowTitle();
                const QString text = modal->text();
                const QJsonObject result = controller.handleToolCall(QStringLiteral("open_about_dialog"), {});
                const QList<QMessageBox*> visibleAboutDialogs = window.findChildren<QMessageBox*>();
                menuModalReused = result.value(QStringLiteral("dialogAction")).toString() == QStringLiteral("reused") &&
                                  modal->isModal() && modal->windowTitle() == title && modal->text() == text &&
                                  visibleAboutDialogs.size() == 1;
                if (!menuModalReused)
                    QTest::qFail("Agent did not reuse the active menu About dialog", __FILE__, __LINE__);
                modal->accept();
            });
            aboutAction->trigger();
        });
        QCoreApplication::processEvents();
        QVERIFY(menuModalReused);
        QVERIFY(window.findChildren<QMessageBox*>(QStringLiteral("AgentAboutDialog")).isEmpty());

        QVERIFY(auditSpy.count() >= 5);
        const AgentActionLogEntry firstAudit = qvariant_cast<AgentActionLogEntry>(auditSpy.first().at(0));
        QCOMPARE(firstAudit.action, QStringLiteral("open_about_dialog"));
        QVERIFY(QJsonDocument::fromJson(firstAudit.params.toUtf8()).object().isEmpty());
        for (const QList<QVariant>& row : auditSpy) {
            const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
            const QByteArray auditText = (entry.actor + entry.action + entry.params + entry.result).toUtf8();
            QVERIFY(!auditText.contains("DeepLux Vision"));
            QVERIFY(!auditText.contains("2024 DeepLux"));
        }
    }

    void testOwnerDestructionDeletesAboutWindow() {
        AgentController& controller = AgentController::instance();
        auto* window = new MainWindow();
        window->show();
        QCoreApplication::processEvents();

        const QJsonObject result = controller.handleToolCall(QStringLiteral("open_about_dialog"), {});
        QCOMPARE(result.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        QPointer<QMessageBox> dialog(window->findChild<QMessageBox*>(QStringLiteral("AgentAboutDialog")));
        QVERIFY(dialog);

        delete window;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(dialog.isNull());
    }

private:
    QTemporaryDir* m_appDataDir = nullptr;
};

QTEST_MAIN(TestAgentAboutHandoff)
#include "test_agent_about_handoff.moc"
