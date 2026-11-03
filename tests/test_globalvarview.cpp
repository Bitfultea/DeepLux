#include "core/common/UserManager.h"
#include "core/common/VarModel.h"
#include "core/manager/GlobalVarManager.h"
#include "ui/views/GlobalVarView.h"

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestGlobalVarView : public QObject {
    Q_OBJECT

private slots:
    void init() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        GlobalVarManager::instance().clearAllVariables();
    }

    void cleanup() {
        GlobalVarManager::instance().clearAllVariables();
        UserManager::instance().login(QStringLiteral("developer"), QString());
    }

    void testAddUsesAvailableNameAndReportsEngineBusy() {
        GlobalVarManager& variables = GlobalVarManager::instance();
        variables.addVariable(new VarModel(QStringLiteral("newVar1"), VarDataType::Int, 1));
        variables.addVariable(new VarModel(QStringLiteral("newVar2"), VarDataType::Int, 2));
        variables.removeVariable(QStringLiteral("newVar1"));

        GlobalVarView view;
        QPushButton* addButton = nullptr;
        for (QPushButton* button : view.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Add")) {
                addButton = button;
                break;
            }
        }
        QVERIFY(addButton);
        QTableWidget* table = view.findChild<QTableWidget*>();
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);

        addButton->click();
        QVERIFY(variables.hasVariable(QStringLiteral("newVar1")));
        QCOMPARE(table->rowCount(), 2);

        QVERIFY(variables.beginExecutionLease());
        QTimer::singleShot(0, &view, []() {
            if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                message->accept();
        });
        addButton->click();
        variables.endExecutionLease();
        QVERIFY(!variables.hasVariable(QStringLiteral("newVar3")));
        QCOMPARE(table->rowCount(), 2);
    }

    void testGlobalVariablesAreHiddenAndReadOnlyAfterPermissionRevocation() {
        GlobalVarManager& variables = GlobalVarManager::instance();
        const QString sentinelName = QStringLiteral("UI_PERMISSION_SENTINEL");
        const QString secondName = QStringLiteral("UI_PERMISSION_SECOND");
        auto* sentinel = new VarModel(sentinelName, VarDataType::Int, 73);
        sentinel->setExpression(QStringLiteral("UI_PERMISSION_EXPRESSION"));
        sentinel->setNote(QStringLiteral("UI_PERMISSION_NOTE"));
        variables.addVariable(sentinel);
        auto* second = new VarModel(secondName, VarDataType::Int, 19);
        second->setExpression(QStringLiteral("91"));
        variables.addVariable(second);
        const QJsonObject sentinelBefore = variables.userVariableSnapshotJson(sentinelName);
        QSignalSpy valueChangedSpy(&variables, &GlobalVarManager::variableValueChanged);

        GlobalVarView view;
        auto* table = view.findChild<QTableWidget*>(QStringLiteral("GlobalVarTable"));
        auto* type = view.findChild<QComboBox*>(QStringLiteral("GlobalVarAddTypeCombo"));
        auto* close = view.findChild<QPushButton*>(QStringLiteral("GlobalVarCloseButton"));
        const auto rowForName = [table](const QString& name) {
            for (int row = 0; row < table->rowCount(); ++row) {
                if (table->item(row, 1) && table->item(row, 1)->text() == name)
                    return row;
            }
            return -1;
        };
        QVERIFY(table);
        QVERIFY(type);
        QVERIFY(close);
        QCOMPARE(table->rowCount(), 2);
        QVERIFY(rowForName(sentinelName) >= 0);
        QVERIFY(close->isEnabled());

        const auto insertUiRow = [table](int row, const QString& name) {
            table->insertRow(row);
            for (int column = 0; column < 5; ++column)
                table->setItem(row, column, new QTableWidgetItem(column == 1 ? name : QStringLiteral("sentinel")));
        };
        for (const QString& user : {QStringLiteral("administrator"), QStringLiteral("operator"),
                                    QStringLiteral("guest-session"), QString()}) {
            if (user.isEmpty())
                UserManager::instance().logout();
            else
                UserManager::instance().login(user, QString());
            QCOMPARE(table->rowCount(), 0);
            QCOMPARE(type->currentIndex(), -1);
            QVERIFY(!table->isEnabled());
            QVERIFY(!type->isEnabled());
            QVERIFY(close->isEnabled());
            for (const QString& buttonName :
                 {QStringLiteral("GlobalVarAddButton"), QStringLiteral("GlobalVarDeleteButton"),
                  QStringLiteral("GlobalVarMoveUpButton"), QStringLiteral("GlobalVarMoveDownButton"),
                  QStringLiteral("GlobalVarExecuteButton"), QStringLiteral("GlobalVarApplyButton")})
                QVERIFY(!view.findChild<QPushButton*>(buttonName)->isEnabled());

            GlobalVarView deniedView;
            auto* deniedTable = deniedView.findChild<QTableWidget*>(QStringLiteral("GlobalVarTable"));
            QVERIFY(deniedTable);
            QCOMPARE(deniedTable->rowCount(), 0);
            QVERIFY(!deniedTable->isEnabled());
            QVERIFY(!deniedView.findChild<QComboBox*>(QStringLiteral("GlobalVarAddTypeCombo"))->isEnabled());
            QVERIFY(!deniedView.findChild<QPushButton*>(QStringLiteral("GlobalVarAddIntButton"))->isEnabled());

            insertUiRow(0, sentinelName);
            insertUiRow(1, secondName);
            table->setCurrentCell(1, 0);
            QVERIFY(QMetaObject::invokeMethod(&view, "onMoveUpClicked", Qt::DirectConnection));
            QCOMPARE(table->item(0, 1)->text(), sentinelName);
            QCOMPARE(table->item(1, 1)->text(), secondName);
            QVERIFY(QMetaObject::invokeMethod(&view, "onMoveDownClicked", Qt::DirectConnection));
            QCOMPARE(table->item(0, 1)->text(), sentinelName);
            QCOMPARE(table->item(1, 1)->text(), secondName);
            QTimer::singleShot(0, []() {
                if (QWidget* modal = QApplication::activeModalWidget())
                    modal->close();
            });
            for (const char* slot : {"onAddClicked", "onDeleteClicked", "onExecuteClicked", "onApplyClicked"})
                QVERIFY(QMetaObject::invokeMethod(&view, slot, Qt::DirectConnection));
            QCOMPARE(table->item(0, 1)->text(), sentinelName);
            QCOMPARE(table->item(1, 1)->text(), secondName);
            QCOMPARE(variables.userVariableSnapshotJson(sentinelName), sentinelBefore);
            QCOMPARE(variables.userVariableSnapshotJson(secondName).value(QStringLiteral("value")).toInt(), 19);
            QCOMPARE(valueChangedSpy.count(), 0);
            QCOMPARE(view.result(), 0);
            table->setRowCount(0);
        }

        table->setRowCount(0);
        emit variables.variableAdded(sentinelName);
        emit variables.variableRemoved(sentinelName);
        emit variables.variableValueChanged(sentinelName, 999);
        emit variables.allVariablesCleared();
        QCOMPARE(table->rowCount(), 0);

        UserManager::instance().login(QStringLiteral("developer"), QString());
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(view.refreshVariablesIfAuthorized());
        QCOMPARE(table->rowCount(), 2);
        QVERIFY(rowForName(sentinelName) >= 0);
    }
};

QTEST_MAIN(TestGlobalVarView)
#include "test_globalvarview.moc"
