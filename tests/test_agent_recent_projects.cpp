#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/agent/AgentActor.h>
#include <core/agent/AgentController.h>
#include <core/common/Logger.h>
#include <core/engine/RunEngine.h>
#include <core/manager/ProjectManager.h>
#include <thread>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentActionLogWidget.h>

using namespace DeepLux;

class TestAgentRecentProjects : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ProjectManager::instance().clearRecentProjects();
    }

    void cleanup() {
        ProjectManager::instance().closeProject();
        ProjectManager::instance().clearRecentProjects();
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testListRecentProjectsIsReadOnlyAndPreservesInvalidEntries() {
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        QVERIFY(!projects.currentProject());

        QTemporaryDir fixtureDir;
        QVERIFY(fixtureDir.isValid());
        const QString staleProjectPath = fixtureDir.filePath(QStringLiteral("deleted-project.dproj"));
        const QString validProjectPath = fixtureDir.filePath(QStringLiteral("valid-project.dproj"));
        QVERIFY(projects.newProject());
        QVERIFY(projects.saveProject(staleProjectPath));
        QVERIFY(projects.saveAsProject(validProjectPath));
        projects.closeProject();
        QVERIFY(QFile::remove(staleProjectPath));

        const QStringList recentBefore = projects.recentProjects();
        QCOMPARE(recentBefore, QStringList({validProjectPath, staleProjectPath}));
        QSettings settings;
        settings.beginGroup(QStringLiteral("ProjectManager"));
        const QStringList persistedRecentBefore = settings.value(QStringLiteral("recentProjects")).toStringList();
        settings.endGroup();

        AgentController& controller = AgentController::instance();
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        AgentActionLogWidget* actionLog = window.findChild<AgentActionLogWidget*>();
        QVERIFY(actionLog);
        const int auditCountBefore = actionLog->entries().size();

        const QJsonObject unexpectedParams{{QStringLiteral("path"), QStringLiteral("must-not-be-audited")}};
        const int loggerCountBeforeDirect = Logger::instance().logs().size();
        const QJsonObject missing =
            controller.actor()->executeTool(QStringLiteral("list_recent_projects"), unexpectedParams);
        QCOMPARE(missing.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonArray missingProjects = missing.value(QStringLiteral("projects")).toArray();
        QCOMPARE(missingProjects.size(), 2);
        const QJsonObject validEntry = missingProjects.at(0).toObject();
        QCOMPARE(validEntry.value(QStringLiteral("index")).toInt(), 0);
        QCOMPARE(validEntry.value(QStringLiteral("path")).toString(), validProjectPath);
        QCOMPARE(validEntry.value(QStringLiteral("fileName")).toString(), QFileInfo(validProjectPath).fileName());
        QCOMPARE(validEntry.value(QStringLiteral("exists")).toBool(), true);
        QCOMPARE(validEntry.value(QStringLiteral("isFile")).toBool(), true);
        const QJsonObject missingEntry = missingProjects.at(1).toObject();
        QCOMPARE(missingEntry.value(QStringLiteral("index")).toInt(), 1);
        QCOMPARE(missingEntry.value(QStringLiteral("path")).toString(), staleProjectPath);
        QCOMPARE(missingEntry.value(QStringLiteral("fileName")).toString(), QFileInfo(staleProjectPath).fileName());
        QCOMPARE(missingEntry.value(QStringLiteral("exists")).toBool(), false);
        QCOMPARE(missingEntry.value(QStringLiteral("isFile")).toBool(), false);
        QCOMPARE(Logger::instance().logs().size(), loggerCountBeforeDirect);

        QVERIFY(QDir().mkdir(staleProjectPath));
        const QJsonObject directory =
            controller.actor()->executeTool(QStringLiteral("list_recent_projects"), unexpectedParams);
        const QJsonArray directoryProjects = directory.value(QStringLiteral("projects")).toArray();
        QCOMPARE(directoryProjects.size(), 2);
        QCOMPARE(directoryProjects.at(0).toObject().value(QStringLiteral("path")).toString(), validProjectPath);
        QCOMPARE(directoryProjects.at(0).toObject().value(QStringLiteral("exists")).toBool(), true);
        QCOMPARE(directoryProjects.at(0).toObject().value(QStringLiteral("isFile")).toBool(), true);
        QCOMPARE(directoryProjects.at(1).toObject().value(QStringLiteral("path")).toString(), staleProjectPath);
        QCOMPARE(directoryProjects.at(1).toObject().value(QStringLiteral("exists")).toBool(), true);
        QCOMPARE(directoryProjects.at(1).toObject().value(QStringLiteral("isFile")).toBool(), false);
        QCOMPARE(Logger::instance().logs().size(), loggerCountBeforeDirect);

        for (const AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            const QJsonObject result =
                controller.handleToolCall(QStringLiteral("list_recent_projects"), unexpectedParams);
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QCOMPARE(result.value(QStringLiteral("projects")).toArray(), directoryProjects);
            QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        }
        QCOMPARE(actionLog->entries().size(), auditCountBefore + 3);
        const AgentActionLogEntry audit = actionLog->entries().last();
        QCOMPARE(audit.action, QStringLiteral("list_recent_projects"));
        QCOMPARE(QJsonDocument::fromJson(audit.params.toUtf8()).object(), QJsonObject());
        QVERIFY(!audit.params.contains(QStringLiteral("must-not-be-audited")));
        QCOMPARE(projects.recentProjects(), recentBefore);
        settings.beginGroup(QStringLiteral("ProjectManager"));
        QCOMPARE(settings.value(QStringLiteral("recentProjects")).toStringList(), persistedRecentBefore);
        settings.endGroup();
        QVERIFY(!projects.currentProject());
        QVERIFY(!RunEngine::instance().isBusy());

        QJsonObject affinityResult;
        std::thread worker([&]() {
            affinityResult = controller.executeDataSourceViewAction(QStringLiteral("list_recent_projects"), {});
        });
        worker.join();
        QCOMPARE(affinityResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QCOMPARE(projects.recentProjects(), recentBefore);

        projects.clearRecentProjects();
        settings.beginGroup(QStringLiteral("ProjectManager"));
        const QStringList persistedEmpty = settings.value(QStringLiteral("recentProjects")).toStringList();
        settings.endGroup();
        const int loggerCountBeforeEmpty = Logger::instance().logs().size();
        const QJsonObject empty = controller.actor()->executeTool(QStringLiteral("list_recent_projects"), {});
        QCOMPARE(empty.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QVERIFY(empty.value(QStringLiteral("projects")).toArray().isEmpty());
        QCOMPARE(Logger::instance().logs().size(), loggerCountBeforeEmpty);
        settings.beginGroup(QStringLiteral("ProjectManager"));
        QCOMPARE(settings.value(QStringLiteral("recentProjects")).toStringList(), persistedEmpty);
        settings.endGroup();
        QVERIFY(!projects.currentProject());
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }
};

QTEST_MAIN(TestAgentRecentProjects)
#include "test_agent_recent_projects.moc"
