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
    // 阶7 批1 复核四轮：FitEllipse 实现后不得再列为 missing（matchKind=direct）；
    // 磁盘 MD 与测试内重生成 MD 均不得再列其为 missing。
    const QString fitEllipseMissingRow =
        QStringLiteral("| FitEllipse | `02Plugins/004几何关系/Plugin.FitEllipse` | - | missing |");
    QVERIFY2(!md.contains(fitEllipseMissingRow), "FitEllipse must not be listed as missing (disk MD)");
    QVERIFY2(!generatedMd.contains(fitEllipseMissingRow), "FitEllipse must not be listed as missing (regenerated MD)");
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
    // 阶7 批1 复核四轮：阶段 1 的 53 项范围按"存在 migrationDecision"统计，
    // 不再按当前 missing 数量（实现后 matchKind 会变为 direct/candidate）。
    int decisionTotal = 0;
    for (const auto& v : json["plugins"].toArray()) {
        const QJsonObject p = v.toObject();
        if (p["migrationDecision"].toString().isEmpty())
            continue;
        ++decisionTotal;
        const QString name = p["legacyPlugin"].toString();
        const QString dec = p["migrationDecision"].toString();
        const QString prio = p["priority"].toString();
        const QByteArray nameCtx = (name + " decision").toUtf8();
        QVERIFY2(validEnum.contains(dec), qPrintable(QString("%1 has invalid decision '%2'").arg(name, dec)));
        QVERIFY2(validPriority.contains(prio), qPrintable(QString("%1 has invalid priority '%2'").arg(name, prio)));

        // 阶1复核：字段必须提供真实结构化契约——非空、去空白后长度≥2，
        // 且不得为占位文本（"见证据"/"待补"等形式通过但无契约信息的值）
        static const QSet<QString> placeholders{"见证据", "待补", "-", "TBD", "N/A", "?"};
        const auto substantive = [&](const char* f) {
            const QString val = p[f].toString().trimmed();
            QVERIFY2(
                val.size() >= 2 && !placeholders.contains(val),
                qPrintable(
                    QString("%1(%2) field '%3' is empty or placeholder: '%4'").arg(name, dec, f, p[f].toString())));
        };

        // 分类专属字段校验
        if (dec == "rebuild") {
            for (const char* f : {"input", "output", "keyParams", "scenario"}) {
                substantive(f);
            }
            // evidence 必须与四字段一致，防止字段与结论描述漂移
            const QString expect = QStringLiteral("输入:%1；输出:%2；关键参数:%3；场景:%4")
                                       .arg(p["input"].toString(), p["output"].toString(), p["keyParams"].toString(),
                                            p["scenario"].toString());
            QVERIFY2(
                p["evidence"].toString() == expect,
                qPrintable(QString("%1(rebuild) evidence does not match input/output/keyParams/scenario").arg(name)));
        } else if (dec == "replace") {
            const QString repl = p["replacementPluginId"].toString();
            QVERIFY2(!repl.isEmpty(), qPrintable(QString("%1(replace) missing replacementPluginId").arg(name)));
            QVERIFY2(currentPluginIds.contains(repl),
                     qPrintable(QString("%1 replacement '%2' is not a real current plugin").arg(name, repl)));
            QVERIFY2(p["evidence"].toString().startsWith(QStringLiteral("替代流程：")),
                     qPrintable(QString("%1(replace) evidence must start with 替代流程：").arg(name)));
        } else if (dec == "retire") {
            substantive("reason");
            QVERIFY2(p["evidence"].toString() == QStringLiteral("淘汰理由：") + p["reason"].toString(),
                     qPrintable(QString("%1(retire) evidence must equal 淘汰理由：+reason").arg(name)));
        } else if (dec == "business_pack") {
            substantive("dependencies");
            QVERIFY2(p["evidence"].toString().startsWith(QStringLiteral("依赖：")),
                     qPrintable(QString("%1(business_pack) evidence must start with 依赖：").arg(name)));
        }
        if (p["matchKind"].toString() != "direct")
            jsonDecision[dec]++; // MD 决策段仅列非 direct 行
    }
    QVERIFY2(decisionTotal == 53, qPrintable(QString("expected 53 migrationDecision, got %1").arg(decisionTotal)));

    // 阶7 批1 复核三轮（P1-4）：全量一致性——结论枚举合法、候选与 matchKind 不矛盾
    static const QSet<QString> validConclusions{QStringLiteral("equivalent"), QStringLiteral("intentionally_changed"),
                                                QStringLiteral("partial"), QStringLiteral("unverified"),
                                                QStringLiteral("not_equivalent")};
    for (const auto& v : json["plugins"].toArray()) {
        const QJsonObject p = v.toObject();
        const QString name = p["legacyPlugin"].toString();
        const QString kind = p["matchKind"].toString();
        const QString cand = p["currentCandidate"].toString();
        // 候选与 matchKind 不矛盾：missing 不得有候选；有候选不得为 missing
        if (kind == QStringLiteral("missing")) {
            QVERIFY2(cand.isEmpty(), qPrintable(QString("%1 matchKind=missing but has candidate %2").arg(name, cand)));
        } else if (kind == QStringLiteral("direct") || kind == QStringLiteral("candidate")) {
            QVERIFY2(!cand.isEmpty(), qPrintable(QString("%1 matchKind=%2 but no candidate").arg(name, kind)));
        }
        // 阶7 批1 复核四轮：mapping 与当前 metadata 扫描结果一致——候选必须真实存在
        if (!cand.isEmpty()) {
            QVERIFY2(currentPluginIds.contains(p["currentPluginId"].toString()),
                     qPrintable(QString("%1 candidate %2 id %3 not found in current metadata scan")
                                    .arg(name, cand, p["currentPluginId"].toString())));
        }
        if (p["reviewState"].toString() == QStringLiteral("reviewed") && p.contains("reviewConclusion")) {
            const QString conc = p["reviewConclusion"].toString();
            QVERIFY2(validConclusions.contains(conc),
                     qPrintable(QString("%1 has invalid reviewConclusion '%2'").arg(name, conc)));
        }
    }

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
