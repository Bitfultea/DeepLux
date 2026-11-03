#include <QApplication>
#include <QDialog>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTimer>
#include <QtTest/QtTest>
#include <core/agent/AgentActor.h>
#include <core/agent/AgentController.h>
#include <core/agent/GuiEvent.h>
#include <core/agent/ToolSchema.h>
#include <core/common/UserManager.h>
#include <core/common/VarModel.h>
#include <core/manager/GlobalVarManager.h>
#include <thread>
#include <ui/views/GlobalVarView.h>
#include <ui/views/MainWindow.h>

using namespace DeepLux;

class TestAgentGlobalVariablesHandoff : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
    }

    void init() {
        GlobalVarManager::instance().clearAllVariables();
        AgentController::instance().clearConversation();
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        UserManager::instance().login(QStringLiteral("developer"), QString());
    }

    void cleanup() {
        AgentController::instance().clearConversation();
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Observer);
        GlobalVarManager::instance().clearAllVariables();
        UserManager::instance().login(QStringLiteral("developer"), QString());
    }

    void testRequiresGuiAndRejectsUnexpectedParameters() {
        AgentController& controller = AgentController::instance();
        const QJsonObject noOwner =
            controller.executeDataSourceViewAction(QStringLiteral("open_global_variables_dialog"), {});
        QCOMPARE(noOwner.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));

        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        const QJsonObject invalid = controller.handleToolCall(
            QStringLiteral("open_global_variables_dialog"),
            QJsonObject{{QStringLiteral("variableName"), QStringLiteral("PRIVATE_VARIABLE_MARKER")}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(window.findChildren<GlobalVarView*>(QStringLiteral("AgentGlobalVarDialog")).isEmpty());

        QJsonObject wrongThread;
        std::thread worker([&]() {
            wrongThread = controller.executeDataSourceViewAction(QStringLiteral("open_global_variables_dialog"), {});
        });
        worker.join();
        QCOMPARE(wrongThread.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QVERIFY(window.findChildren<GlobalVarView*>(QStringLiteral("AgentGlobalVarDialog")).isEmpty());
    }

    void testOpensReusesAndClosesWithoutReadingOrChangingVariables() {
        AgentController& controller = AgentController::instance();
        GlobalVarManager& variables = GlobalVarManager::instance();
        const QString privateName = QStringLiteral("PRIVATE_VARIABLE_MARKER_50");
        const QString privateValue = QStringLiteral("PRIVATE_VALUE_MARKER_50");
        auto* model = new VarModel(privateName, VarDataType::String, privateValue);
        model->setExpression(QStringLiteral("PRIVATE_EXPRESSION_MARKER_50"));
        variables.addVariable(model);
        const QJsonObject before = variables.userVariableSnapshotJson(privateName);

        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);

        UserManager::instance().login(QStringLiteral("operator"), QString());
        const QJsonObject denied = controller.handleToolCall(QStringLiteral("open_global_variables_dialog"), {});
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(
            denied.value(QStringLiteral("details")).toObject().value(QStringLiteral("requiredPermission")).toString(),
            QStringLiteral("GlobalVar"));
        QVERIFY(window.findChildren<GlobalVarView*>(QStringLiteral("AgentGlobalVarDialog")).isEmpty());
        UserManager::instance().login(QStringLiteral("developer"), QString());

        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_global_variables_dialog"));
        QCOMPARE(tool.name, QStringLiteral("open_global_variables_dialog"));
        QVERIFY(tool.params.isEmpty());
        QVERIFY(!tool.dangerous);

        for (const AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            const QJsonObject result = controller.handleToolCall(QStringLiteral("open_global_variables_dialog"), {});
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
            QCOMPARE(result.value(QStringLiteral("userActionRequired")).toBool(), true);
        }

        const QList<GlobalVarView*> views = window.findChildren<GlobalVarView*>(QStringLiteral("AgentGlobalVarDialog"));
        QCOMPARE(views.size(), 1);
        GlobalVarView* dialog = views.first();
        QVERIFY(dialog->isVisible());
        QVERIFY(!dialog->isModal());
        QTableWidget* table = dialog->findChild<QTableWidget*>();
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 1)->text(), privateName);
        QCOMPARE(table->item(0, 3)->text(), privateValue);

        QVERIFY(!toolSpy.isEmpty());
        for (const QList<QVariant>& row : toolSpy) {
            const QJsonObject response = row.at(1).toJsonObject();
            const QByteArray serialized = QJsonDocument(response).toJson();
            QVERIFY(!serialized.contains(privateName.toUtf8()));
            QVERIFY(!serialized.contains(privateValue.toUtf8()));
            QVERIFY(!serialized.contains("PRIVATE_EXPRESSION_MARKER_50"));
        }
        QVERIFY(auditSpy.count() >= 3);
        for (const QList<QVariant>& row : auditSpy) {
            const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
            QCOMPARE(entry.action, QStringLiteral("open_global_variables_dialog"));
            QCOMPARE(QJsonDocument::fromJson(entry.params.toUtf8()).object(), QJsonObject());
            const QByteArray serialized = (entry.actor + entry.action + entry.params + entry.result).toUtf8();
            QVERIFY(!serialized.contains(privateName.toUtf8()));
            QVERIFY(!serialized.contains(privateValue.toUtf8()));
            QVERIFY(!serialized.contains("PRIVATE_EXPRESSION_MARKER_50"));
        }
        QCOMPARE(variables.userVariableSnapshotJson(privateName), before);

        dialog->close();
        QVERIFY(!dialog->isVisible());
        QCOMPARE(window.findChildren<GlobalVarView*>(QStringLiteral("AgentGlobalVarDialog")).size(), 1);

        UserManager::instance().login(QStringLiteral("operator"), QString());
        QCOMPARE(table->rowCount(), 0);
        const QJsonObject deniedWhileHidden =
            controller.handleToolCall(QStringLiteral("open_global_variables_dialog"), {});
        QCOMPARE(deniedWhileHidden.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(!dialog->isVisible());

        UserManager::instance().login(QStringLiteral("developer"), QString());
        const QJsonObject reopened = controller.handleToolCall(QStringLiteral("open_global_variables_dialog"), {});
        QCOMPARE(reopened.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));
        QCOMPARE(reopened.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
        QCOMPARE(window.findChildren<GlobalVarView*>(QStringLiteral("AgentGlobalVarDialog")).size(), 1);
        QVERIFY(dialog->isVisible());
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 1)->text(), privateName);
        QCOMPARE(table->item(0, 3)->text(), privateValue);
        QCOMPARE(variables.userVariableSnapshotJson(privateName), before);
    }

    void testReusesExistingModalMenuDialogWithoutChangingModality() {
        AgentController& controller = AgentController::instance();
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        bool modalWasReused = false;
        QJsonObject result;
        QPointer<GlobalVarView> menuDialog;
        QTimer::singleShot(0, &window, [&]() {
            for (GlobalVarView* candidate : window.findChildren<GlobalVarView*>()) {
                if (candidate && candidate->isVisible() && candidate->isModal()) {
                    menuDialog = candidate;
                    break;
                }
            }
            if (!menuDialog)
                return;
            result = controller.handleToolCall(QStringLiteral("open_global_variables_dialog"), {});
            modalWasReused = result.value(QStringLiteral("dialogAction")) == QLatin1String("reused") &&
                             menuDialog->isModal() && window.findChildren<GlobalVarView*>().size() == 1;
            menuDialog->accept();
        });
        QVERIFY(QMetaObject::invokeMethod(&window, "onGlobalVar", Qt::DirectConnection));
        QVERIFY(modalWasReused);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
        QVERIFY(menuDialog.isNull());
    }

    void testMainWindowDestructionClearsDialogAndRejectsFurtherHandoff() {
        AgentController& controller = AgentController::instance();
        QPointer<GlobalVarView> dialog;
        {
            MainWindow window;
            window.show();
            QCoreApplication::processEvents();

            const QJsonObject result = controller.handleToolCall(QStringLiteral("open_global_variables_dialog"), {});
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
            const QList<GlobalVarView*> dialogs =
                window.findChildren<GlobalVarView*>(QStringLiteral("AgentGlobalVarDialog"));
            QCOMPARE(dialogs.size(), 1);
            dialog = dialogs.first();
            QVERIFY(dialog);
        }

        QVERIFY(dialog.isNull());
        const QJsonObject afterWindowDestroyed =
            controller.handleToolCall(QStringLiteral("open_global_variables_dialog"), {});
        QCOMPARE(afterWindowDestroyed.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
    }
};

QTEST_MAIN(TestAgentGlobalVariablesHandoff)
#include "test_agent_globalvars_handoff.moc"
