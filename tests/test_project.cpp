#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QtTest/QtTest>
#include <core/model/Project.h>

using namespace DeepLux;

class TestProject : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testCreate();
    void testName();
    void testModules();
    void testSetModuleParam();
    void testMoveModule();
    void testConnections();
    void testCameras();
    void testSerialization();
    void testSaveLoad();
    void testModuleRuntimeFieldsRoundTrip();

    // B2: 布局/结构 API
    void testSetModulePosition();
    void testReorderModules();
    void testLayoutInitializedRoundTrip();
    void testSessionTokenDiffersForNewAndLoadedProject();
    void testStructureFingerprintSensitiveToConnectionDeclarationOrder();
    void testApplyStructureTransaction();
    void testApplyStructureTransactionFailureLeavesStateUntouched();

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

// ---------------------------------------------------------------------------
// B2: 布局/结构 API
// ---------------------------------------------------------------------------

namespace {

ModuleInstance makeModule(const QString& id) {
    ModuleInstance module;
    module.id = id;
    module.moduleId = QStringLiteral("test.module");
    module.name = id;
    return module;
}

ModuleConnection makeConnection(const QString& from, const QString& to, const QString& port = QStringLiteral("image")) {
    ModuleConnection connection;
    connection.fromModuleId = from;
    connection.toModuleId = to;
    connection.fromPort = port;
    connection.toPort = port;
    connection.edgeType = QStringLiteral("data");
    return connection;
}

} // namespace

void TestProject::testSetModulePosition() {
    Project project;
    project.addModule(makeModule(QStringLiteral("m1")));
    project.setModified(false);

    QSignalSpy positionSpy(&project, &Project::modulePositionChanged);
    QSignalSpy updatedSpy(&project, &Project::moduleUpdated);
    QVERIFY(positionSpy.isValid());
    QVERIFY(updatedSpy.isValid());

    project.setModulePosition(QStringLiteral("m1"), 120, 340);

    QCOMPARE(project.moduleById(QStringLiteral("m1"))->posX, 120);
    QCOMPARE(project.moduleById(QStringLiteral("m1"))->posY, 340);
    QCOMPARE(positionSpy.count(), 1);
    QCOMPARE(updatedSpy.count(), 0);
    QVERIFY(project.isModified());

    const QList<QVariant> args = positionSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("m1"));
    QCOMPARE(args.at(1).toInt(), 120);
    QCOMPARE(args.at(2).toInt(), 340);

    // 未知模块不改变任何状态
    project.setModified(false);
    project.setModulePosition(QStringLiteral("missing"), 1, 2);
    QCOMPARE(positionSpy.count(), 0);
    QVERIFY(!project.isModified());
}

void TestProject::testReorderModules() {
    Project project;
    project.addModule(makeModule(QStringLiteral("a")));
    project.addModule(makeModule(QStringLiteral("b")));
    project.addModule(makeModule(QStringLiteral("c")));

    QSignalSpy reorderedSpy(&project, &Project::modulesReordered);
    QVERIFY(reorderedSpy.isValid());

    QVERIFY(project.reorderModules(QStringList() << QStringLiteral("c") << QStringLiteral("a") << QStringLiteral("b")));
    QCOMPARE(project.modules().at(0).id, QStringLiteral("c"));
    QCOMPARE(project.modules().at(1).id, QStringLiteral("a"));
    QCOMPARE(project.modules().at(2).id, QStringLiteral("b"));
    QCOMPARE(reorderedSpy.count(), 1);

    // 不完整/重复/未知顺序必须拒绝且不改变状态
    const int before = reorderedSpy.count();
    QVERIFY(!project.reorderModules(QStringList() << QStringLiteral("a") << QStringLiteral("b")));
    QVERIFY(
        !project.reorderModules(QStringList() << QStringLiteral("a") << QStringLiteral("a") << QStringLiteral("b")));
    QVERIFY(
        !project.reorderModules(QStringList() << QStringLiteral("a") << QStringLiteral("b") << QStringLiteral("x")));
    QCOMPARE(reorderedSpy.count(), before);
    QCOMPARE(project.modules().at(0).id, QStringLiteral("c"));
}

void TestProject::testLayoutInitializedRoundTrip() {
    Project project;
    QVERIFY(!project.layoutInitialized());

    project.setLayoutInitialized(true);
    QVERIFY(project.layoutInitialized());

    const QJsonObject json = project.toJson();
    QVERIFY(json.contains("layoutInitialized"));
    QVERIFY(json.value("layoutInitialized").toBool());

    Project restored;
    QVERIFY(restored.fromJson(json));
    QVERIFY(restored.layoutInitialized());

    // 旧工程缺少字段 => false
    QJsonObject legacy = json;
    legacy.remove("layoutInitialized");
    Project oldProject;
    QVERIFY(oldProject.fromJson(legacy));
    QVERIFY(!oldProject.layoutInitialized());
}

void TestProject::testSessionTokenDiffersForNewAndLoadedProject() {
    Project first;
    Project second;
    QVERIFY(!first.sessionToken().isNull());
    QVERIFY(!second.sessionToken().isNull());
    QVERIFY(first.sessionToken() != second.sessionToken());

    Project loaded;
    QVERIFY(loaded.fromJson(first.toJson()));
    QVERIFY(!loaded.sessionToken().isNull());
    QVERIFY(loaded.sessionToken() != first.sessionToken());
}

void TestProject::testStructureFingerprintSensitiveToConnectionDeclarationOrder() {
    Project project;
    project.addModule(makeModule(QStringLiteral("A")));
    project.addModule(makeModule(QStringLiteral("B")));
    project.addModule(makeModule(QStringLiteral("C")));

    project.addConnection(makeConnection(QStringLiteral("A"), QStringLiteral("B")));
    project.addConnection(makeConnection(QStringLiteral("A"), QStringLiteral("C")));
    const QString firstFingerprint = project.structureFingerprint();
    QVERIFY(!firstFingerprint.isEmpty());

    // 重新构造同样两个连接，但声明顺序交换
    Project swapped;
    swapped.addModule(makeModule(QStringLiteral("A")));
    swapped.addModule(makeModule(QStringLiteral("B")));
    swapped.addModule(makeModule(QStringLiteral("C")));
    swapped.addConnection(makeConnection(QStringLiteral("A"), QStringLiteral("C")));
    swapped.addConnection(makeConnection(QStringLiteral("A"), QStringLiteral("B")));

    QVERIFY(firstFingerprint != swapped.structureFingerprint());
}

void TestProject::testApplyStructureTransaction() {
    Project project;
    project.addModule(makeModule(QStringLiteral("a")));
    project.addModule(makeModule(QStringLiteral("b")));
    project.addModule(makeModule(QStringLiteral("c")));
    project.addConnection(makeConnection(QStringLiteral("a"), QStringLiteral("b")));

    QSignalSpy structureSpy(&project, &Project::structureChanged);
    QVERIFY(structureSpy.isValid());

    FlowGraphCandidate candidate;
    candidate.orderedIds = QStringList() << QStringLiteral("c") << QStringLiteral("a") << QStringLiteral("b");
    candidate.connections = {makeConnection(QStringLiteral("b"), QStringLiteral("c"))};

    QString error = QStringLiteral("sentinel");
    QVERIFY(project.applyStructureTransaction(candidate, StructureEditOrigin::Agent, &error));
    QVERIFY(error.isEmpty());

    QCOMPARE(project.modules().size(), 3);
    QCOMPARE(project.modules().at(0).id, QStringLiteral("c"));
    QCOMPARE(project.modules().at(1).id, QStringLiteral("a"));
    QCOMPARE(project.modules().at(2).id, QStringLiteral("b"));
    QCOMPARE(project.connections().size(), 1);
    QCOMPARE(project.connections().first().fromModuleId, QStringLiteral("b"));
    QCOMPARE(project.connections().first().toModuleId, QStringLiteral("c"));
    QCOMPARE(structureSpy.count(), 1);
    QCOMPARE(static_cast<int>(structureSpy.takeFirst().at(0).value<StructureEditOrigin>()),
             static_cast<int>(StructureEditOrigin::Agent));
    QVERIFY(project.isModified());
}

void TestProject::testApplyStructureTransactionFailureLeavesStateUntouched() {
    Project project;
    project.addModule(makeModule(QStringLiteral("a")));
    project.addModule(makeModule(QStringLiteral("b")));
    project.addModule(makeModule(QStringLiteral("c")));
    project.addConnection(makeConnection(QStringLiteral("a"), QStringLiteral("b")));
    project.setModified(false);

    const QString originalFingerprint = project.structureFingerprint();
    const QStringList originalOrder = {project.modules().at(0).id, project.modules().at(1).id,
                                       project.modules().at(2).id};

    QSignalSpy structureSpy(&project, &Project::structureChanged);
    QSignalSpy moduleAddedSpy(&project, &Project::moduleAdded);
    QSignalSpy connectionAddedSpy(&project, &Project::connectionAdded);

    QList<FlowGraphCandidate> invalidCandidates;

    FlowGraphCandidate missingModule;
    missingModule.orderedIds = QStringList() << QStringLiteral("a") << QStringLiteral("b");
    invalidCandidates.append(missingModule);

    FlowGraphCandidate duplicateId;
    duplicateId.orderedIds = QStringList() << QStringLiteral("a") << QStringLiteral("a") << QStringLiteral("b");
    invalidCandidates.append(duplicateId);

    FlowGraphCandidate unknownId;
    unknownId.orderedIds = QStringList() << QStringLiteral("a") << QStringLiteral("b") << QStringLiteral("x");
    invalidCandidates.append(unknownId);

    FlowGraphCandidate incompletePermutation;
    incompletePermutation.orderedIds = QStringList() << QStringLiteral("a") << QStringLiteral("b")
                                                     << QStringLiteral("c") << QStringLiteral("d");
    invalidCandidates.append(incompletePermutation);

    FlowGraphCandidate unknownEndpoint;
    unknownEndpoint.orderedIds = QStringList() << QStringLiteral("a") << QStringLiteral("b") << QStringLiteral("c");
    unknownEndpoint.connections = {makeConnection(QStringLiteral("a"), QStringLiteral("x"))};
    invalidCandidates.append(unknownEndpoint);

    FlowGraphCandidate duplicateEdge;
    duplicateEdge.orderedIds = QStringList() << QStringLiteral("a") << QStringLiteral("b") << QStringLiteral("c");
    duplicateEdge.connections = {makeConnection(QStringLiteral("a"), QStringLiteral("b")),
                                 makeConnection(QStringLiteral("a"), QStringLiteral("b"))};
    invalidCandidates.append(duplicateEdge);

    for (const FlowGraphCandidate& candidate : invalidCandidates) {
        QString error;
        QVERIFY(!project.applyStructureTransaction(candidate, StructureEditOrigin::Gui, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!project.isModified());
        QCOMPARE(project.structureFingerprint(), originalFingerprint);
        QCOMPARE(project.modules().size(), 3);
        for (int i = 0; i < originalOrder.size(); ++i)
            QCOMPARE(project.modules().at(i).id, originalOrder.at(i));
        QCOMPARE(structureSpy.count(), 0);
        QCOMPARE(moduleAddedSpy.count(), 0);
        QCOMPARE(connectionAddedSpy.count(), 0);
    }
}

QTEST_MAIN(TestProject)
#include "test_project.moc"
