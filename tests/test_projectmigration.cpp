#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/manager/PluginManager.h>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>
#include <core/model/ProjectMigrator.h>

using namespace DeepLux;

/// 阶段 2.2：2.0 -> 3.0 迁移器测试
class TestProjectMigration : public QObject {
    Q_OBJECT

private slots:
    void testMigrateLinearImageFlow();
    void testMigrationIdempotent();
    void testSaveCreatesV2Backup();
    void testLoadDoesNotModifyOriginal();
    void testProjectManagerMigratesOnOpenAndBacksUpOnSave();

private:
    void fillV2Project(Project& project);
};

void TestProjectMigration::fillV2Project(Project& project) {
    // 以 2.0 JSON 载入，确保 formatVersion=2.0
    QJsonObject json;
    json["version"] = "2.0";
    json["name"] = "迁移测试";

    QJsonArray modules;
    QJsonObject grab;
    grab["id"] = "grab";
    grab["moduleId"] = "GrabImage";
    grab["name"] = "图像采集";
    modules.append(grab);
    QJsonObject find;
    find["id"] = "findcircle";
    find["moduleId"] = "FindCircle";
    find["name"] = "找圆";
    modules.append(find);
    json["modules"] = modules;

    QJsonArray conns;
    QJsonObject conn;
    conn["fromModuleId"] = "grab";
    conn["toModuleId"] = "findcircle";
    conn["fromOutput"] = 0;
    conn["toInput"] = 0;
    conns.append(conn);
    json["connections"] = conns;

    project.fromJson(json);
}

void TestProjectMigration::testMigrateLinearImageFlow() {
    Project project;
    fillV2Project(project);
    QCOMPARE(project.formatVersion(), QStringLiteral("2.0"));

    MigrationReport report = ProjectMigrator::migrate(project);

    QCOMPARE(project.formatVersion(), QStringLiteral("3.0"));
    QVERIFY(!project.flows().isEmpty());
    QCOMPARE(project.flows().first().id, QStringLiteral("main"));
    QCOMPARE(project.flows().first().nodeIds.size(), 2);

    // 线性图像流程应映射 image -> image，无阻塞
    const auto conns = project.connections();
    QCOMPARE(conns.size(), 1);
    QCOMPARE(conns.first().fromPort, QStringLiteral("image"));
    QCOMPARE(conns.first().toPort, QStringLiteral("image"));
    QVERIFY2(!report.hasBlockers(), qPrintable(report.blockers.join("; ")));
}

void TestProjectMigration::testMigrationIdempotent() {
    Project a;
    fillV2Project(a);
    ProjectMigrator::migrate(a);
    const QJsonObject first = a.toJson();

    // 重复迁移应保持一致
    ProjectMigrator::migrate(a);
    const QJsonObject second = a.toJson();

    QCOMPARE(first["flows"], second["flows"]);
    QCOMPARE(first["connections"], second["connections"]);
    QCOMPARE(first["version"], second["version"]);
}

void TestProjectMigration::testSaveCreatesV2Backup() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("proj.dlx.json");

    // 先写一个 2.0 文件
    Project v2;
    fillV2Project(v2);
    QVERIFY(v2.save(path));

    // 读取并迁移为 3.0，再 saveWithBackup -> 应生成 .v2.bak
    Project v3;
    QVERIFY(v3.load(path));
    ProjectMigrator::migrate(v3);
    QVERIFY(ProjectMigrator::saveWithBackup(v3, path));
    QVERIFY2(QFile::exists(path + ".v2.bak"), "expected .v2.bak for 2.0 overwrite");

    // 备份应为 2.0
    QFile bak(path + ".v2.bak");
    QVERIFY(bak.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(bak.readAll()).object()["version"].toString(), QStringLiteral("2.0"));
}

void TestProjectMigration::testLoadDoesNotModifyOriginal() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("orig.dlx.json");

    Project v2;
    fillV2Project(v2);
    QVERIFY(v2.save(path));
    QFile before(path);
    before.open(QIODevice::ReadOnly);
    const QByteArray originalBytes = before.readAll();
    before.close();

    // 仅加载不应修改原文件
    Project loaded;
    QVERIFY(loaded.load(path));

    QFile after(path);
    after.open(QIODevice::ReadOnly);
    QCOMPARE(after.readAll(), originalBytes);
}

void TestProjectMigration::testProjectManagerMigratesOnOpenAndBacksUpOnSave() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("legacy.dlx.json");

    Project legacy;
    fillV2Project(legacy);
    QVERIFY(legacy.save(path));

    ProjectManager& manager = ProjectManager::instance();
    manager.closeProject();
    Project* migrated = manager.openProject(path);
    QVERIFY(migrated != nullptr);
    QCOMPARE(migrated->formatVersion(), QStringLiteral("3.0"));
    QCOMPARE(migrated->connections().first().fromPort, QStringLiteral("image"));
    QCOMPARE(migrated->connections().first().toPort, QStringLiteral("image"));
    QVERIFY(manager.saveProject());

    QFile backup(path + ".v2.bak");
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(backup.readAll()).object()["version"].toString(), QStringLiteral("2.0"));
    manager.closeProject();
}

QTEST_MAIN(TestProjectMigration)
#include "test_projectmigration.moc"
