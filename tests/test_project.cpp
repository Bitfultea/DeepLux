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

QTEST_MAIN(TestProject)
#include "test_project.moc"
