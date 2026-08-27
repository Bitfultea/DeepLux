#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
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
    // 阶4: 只读一致性——JSON 结论分布与 legacy-comparison.md 陈述一致，防漂移
    void testMappingConclusionConsistency();
    // 阶1: 迁移决策枚举+统计一致性（53 missing 全部决策，JSON 与 MD 一致）
    void testMigrationDecisionConsistency();

private:
    void fillV2Project(Project& project);
};

void TestProjectMigration::testMappingConclusionConsistency() {
    const QString root = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    QFile jf(root + "/docs/baseline/hotfix-plugin-mapping.json");
    QFile mf(root + "/docs/baseline/legacy-comparison.md");
    QFile generatedMf(root + "/docs/baseline/hotfix-plugin-mapping.md");
    QVERIFY(jf.open(QIODevice::ReadOnly));
    QVERIFY(mf.open(QIODevice::ReadOnly));
    QVERIFY(generatedMf.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(jf.readAll()).object();
    const QString md = QString::fromUtf8(mf.readAll());
    const QString generatedMd = QString::fromUtf8(generatedMf.readAll());
    jf.close();
    mf.close();
    generatedMf.close();

    // 统计 JSON 结论分布
    QMap<QString, int> jsonCount;
    for (const auto& v : json["plugins"].toArray()) {
        const QJsonObject p = v.toObject();
        if (p["reviewState"].toString() == "reviewed")
            jsonCount[p["reviewConclusion"].toString()]++;
    }

    // MD 汇总行形如 "equivalent=0、intentionally_changed=12、partial=34、unverified=4"
    for (const QString key : {"equivalent", "intentionally_changed", "partial", "unverified"}) {
        QRegularExpression re(key + "=(\\d+)");
        auto m = re.match(md);
        QVERIFY2(m.hasMatch(), qPrintable("MD missing count for " + key));
        QCOMPARE(m.captured(1).toInt(), jsonCount.value(key, 0));
    }

    for (const QString key : {"equivalent", "intentionally_changed", "partial", "unverified", "not_equivalent"}) {
        const QRegularExpression re(
            QStringLiteral("\\|\\s*%1\\s*\\|\\s*(\\d+)\\s*\\|").arg(QRegularExpression::escape(key)));
        const auto match = re.match(generatedMd);
        QVERIFY2(match.hasMatch(), qPrintable("generated mapping missing count for " + key));
        QCOMPARE(match.captured(1).toInt(), jsonCount.value(key, 0));
    }
}

void TestProjectMigration::testMigrationDecisionConsistency() {
    // 阶1: 53 个 missing 全部得到决策；枚举合法；JSON 与 mapping.md 统计一致
    const QString root = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    QFile jf(root + "/docs/baseline/hotfix-plugin-mapping.json");
    QFile mf(root + "/docs/baseline/hotfix-plugin-mapping.md");
    QVERIFY(jf.open(QIODevice::ReadOnly));
    QVERIFY(mf.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(jf.readAll()).object();
    const QString md = QString::fromUtf8(mf.readAll());
    jf.close();
    mf.close();

    const QSet<QString> validEnum{"rebuild", "replace", "retire", "business_pack"};
    const QSet<QString> validPriority{"P0", "P1", "P2", "P3"};

    // 收集当前真实插件 ID：扫描 src/plugins/*/metadata.json 的 id（权威来源），
    // 用于校验 replace 的替代 ID 真实存在
    QSet<QString> currentPluginIds;
    QDirIterator it(root + "/src/plugins", QStringList() << "metadata.json", QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile mf(it.next());
        if (!mf.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject mo = QJsonDocument::fromJson(mf.readAll()).object();
        mf.close();
        if (!mo["id"].toString().isEmpty())
            currentPluginIds.insert(mo["id"].toString());
    }
    QVERIFY2(!currentPluginIds.isEmpty(), "no current plugin metadata found");

    QMap<QString, int> jsonDecision;
    int missingTotal = 0;
    for (const auto& v : json["plugins"].toArray()) {
        const QJsonObject p = v.toObject();
        if (p["matchKind"].toString() != "missing")
            continue;
        ++missingTotal;
        const QString name = p["legacyPlugin"].toString();
        const QString dec = p["migrationDecision"].toString();
        const QString prio = p["priority"].toString();
        const QByteArray nameCtx = (name + " decision").toUtf8();
        QVERIFY2(validEnum.contains(dec), qPrintable(QString("%1 has invalid decision '%2'").arg(name, dec)));
        QVERIFY2(validPriority.contains(prio), qPrintable(QString("%1 has invalid priority '%2'").arg(name, prio)));

        // 分类专属字段校验
        if (dec == "rebuild") {
            for (const char* f : {"input", "output", "keyParams", "scenario"}) {
                QVERIFY2(!p[f].toString().isEmpty(), qPrintable(QString("%1(rebuild) missing %2").arg(name, f)));
            }
        } else if (dec == "replace") {
            const QString repl = p["replacementPluginId"].toString();
            QVERIFY2(!repl.isEmpty(), qPrintable(QString("%1(replace) missing replacementPluginId").arg(name)));
            QVERIFY2(currentPluginIds.contains(repl),
                     qPrintable(QString("%1 replacement '%2' is not a real current plugin").arg(name, repl)));
        } else if (dec == "retire") {
            QVERIFY2(!p["reason"].toString().isEmpty(), qPrintable(QString("%1(retire) missing reason").arg(name)));
        } else if (dec == "business_pack") {
            QVERIFY2(!p["dependencies"].toString().isEmpty(),
                     qPrintable(QString("%1(business_pack) missing dependencies").arg(name)));
        }
        jsonDecision[dec]++;
    }
    QVERIFY2(missingTotal == 53, qPrintable(QString("expected 53 missing, got %1").arg(missingTotal)));

    // 截取"迁移范围决策"段，避免与 matchKind 表的 business_pack 计数混淆
    const int secStart = md.indexOf(QStringLiteral("## 迁移范围决策"));
    QVERIFY2(secStart >= 0, "mapping.md missing migration decision section");
    int secEnd = md.indexOf(QStringLiteral("完整逐项数据"), secStart);
    if (secEnd < 0)
        secEnd = md.size();
    const QString sec = md.mid(secStart, secEnd - secStart);

    // mapping.md 迁移决策统计与 JSON 一致
    for (const QString key : {"rebuild", "replace", "retire", "business_pack", "pending"}) {
        const QRegularExpression re(
            QStringLiteral("\\|\\s*%1\\s*\\|\\s*(\\d+)\\s*\\|").arg(QRegularExpression::escape(key)));
        const auto match = re.match(sec);
        QVERIFY2(match.hasMatch(), qPrintable("mapping.md missing migration count for " + key));
        QCOMPARE(match.captured(1).toInt(), jsonDecision.value(key, 0));
    }
}

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
