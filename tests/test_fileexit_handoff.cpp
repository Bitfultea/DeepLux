#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/device/CameraManager.h>
#include <ui/views/MainWindow.h>

using DeepLux::CameraManager;
using DeepLux::MainWindow;

class QuitOnLastWindowClosedGuard {
public:
    explicit QuitOnLastWindowClosedGuard(bool value) : m_previousValue(qApp->quitOnLastWindowClosed()) {
        qApp->setQuitOnLastWindowClosed(value);
    }

    ~QuitOnLastWindowClosedGuard() {
        qApp->setQuitOnLastWindowClosed(m_previousValue);
    }

private:
    bool m_previousValue;
};

class FileExitHandoffTest : public QObject {
    Q_OBJECT

private slots:
    void testFileExitMenuClosesMainWindowWithoutQuittingTestHost();
};

void FileExitHandoffTest::testFileExitMenuClosesMainWindowWithoutQuittingTestHost() {
    QTemporaryDir settingsDir;
    QVERIFY(settingsDir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    const QuitOnLastWindowClosedGuard quitPolicyGuard(false);

    MainWindow window;
    window.show();
    QTRY_VERIFY(window.isVisible());

    QAction* exitAction = nullptr;
    for (QAction* menuAction : window.menuBar()->actions()) {
        QMenu* menu = menuAction->menu();
        if (!menu)
            continue;
        for (QAction* action : menu->actions()) {
            if (action->text() == QStringLiteral("退出")) {
                exitAction = action;
                break;
            }
        }
        if (exitAction)
            break;
    }
    QVERIFY(exitAction);

    QSignalSpy shutdownSpy(&CameraManager::instance(), &CameraManager::shutdownCompleted);
    QSignalSpy aboutToQuitSpy(qApp, &QCoreApplication::aboutToQuit);
    QVERIFY(shutdownSpy.isValid());
    QVERIFY(aboutToQuitSpy.isValid());

    exitAction->trigger();

    QCOMPARE(shutdownSpy.count(), 1);
    QVERIFY(window.isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
    QCOMPARE(shutdownSpy.count(), 1);
    QCOMPARE(aboutToQuitSpy.count(), 0);
}

QTEST_MAIN(FileExitHandoffTest)
#include "test_fileexit_handoff.moc"
