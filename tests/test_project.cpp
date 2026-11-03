#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>
#include <algorithm>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>
#include <limits>

using namespace DeepLux;

class TestProject : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testCreate();
    void testName();
    void testModules();
    void testPositionSourceGenerationsAreRuntimeOnly();
    void testParameterSourceGenerationsTrackAllModelWrites();
    void testModuleOrderExternalGenerationAndLoadInvalidation();
    void testSetModuleParam();
    void testMeasurementInputPickCoordinateFrameInvalidatesAfterIndependentEdits();
    void testMoveModule();
    void testConnections();
    void testCameras();
    void testSerialization();
    void testSaveLoad();
    void testAtomicSavePreservesExistingFileAndProjectState();
    void testAtomicSavePreservesSymlinkTargetsAndRejectsDanglingLinks();
    void testModuleRuntimeFieldsRoundTrip();
    // 阶段1 §3（G1）：共享入口未保存保护（GUI/CLI/Agent 同一 resolveUnsavedForSwitch）
    void testUnsavedSwitchPoliciesAtManagerLevel();
    void testUnsavedSaveFailureKeepsProjectForOpenNewAndClose();
    void testOpenStructurallyInvalidProjectKeepsCurrent();

private:
    QString m_tempPath;
};

void TestProject::initTestCase() {
    qDebug() << "=== TestProject Start ===";
    m_tempPath =
        QDir::tempPath() + "/deeplux_test_" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".deeplux";
}

void TestProject::cleanupTestCase() {
    QFile::remove(m_tempPath);
    qDebug() << "=== TestProject End ===";
}

void TestProject::testCreate() {
    Project project;

    QVERIFY(!project.id().isEmpty());
    QCOMPARE(project.name(), QString("未命名项目"));
    QVERIFY(!project.isModified());
}

void TestProject::testName() {
    Project project;

    project.setName("测试项目");
    QCOMPARE(project.name(), QString("测试项目"));
    QVERIFY(project.isModified());
}

void TestProject::testModules() {
    Project project;

    ModuleInstance module;
    module.id = "test-module-1";
    module.moduleId = "com.deeplux.test";
    module.name = "Test Module";
    module.posX = 100;
    module.posY = 200;

    project.addModule(module);
    QCOMPARE(project.modules().size(), 1);

    ModuleInstance* found = project.findModule("test-module-1");
    QVERIFY(found != nullptr);
    QCOMPARE(found->name, QString("Test Module"));

    project.removeModule("test-module-1");
    QCOMPARE(project.modules().size(), 0);
}

void TestProject::testPositionSourceGenerationsAreRuntimeOnly() {
    Project project;
    ModuleInstance module;
    module.id = QStringLiteral("position_source");
    module.moduleId = QStringLiteral("test");
    project.addModule(module);
    QCOMPARE(project.positionGenerations(module.id).gui, quint64(0));
    QCOMPARE(project.positionGenerations(module.id).agent, quint64(0));

    ModuleInstance updated = module;
    updated.posX = 10;
    project.updateModule(module.id, updated, Project::PositionChangeSource::Gui);
    QCOMPARE(project.positionGenerations(module.id).gui, quint64(0));
    QCOMPARE(project.positionGenerations(module.id).agent, quint64(1));

    project.updateModule(module.id, updated, Project::PositionChangeSource::Agent); // position no-op
    QCOMPARE(project.positionGenerations(module.id).gui, quint64(0));
    QCOMPARE(project.positionGenerations(module.id).agent, quint64(1));

    updated.posY = -5;
    project.updateModule(module.id, updated, Project::PositionChangeSource::Agent);
    QCOMPARE(project.positionGenerations(module.id).gui, quint64(1));
    QCOMPARE(project.positionGenerations(module.id).agent, quint64(1));
    updated.posX = -20;
    project.updateModule(module.id, updated);
    QCOMPARE(project.positionGenerations(module.id).gui, quint64(2));
    QCOMPARE(project.positionGenerations(module.id).agent, quint64(2));

    updated.posX = std::numeric_limits<qint32>::min();
    updated.posY = std::numeric_limits<qint32>::max();
    project.updateModule(module.id, updated, Project::PositionChangeSource::Gui);
    QVERIFY(!project.toJson().contains(QStringLiteral("positionGenerations")));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("position-generations.dproj"));
    QVERIFY(project.save(path));
    Project reopened;
    QVERIFY(reopened.load(path));
    QCOMPARE(reopened.moduleById(module.id)->posX, std::numeric_limits<qint32>::min());
    QCOMPARE(reopened.moduleById(module.id)->posY, std::numeric_limits<qint32>::max());
    QCOMPARE(reopened.positionGenerations(module.id).gui, quint64(0));
    QCOMPARE(reopened.positionGenerations(module.id).agent, quint64(0));
}

void TestProject::testModuleOrderExternalGenerationAndLoadInvalidation() {
    Project project;
    ModuleInstance first;
    first.id = QStringLiteral("order_generation_first");
    first.moduleId = QStringLiteral("test");
    ModuleInstance second = first;
    second.id = QStringLiteral("order_generation_second");
    project.addModule(first);
    project.addModule(second);
    QCOMPARE(project.moduleOrderExternalGeneration(), quint64(2));

    QVERIFY(project.moveModule(second.id, 0));
    QCOMPARE(project.moduleOrderExternalGeneration(), quint64(3));
    QVERIFY(project.moveModule(second.id, 0)); // Same index is a no-op.
    QVERIFY(!project.moveModule(QStringLiteral("missing"), 0));
    project.removeModule(QStringLiteral("missing"));
    QCOMPARE(project.moduleOrderExternalGeneration(), quint64(3));

    const ModuleInstance duplicate = first;
    project.addModule(duplicate);
    QCOMPARE(project.moduleOrderExternalGeneration(), quint64(3));
    ModuleInstance emptyId = first;
    emptyId.id.clear(); // Existing Project API accepts and inserts this; membership changed.
    project.addModule(emptyId);
    QCOMPARE(project.moduleOrderExternalGeneration(), quint64(4));

    project.moveModule(second.id, 2, Project::ModuleOrderChangeSource::Agent);
    ModuleInstance agentAdded;
    agentAdded.id = QStringLiteral("agent_order_added");
    agentAdded.moduleId = QStringLiteral("test");
    project.addModule(agentAdded, -1, Project::ModuleOrderChangeSource::Agent);
    project.removeModule(QStringLiteral("agent_order_added"), Project::ModuleOrderChangeSource::Agent);
    QCOMPARE(project.moduleOrderExternalGeneration(), quint64(4));
    QVERIFY(!project.toJson().contains(QStringLiteral("moduleOrderExternalGeneration")));

    // Empty IDs are accepted by the in-memory API but rejected by project-file validation.
    // Keep the reload case valid while preserving the explicit insertion/generation check above.
    project.removeModule(QString());
    QCOMPARE(project.moduleOrderExternalGeneration(), quint64(5));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("same-order.dproj"));
    QVERIFY(project.save(path));
    const QJsonObject saved = project.toJson();
    const quint64 beforeReload = project.moduleOrderExternalGeneration();
    QVERIFY(project.load(path));
    QCOMPARE(project.toJson(), saved);
    QCOMPARE(project.moduleOrderExternalGeneration(), beforeReload + 1);

    project.setName(QStringLiteral("dirty before failed load"));
    const QJsonObject beforeFailedLoad = project.toJson();
    const quint64 beforeFailedLoadGeneration = project.moduleOrderExternalGeneration();
    QTemporaryFile invalidFile(dir.filePath(QStringLiteral("invalid-XXXXXX.dproj")));
    QVERIFY(invalidFile.open());
    QCOMPARE(invalidFile.write("{"), qint64(1));
    QVERIFY(invalidFile.flush());
    QVERIFY(!project.load(invalidFile.fileName()));
    QCOMPARE(project.toJson(), beforeFailedLoad);
    QCOMPARE(project.moduleOrderExternalGeneration(), beforeFailedLoadGeneration);
    QVERIFY(project.isModified());
}

void TestProject::testParameterSourceGenerationsTrackAllModelWrites() {
    Project project;
    ModuleInstance module;
    module.id = QStringLiteral("param_generations");
    module.moduleId = QStringLiteral("test");
    module.params = QJsonObject{{QStringLiteral("a"), 1}, {QStringLiteral("nullable"), QJsonValue::Null}};
    project.addModule(module);
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("a")).gui, quint64(1));
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("a")).agent, quint64(1));

    const auto initialNullable = project.parameterGenerations(module.id, QStringLiteral("nullable"));
    QVERIFY(project.setModuleParam(module.id, QStringLiteral("a"), 1)); // same-value no-op
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("a")).gui, quint64(1));
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("a")).agent, quint64(1));

    QVERIFY(project.setModuleParam(module.id, QStringLiteral("a"), 2, Project::ParameterChangeSource::Gui));
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("a")).gui, quint64(2));
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("a")).agent, quint64(1));

    QVERIFY(project.removeModuleParam(module.id, QStringLiteral("nullable"), Project::ParameterChangeSource::Agent));
    QVERIFY(!project.moduleById(module.id)->params.contains(QStringLiteral("nullable")));
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("nullable")).gui, initialNullable.gui);
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("nullable")).agent, initialNullable.agent + 1);

    ModuleInstance unknownWrite = *project.moduleById(module.id);
    unknownWrite.params.insert(QStringLiteral("b"), 3);
    project.updateModule(module.id, unknownWrite); // unspecified direct model writes invalidate both sources
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("b")).gui, quint64(1));
    QCOMPARE(project.parameterGenerations(module.id, QStringLiteral("b")).agent, quint64(1));
    QVERIFY(!project.toJson().contains(QStringLiteral("parameterGenerations")));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("parameter-generations.dproj"));
    QVERIFY(project.save(path));
    Project reopened;
    QVERIFY(reopened.load(path));
    QCOMPARE(reopened.moduleById(module.id)->params.value(QStringLiteral("a")), QJsonValue(2));
    QVERIFY(!reopened.moduleById(module.id)->params.contains(QStringLiteral("nullable")));
    QCOMPARE(reopened.parameterGenerations(module.id, QStringLiteral("a")).gui, quint64(0));
    QCOMPARE(reopened.parameterGenerations(module.id, QStringLiteral("a")).agent, quint64(0));
}

void TestProject::testSetModuleParam() {
    Project project;

    ModuleInstance module;
    module.id = "test-module-1";
    module.moduleId = "com.deeplux.test";
    module.name = "Test Module";
    project.addModule(module);
    project.setModified(false);

    QSignalSpy updatedSpy(&project, &Project::moduleUpdated);
    QVERIFY(project.setModuleParam("test-module-1", "threshold", 42));

    ModuleInstance* found = project.findModule("test-module-1");
    QVERIFY(found != nullptr);
    QCOMPARE(found->params.value("threshold").toInt(), 42);
    QVERIFY(project.isModified());
    QCOMPARE(updatedSpy.count(), 1);

    project.setModified(false);
    QVERIFY(!project.setModuleParam("missing-module", "threshold", 7));
    QVERIFY(!project.isModified());
}

void TestProject::testMeasurementInputPickCoordinateFrameInvalidatesAfterIndependentEdits() {
    const auto projectWithPickedInput = []() {
        auto project = std::make_unique<Project>();
        ModuleInstance input;
        input.id = QStringLiteral("measurement");
        input.moduleId = QStringLiteral("MeasurementInput");
        input.params = QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_line")},
                                   {QStringLiteral("point"), QJsonArray{1.0, 2.0, 0.0}},
                                   {QStringLiteral("line"), QJsonArray{3.0, 4.0, 5.0, 6.0}},
                                   {QStringLiteral("pick_coordinate_frame"), QStringLiteral("ImageLocalPixel")}};
        project->addModule(input);
        return project;
    };

    {
        auto project = projectWithPickedInput();
        QVERIFY(project->setModuleParam(QStringLiteral("measurement"), QStringLiteral("mode"),
                                        QStringLiteral("point_set")));
        QCOMPARE(project->moduleById(QStringLiteral("measurement"))
                     ->params.value(QStringLiteral("pick_coordinate_frame"))
                     .toString(),
                 QStringLiteral("Unspecified"));
        QVERIFY(project->setModuleParam(QStringLiteral("measurement"), QStringLiteral("mode"),
                                        QStringLiteral("point_line")));
        QCOMPARE(project->moduleById(QStringLiteral("measurement"))
                     ->params.value(QStringLiteral("pick_coordinate_frame"))
                     .toString(),
                 QStringLiteral("Unspecified"));
    }
    {
        auto project = projectWithPickedInput();
        ModuleInstance updated = *project->moduleById(QStringLiteral("measurement"));
        updated.params.insert(QStringLiteral("point"), QJsonArray{7.0, 8.0, 9.0});
        project->updateModule(updated.id, updated);
        QCOMPARE(project->moduleById(updated.id)->params.value(QStringLiteral("pick_coordinate_frame")).toString(),
                 QStringLiteral("Unspecified"));
    }
    {
        auto project = projectWithPickedInput();
        QVERIFY(project->removeModuleParam(QStringLiteral("measurement"), QStringLiteral("line")));
        QCOMPARE(project->moduleById(QStringLiteral("measurement"))
                     ->params.value(QStringLiteral("pick_coordinate_frame"))
                     .toString(),
                 QStringLiteral("Unspecified"));
    }
    {
        auto project = projectWithPickedInput();
        QVERIFY(project->setModuleParam(QStringLiteral("measurement"), QStringLiteral("line1"),
                                        QJsonArray{10.0, 20.0, 30.0, 40.0}));
        QCOMPARE(project->moduleById(QStringLiteral("measurement"))
                     ->params.value(QStringLiteral("pick_coordinate_frame"))
                     .toString(),
                 QStringLiteral("Unspecified"));
    }
    {
        auto project = projectWithPickedInput();
        ModuleInstance updated = *project->moduleById(QStringLiteral("measurement"));
        updated.params.insert(QStringLiteral("line2"), QJsonArray{11.0, 21.0, 31.0, 41.0});
        project->updateModule(updated.id, updated);
        QCOMPARE(project->moduleById(updated.id)->params.value(QStringLiteral("pick_coordinate_frame")).toString(),
                 QStringLiteral("Unspecified"));
    }
    {
        Project project;
        ModuleInstance legacy;
        legacy.id = QStringLiteral("legacy");
        legacy.moduleId = QStringLiteral("MeasurementInput");
        legacy.params.insert(QStringLiteral("mode"), QStringLiteral("point_line"));
        project.addModule(legacy);
        QVERIFY(project.setModuleParam(QStringLiteral("legacy"), QStringLiteral("mode"), QStringLiteral("point_pair")));
        QVERIFY(
            !project.moduleById(QStringLiteral("legacy"))->params.contains(QStringLiteral("pick_coordinate_frame")));
    }
}

void TestProject::testMoveModule() {
    Project project;

    ModuleInstance first;
    first.id = "first";
    first.moduleId = "test";
    project.addModule(first);

    ModuleInstance second;
    second.id = "second";
    second.moduleId = "test";
    project.addModule(second);

    ModuleInstance third;
    third.id = "third";
    third.moduleId = "test";
    project.addModule(third);

    QVERIFY(project.moveModule("third", 0));
    QCOMPARE(project.modules().at(0).id, QString("third"));
    QCOMPARE(project.modules().at(1).id, QString("first"));
    QCOMPARE(project.modules().at(2).id, QString("second"));

    QVERIFY(!project.moveModule("missing", 1));
}

void TestProject::testConnections() {
    Project project;

    // 先添加两个模块
    ModuleInstance m1;
    m1.id = "m1";
    m1.moduleId = "test";
    project.addModule(m1);

    ModuleInstance m2;
    m2.id = "m2";
    m2.moduleId = "test";
    project.addModule(m2);

    // 添加连接
    ModuleConnection conn;
    conn.fromModuleId = "m1";
    conn.toModuleId = "m2";

    project.addConnection(conn);
    QCOMPARE(project.connections().size(), 1);

    project.removeConnection("m1", "m2");
    QCOMPARE(project.connections().size(), 0);
}

void TestProject::testCameras() {
    Project project;

    CameraConfig cam;
    cam.id = "cam-001";
    cam.type = "Basler";
    cam.serialNumber = "12345678";

    project.addCamera(cam);
    QCOMPARE(project.cameras().size(), 1);

    const CameraConfig* found = project.findCamera("cam-001");
    QVERIFY(found != nullptr);
    QCOMPARE(found->type, QString("Basler"));

    project.removeCamera("cam-001");
    QCOMPARE(project.cameras().size(), 0);
}

void TestProject::testSerialization() {
    Project project;
    project.setName("序列化测试");

    ModuleInstance m;
    m.id = "mod-1";
    m.moduleId = "test";
    m.name = "Test";
    project.addModule(m);

    QJsonObject json = project.toJson();

    QCOMPARE(json["name"].toString(), QString("序列化测试"));
    QVERIFY(json.contains("modules"));
    QVERIFY(json["modules"].toArray().size() == 1);

    Project project2;
    QVERIFY(project2.fromJson(json));
    QCOMPARE(project2.name(), QString("序列化测试"));
    QCOMPARE(project2.modules().size(), 1);
}

void TestProject::testSaveLoad() {
    // 创建并保存
    {
        Project project;
        project.setName("保存测试");

        ModuleInstance m;
        m.id = "save-test";
        m.moduleId = "test";
        project.addModule(m);

        QVERIFY(project.save(m_tempPath));
    }

    // 加载
    {
        Project project;
        QVERIFY(project.load(m_tempPath));

        QCOMPARE(project.name(), QString("保存测试"));
        QCOMPARE(project.modules().size(), 1);
    }

    QFile::remove(m_tempPath);
}

void TestProject::testAtomicSavePreservesExistingFileAndProjectState() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("atomic-project.dproj"));

    Project project;
    project.setName(QStringLiteral("sentinel-v3"));
    QVERIFY(project.save(path));
    const QString savedPath = project.filePath();
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = original.readAll();
    original.close();

    project.setName(QStringLiteral("dirty-after-sentinel"));
    QVERIFY(project.isModified());
    QSignalSpy modifiedSpy(&project, &Project::modifiedChanged);
    Project::failNextSaveAfterBytesForTesting(32);
    QVERIFY(!project.save());
    QCOMPARE(project.filePath(), savedPath);
    QVERIFY(project.isModified());
    QVERIFY(std::none_of(modifiedSpy.cbegin(), modifiedSpy.cend(),
                         [](const QList<QVariant>& args) { return !args.isEmpty() && !args.first().toBool(); }));
    QFile afterFailure(path);
    QVERIFY(afterFailure.open(QIODevice::ReadOnly));
    QCOMPARE(afterFailure.readAll(), originalBytes);

    QVERIFY(project.save());
    QVERIFY(!project.isModified());
    QCOMPARE(project.filePath(), savedPath);
    Project reloaded;
    QVERIFY(reloaded.load(path));
    QCOMPARE(reloaded.name(), QStringLiteral("dirty-after-sentinel"));
}

void TestProject::testAtomicSavePreservesSymlinkTargetsAndRejectsDanglingLinks() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("project-target.dproj"));
    const QString alias = dir.filePath(QStringLiteral("project-alias.dproj"));
    Project initialTarget;
    initialTarget.setName(QStringLiteral("existing-link-target"));
    QVERIFY(initialTarget.save(target));
    if (!QFile::link(target, alias) || !QFileInfo(alias).isSymLink())
        QSKIP("Symbolic links are unavailable in this environment");

    Project project;
    project.setName(QStringLiteral("symlink-save"));
    QVERIFY(project.save(alias));
    QVERIFY(QFileInfo(alias).isSymLink());
    QCOMPARE(project.filePath(), alias);
    Project targetProject;
    QVERIFY(targetProject.load(target));
    QCOMPARE(targetProject.name(), QStringLiteral("symlink-save"));

    const QString missingTarget = dir.filePath(QStringLiteral("missing-target.dproj"));
    const QString danglingAlias = dir.filePath(QStringLiteral("dangling-alias.dproj"));
    QVERIFY(QFile::link(missingTarget, danglingAlias));
    QVERIFY(QFileInfo(danglingAlias).isSymLink());
    Project danglingProject;
    danglingProject.setName(QStringLiteral("must-not-create-link-target"));
    const QString previousPath = danglingProject.filePath();
    QVERIFY(danglingProject.isModified());
    QVERIFY(!danglingProject.save(danglingAlias));
    QVERIFY(QFileInfo(danglingAlias).isSymLink());
    QVERIFY(!QFileInfo::exists(missingTarget));
    QCOMPARE(danglingProject.filePath(), previousPath);
    QVERIFY(danglingProject.isModified());
}

void TestProject::testModuleRuntimeFieldsRoundTrip() {
    Project project;
    ModuleInstance m;
    m.id = "n1";
    m.moduleId = "Blob";
    m.name = "斑点";
    m.note = "备注内容";
    m.enabled = false;
    m.breakpoint = true;
    project.addModule(m);

    // toJson 含三字段
    QJsonObject json = project.toJson();
    QJsonObject mj = json["modules"].toArray().first().toObject();
    QCOMPARE(mj["note"].toString(), QString("备注内容"));
    QCOMPARE(mj["enabled"].toBool(), false);
    QCOMPARE(mj["breakpoint"].toBool(), true);

    // 往返一致
    Project back;
    back.fromJson(json);
    QCOMPARE(back.modules().size(), 1);
    QCOMPARE(back.modules().first().note, QString("备注内容"));
    QCOMPARE(back.modules().first().enabled, false);
    QCOMPARE(back.modules().first().breakpoint, true);

    // 旧工程缺字段 → 默认 enabled=true, breakpoint=false
    QJsonObject legacy;
    legacy["id"] = "n2";
    legacy["moduleId"] = "Blob";
    QJsonObject projJson;
    projJson["version"] = "2.0";
    QJsonArray arr;
    arr.append(legacy);
    projJson["modules"] = arr;
    Project legacyProj;
    legacyProj.fromJson(projJson);
    QCOMPARE(legacyProj.modules().first().enabled, true);
    QCOMPARE(legacyProj.modules().first().breakpoint, false);
    QVERIFY(legacyProj.modules().first().note.isEmpty());
}

// 阶段1 §3（G1，审核整改 R1）：ProjectManager 共享守卫的 取消/保存/放弃 三态回归——
// 取消与保存失败保留当前工程;保存成功分支先落盘再切换;打开损坏文件仍先验证后替换
void TestProject::testUnsavedSwitchPoliciesAtManagerLevel() {
    ProjectManager& pm = ProjectManager::instance();
    pm.closeProject();

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Project* a = pm.newProjectChecked(ProjectManager::UnsavedPolicy::Cancel);
    QVERIFY(a);
    a->setName(QStringLiteral("A")); // setName → touch() → isModified
    QVERIFY(a->isModified());

    // Cancel：拒绝切换,保留当前工程,结构化错误码
    QString code, msg;
    QVERIFY(pm.newProjectChecked(ProjectManager::UnsavedPolicy::Cancel, &code, &msg) == nullptr);
    QCOMPARE(code, QStringLiteral("PROJECT_UNSAVED"));
    QCOMPARE(pm.currentProject(), a);

    // Save 但从未保存过（无路径）→ SAVE_FAILED,不继续
    QVERIFY(pm.newProjectChecked(ProjectManager::UnsavedPolicy::Save, &code, &msg) == nullptr);
    QCOMPARE(code, QStringLiteral("SAVE_FAILED"));
    QCOMPARE(pm.currentProject(), a);

    // Save 成功分支：A 先落盘;再修改后 Save 策略应先保存（文件含最新名 A2）再切换
    const QString pathA = QDir(dir.path()).filePath("a.dproj");
    QVERIFY(pm.saveProject(pathA));
    QVERIFY(!pm.currentProject()->isModified());
    a->setName(QStringLiteral("A2"));
    QVERIFY(a->isModified());
    Project* b = pm.newProjectChecked(ProjectManager::UnsavedPolicy::Save, &code, &msg);
    QVERIFY2(b != nullptr, qPrintable(msg));
    QVERIFY(pm.currentProject() == b);
    {
        QFile saved(pathA);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(saved.readAll()).contains(QStringLiteral("A2")),
                 "save policy must persist latest state before switching");
    }

    // Discard：修改后显式放弃 → 切换成功,不落盘
    b->setName(QStringLiteral("B"));
    QVERIFY(pm.currentProject()->isModified());
    Project* c = pm.newProjectChecked(ProjectManager::UnsavedPolicy::Discard);
    QVERIFY(c != nullptr);
    QVERIFY(pm.currentProject() == c);

    // openProjectChecked：Cancel 在触碰候选文件前即拒绝;Discard 放行后
    // 损坏文件打开失败仍保留原工程（先验证后替换）
    c->setName(QStringLiteral("C"));
    const QString broken = QDir(dir.path()).filePath("broken.dproj");
    {
        QFile bf(broken);
        QVERIFY(bf.open(QIODevice::WriteOnly));
        bf.write("{ this is not valid json ");
    }
    QVERIFY(pm.openProjectChecked(broken, ProjectManager::UnsavedPolicy::Cancel, &code, &msg) == nullptr);
    QCOMPARE(code, QStringLiteral("PROJECT_UNSAVED"));
    QCOMPARE(pm.currentProject()->name(), QStringLiteral("C"));
    code.clear();
    QVERIFY(pm.openProjectChecked(broken, ProjectManager::UnsavedPolicy::Discard, &code, &msg) == nullptr);
    QVERIFY2(code.isEmpty(), "failure must come from openProject itself, not the unsaved guard");
    QCOMPARE(pm.currentProject()->name(), QStringLiteral("C")); // 原工程保留

    // 复审2 R2：Save 策略带显式路径（GUI「另存为」语义）——无路径新工程也能兑现"保存后继续"
    Project* n = pm.newProjectChecked(ProjectManager::UnsavedPolicy::Discard);
    QVERIFY(n != nullptr);
    n->setName(QStringLiteral("N")); // 无路径且已修改
    QVERIFY(n->isModified());
    QVERIFY(n->filePath().isEmpty());
    const QString pathN = QDir(dir.path()).filePath("n.dproj");
    code.clear();
    Project* afterSaveAs = pm.newProjectChecked(ProjectManager::UnsavedPolicy::Save, &code, &msg, pathN);
    QVERIFY2(afterSaveAs != nullptr, qPrintable(msg)); // 另存为成功 → 切换
    QVERIFY(pm.currentProject() == afterSaveAs);
    {
        QFile saved(pathN);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(saved.readAll()).contains(QStringLiteral("N")),
                 "save-as must persist the project before switching");
    }
    // 另存为到不可写路径 → SAVE_FAILED,当前工程保留（保存失败不继续）
    afterSaveAs->setName(QStringLiteral("M")); // 已修改且无路径
    code.clear();
    QVERIFY(pm.newProjectChecked(ProjectManager::UnsavedPolicy::Save, &code, &msg,
                                 QStringLiteral("/nonexistent_dir_dlx/x.dproj")) == nullptr);
    QCOMPARE(code, QStringLiteral("SAVE_FAILED"));
    QCOMPARE(pm.currentProject()->name(), QStringLiteral("M"));

    // closeProjectChecked：Cancel 拒绝;Discard 放行
    QVERIFY(!pm.closeProjectChecked(ProjectManager::UnsavedPolicy::Cancel, &code, &msg));
    QCOMPARE(code, QStringLiteral("PROJECT_UNSAVED"));
    QVERIFY(pm.hasProject());
    QVERIFY(pm.closeProjectChecked(ProjectManager::UnsavedPolicy::Discard));
    QVERIFY(!pm.hasProject());
}

void TestProject::testUnsavedSaveFailureKeepsProjectForOpenNewAndClose() {
    ProjectManager& manager = ProjectManager::instance();
    manager.closeProject();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString currentPath = dir.filePath(QStringLiteral("current.dproj"));
    const QString candidatePath = dir.filePath(QStringLiteral("candidate.dproj"));
    Project* current = manager.newProject();
    QVERIFY(current);
    current->setName(QStringLiteral("save-failure-source"));
    QVERIFY(manager.saveProject(currentPath));
    current->setName(QStringLiteral("dirty-source-kept"));
    QVERIFY(current->isModified());
    const QJsonObject dirtySnapshot = current->toJson();

    Project candidate;
    candidate.setName(QStringLiteral("candidate"));
    QVERIFY(candidate.save(candidatePath));

    QFile sourceFile(currentPath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = sourceFile.readAll();
    sourceFile.close();
    QSignalSpy savedSpy(&manager, &ProjectManager::projectSaved);
    QSignalSpy openedSpy(&manager, &ProjectManager::projectOpened);
    QSignalSpy createdSpy(&manager, &ProjectManager::projectCreated);
    QSignalSpy closedSpy(&manager, &ProjectManager::projectClosed);
    QString errorCode;
    QString errorMessage;

    Project::failNextSaveAfterBytesForTesting(24);
    QVERIFY(manager.newProjectChecked(ProjectManager::UnsavedPolicy::Save, &errorCode, &errorMessage) == nullptr);
    QCOMPARE(errorCode, QStringLiteral("SAVE_FAILED"));
    QCOMPARE(manager.currentProject(), current);
    QVERIFY(current->isModified());
    QCOMPARE(current->toJson(), dirtySnapshot);

    errorCode.clear();
    Project::failNextSaveAfterBytesForTesting(24);
    QVERIFY(manager.openProjectChecked(candidatePath, ProjectManager::UnsavedPolicy::Save, &errorCode, &errorMessage) ==
            nullptr);
    QCOMPARE(errorCode, QStringLiteral("SAVE_FAILED"));
    QCOMPARE(manager.currentProject(), current);
    QVERIFY(current->isModified());
    QCOMPARE(current->toJson(), dirtySnapshot);

    errorCode.clear();
    Project::failNextSaveAfterBytesForTesting(24);
    QVERIFY(!manager.closeProjectChecked(ProjectManager::UnsavedPolicy::Save, &errorCode, &errorMessage));
    QCOMPARE(errorCode, QStringLiteral("SAVE_FAILED"));
    QCOMPARE(manager.currentProject(), current);
    QVERIFY(current->isModified());
    QCOMPARE(current->toJson(), dirtySnapshot);
    QCOMPARE(savedSpy.count(), 0);
    QCOMPARE(openedSpy.count(), 0);
    QCOMPARE(createdSpy.count(), 0);
    QCOMPARE(closedSpy.count(), 0);
    QFile sourceAfterFailure(currentPath);
    QVERIFY(sourceAfterFailure.open(QIODevice::ReadOnly));
    QCOMPARE(sourceAfterFailure.readAll(), originalBytes);
    QVERIFY(manager.closeProjectChecked(ProjectManager::UnsavedPolicy::Discard));
}

void TestProject::testOpenStructurallyInvalidProjectKeepsCurrent() {
    ProjectManager& manager = ProjectManager::instance();
    manager.closeProject();
    Project* current = manager.newProject();
    QVERIFY(current);
    current->setName(QStringLiteral("keep-me"));
    QVERIFY(current->isModified());
    QSignalSpy closedSpy(&manager, &ProjectManager::projectClosed);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("invalid.dproj"));
    const QList<QByteArray> invalidProjects = {
        "{}",
        R"({"version":"3.0","name":"x","modules":["bad"],"connections":[]})",
        R"({"version":"3.0","name":"x","modules":[{}],"connections":[]})",
        R"({"version":"3.0","name":"x","modules":[],"connections":["bad"]})",
        R"({"version":"3.0","name":"x","modules":[],"connections":[{}]})",
        R"({"version":"3.0","name":"x","modules":[{"id":"m","moduleId":"Blob","params":"bad"}],"connections":[]})",
        R"({"version":"3.0","name":"x","modules":[],"connections":[],"flows":"bad"})",
        R"({"version":"3.0","name":"x","modules":[],"connections":[],"resources":{"cameras":["bad"]}})",
        R"({"version":"3.0","name":"x","modules":[],"connections":[],"resources":{"dataSources":[{"id":"s","metadata":"bad"}]}})",
        R"({"version":"3.0","name":"x","modules":[],"connections":[],"migration":{"warnings":[4]}})",
        R"({"version":"3.0","name":"x","created":"bad","modules":[],"connections":[]})",
        R"({"version":"3.0","name":"x","modified":"bad","modules":[],"connections":[]})",
        R"({"version":"3.0","name":"x","modules":[],"connections":[],"resources":{"dataSources":[{"id":"s","importTime":"bad"}]}})",
        R"({"version":"3.0","name":"x","modules":[],"connections":[],"migration":{"migratedAt":"bad"}})",
        R"({"version":"3.0","name":"x","modules":[{"id":"m","moduleId":"Blob","posX":1.5}],"connections":[]})",
        R"({"version":"3.0","name":"x","modules":[],"connections":[],"resources":{"dataSources":[{"id":"s","importTime":1.5}]}})",
    };
    for (const QByteArray& contents : invalidProjects) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(contents), qint64(contents.size()));
        file.close();

        QVERIFY2(!manager.openProjectChecked(path, ProjectManager::UnsavedPolicy::Discard), contents.constData());
        QCOMPARE(manager.currentProject(), current);
        QCOMPARE(current->name(), QStringLiteral("keep-me"));
        QVERIFY(current->isModified());
        QCOMPARE(closedSpy.count(), 0);
    }
    manager.closeProject();
}

QTEST_MAIN(TestProject)
#include "test_project.moc"
