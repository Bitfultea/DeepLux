#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/common/Logger.h>
#include <core/engine/RunEngine.h>
#include <core/model/Project.h>

using namespace DeepLux;

// Test module implementation
class TestExecutionModule : public ModuleBase {
    Q_OBJECT

public:
    explicit TestExecutionModule(const QString& name, QObject* parent = nullptr) : ModuleBase(parent) {
        m_moduleId = "com.deeplux.test." + name;
        m_name = name;
        m_category = "test";
        m_author = "Test Author";
        m_description = "A test module for RunEngine";
    }

    bool executeCalled = false;
    bool executeResult = true;
    QStringList* executionLog = nullptr;

protected:
    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        Q_UNUSED(output)
        executeCalled = true;
        if (executionLog) {
            executionLog->append(instanceName().isEmpty() ? name() : instanceName());
        }
        return executeResult;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class TestRunEngine : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void testSingleton();
    void testRunOnce();
    void testCycleMode();
    void testPauseResume();
    void testStop();
    void testAddModule();
    void testRemoveModule();
    void testClearModules();
    void testModuleExecution();
    void testLoadProjectUsesInstanceIdsForDuplicateModuleNames();
    void testLoadProjectExecutesInConnectionTopologyOrder();
    void testLoadProjectRejectsConnectionWithMissingModule();
    void testLoadProjectRejectsConnectionCycle();
    void testModuleExecutionLogsElapsedTime();
    void testOutputManagement();
    void testStatistics();

private:
    void resetEngine();
};

void TestRunEngine::resetEngine() {
    RunEngine& engine = RunEngine::instance();
    engine.stop();
    engine.clearModules();
    engine.clearOutputs();
}

void TestRunEngine::initTestCase() {
    qDebug() << "=== TestRunEngine Start ===";
}

void TestRunEngine::cleanupTestCase() {
    qDebug() << "=== TestRunEngine End ===";
}

void TestRunEngine::init() {
    resetEngine();
}

void TestRunEngine::cleanup() {
    resetEngine();
}

void TestRunEngine::testSingleton() {
    RunEngine& instance1 = RunEngine::instance();
    RunEngine& instance2 = RunEngine::instance();

    QVERIFY(&instance1 == &instance2);
}

void TestRunEngine::testRunOnce() {
    RunEngine& engine = RunEngine::instance();

    TestExecutionModule* module = new TestExecutionModule("TestModule");
    module->initialize();
    engine.addModule(module);

    engine.runOnce();

    // Verify module was called
    QVERIFY(module->executeCalled);

    delete module;
}

void TestRunEngine::testCycleMode() {
    RunEngine& engine = RunEngine::instance();

    engine.setCycleMode(true);
    QVERIFY(engine.isCycleMode());
    QVERIFY(engine.runMode() == RunMode::RunCycle);

    engine.setCycleMode(false);
    QVERIFY(!engine.isCycleMode());
    QVERIFY(engine.runMode() == RunMode::None);
}

void TestRunEngine::testPauseResume() {
    RunEngine& engine = RunEngine::instance();

    TestExecutionModule* module = new TestExecutionModule("TestModule");
    module->initialize();
    engine.addModule(module);

    engine.start();
    QVERIFY(engine.isRunning());

    engine.pause();
    QVERIFY(engine.isPaused());
    QVERIFY(!engine.isRunning());

    engine.resume();
    QVERIFY(engine.isRunning());
    QVERIFY(!engine.isPaused());

    engine.stop();
    QVERIFY(engine.isStopped());

    delete module;
}

void TestRunEngine::testStop() {
    RunEngine& engine = RunEngine::instance();

    TestExecutionModule* module = new TestExecutionModule("TestModule");
    module->initialize();
    engine.addModule(module);

    engine.start();
    QVERIFY(engine.isRunning());

    engine.stop();
    QVERIFY(engine.isStopped());
    QVERIFY(!engine.isRunning());
    QVERIFY(!engine.isPaused());

    delete module;
}

void TestRunEngine::testAddModule() {
    RunEngine& engine = RunEngine::instance();

    TestExecutionModule* module = new TestExecutionModule("AddTestModule");
    module->initialize();

    engine.addModule(module);

    QVERIFY(engine.modules().size() == 1);
    QVERIFY(engine.getModule("AddTestModule") == module);

    delete module;
}

void TestRunEngine::testRemoveModule() {
    RunEngine& engine = RunEngine::instance();

    TestExecutionModule* module = new TestExecutionModule("RemoveTestModule");
    module->initialize();

    engine.addModule(module);
    QVERIFY(engine.modules().size() == 1);

    engine.removeModule(module->id());
    QVERIFY(engine.modules().size() == 0);
    QVERIFY(engine.getModule("RemoveTestModule") == nullptr);

    delete module;
}

void TestRunEngine::testClearModules() {
    RunEngine& engine = RunEngine::instance();

    TestExecutionModule* module1 = new TestExecutionModule("Module1");
    TestExecutionModule* module2 = new TestExecutionModule("Module2");
    module1->initialize();
    module2->initialize();

    engine.addModule(module1);
    engine.addModule(module2);
    QVERIFY(engine.modules().size() == 2);

    engine.clearModules();
    QVERIFY(engine.modules().size() == 0);

    delete module1;
    delete module2;
}

void TestRunEngine::testModuleExecution() {
    RunEngine& engine = RunEngine::instance();

    TestExecutionModule* module = new TestExecutionModule("ExecTestModule");
    module->initialize();
    engine.addModule(module);

    module->executeCalled = false;
    engine.runOnce();

    QVERIFY(module->executeCalled);

    delete module;
}

void TestRunEngine::testLoadProjectUsesInstanceIdsForDuplicateModuleNames() {
    RunEngine& engine = RunEngine::instance();
    Project project;

    ModuleInstance first;
    first.id = "instance_1";
    first.moduleId = "same-module";
    first.name = "Shared Module";
    project.addModule(first);

    ModuleInstance second;
    second.id = "instance_2";
    second.moduleId = "same-module";
    second.name = "Shared Module";
    project.addModule(second);

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) {
        Q_UNUSED(inst)
        return new TestExecutionModule("Shared Module");
    }));

    QCOMPARE(engine.modules().size(), 2);
    auto* firstModule = qobject_cast<TestExecutionModule*>(engine.getModule("instance_1"));
    auto* secondModule = qobject_cast<TestExecutionModule*>(engine.getModule("instance_2"));
    QVERIFY(firstModule != nullptr);
    QVERIFY(secondModule != nullptr);
    QVERIFY(firstModule != secondModule);

    engine.runOnce();

    QVERIFY(firstModule->executeCalled);
    QVERIFY(secondModule->executeCalled);
}

void TestRunEngine::testLoadProjectExecutesInConnectionTopologyOrder() {
    RunEngine& engine = RunEngine::instance();
    Project project;
    QStringList executionLog;

    ModuleInstance third;
    third.id = "C";
    third.moduleId = "test";
    third.name = "Third";
    project.addModule(third);

    ModuleInstance first;
    first.id = "A";
    first.moduleId = "test";
    first.name = "First";
    project.addModule(first);

    ModuleInstance second;
    second.id = "B";
    second.moduleId = "test";
    second.name = "Second";
    project.addModule(second);

    ModuleConnection firstToSecond;
    firstToSecond.fromModuleId = "A";
    firstToSecond.toModuleId = "B";
    project.addConnection(firstToSecond);

    ModuleConnection secondToThird;
    secondToThird.fromModuleId = "B";
    secondToThird.toModuleId = "C";
    project.addConnection(secondToThird);

    QVERIFY(engine.loadProject(&project, [&executionLog](const ModuleInstance& inst) {
        auto* module = new TestExecutionModule(inst.name);
        module->executionLog = &executionLog;
        return module;
    }));

    engine.runOnce();

    QCOMPARE(executionLog, QStringList({"A", "B", "C"}));
}

void TestRunEngine::testLoadProjectRejectsConnectionWithMissingModule() {
    RunEngine& engine = RunEngine::instance();
    Project project;

    ModuleInstance only;
    only.id = "A";
    only.moduleId = "test";
    only.name = "Only";
    project.addModule(only);

    ModuleConnection missing;
    missing.fromModuleId = "A";
    missing.toModuleId = "missing";
    project.addConnection(missing);

    QVERIFY(!engine.loadProject(&project, [](const ModuleInstance& inst) {
        Q_UNUSED(inst)
        return new TestExecutionModule("Only");
    }));
    QCOMPARE(engine.modules().size(), 0);
}

void TestRunEngine::testLoadProjectRejectsConnectionCycle() {
    RunEngine& engine = RunEngine::instance();
    Project project;

    ModuleInstance first;
    first.id = "A";
    first.moduleId = "test";
    first.name = "First";
    project.addModule(first);

    ModuleInstance second;
    second.id = "B";
    second.moduleId = "test";
    second.name = "Second";
    project.addModule(second);

    ModuleConnection firstToSecond;
    firstToSecond.fromModuleId = "A";
    firstToSecond.toModuleId = "B";
    project.addConnection(firstToSecond);

    ModuleConnection secondToFirst;
    secondToFirst.fromModuleId = "B";
    secondToFirst.toModuleId = "A";
    project.addConnection(secondToFirst);

    QVERIFY(!engine.loadProject(&project, [](const ModuleInstance& inst) {
        Q_UNUSED(inst)
        return new TestExecutionModule("Cycle");
    }));
    QCOMPARE(engine.modules().size(), 0);
}

void TestRunEngine::testModuleExecutionLogsElapsedTime() {
    Logger::instance().clearLogs();

    RunEngine& engine = RunEngine::instance();
    TestExecutionModule* module = new TestExecutionModule("TimedModule");
    module->initialize();
    engine.addModule(module);

    engine.runOnce();

    const QList<LogEntry> logs = Logger::instance().logs("Run");
    bool found = false;
    for (const LogEntry& entry : logs) {
        if (entry.message.contains("Module finished: TimedModule") && entry.message.contains("elapsed=") &&
            entry.message.contains("ms")) {
            found = true;
            break;
        }
    }
    QVERIFY2(found, "Run log should include per-module elapsed time");

    delete module;
}

void TestRunEngine::testOutputManagement() {
    RunEngine& engine = RunEngine::instance();

    engine.setOutput("Module1", "var1", 100);
    engine.setOutput("Module1", "var2", "test");

    QVERIFY(engine.hasOutput("Module1", "var1"));
    QVERIFY(engine.hasOutput("Module1", "var2"));
    QVERIFY(!engine.hasOutput("Module1", "nonexistent"));

    QVariant val1 = engine.getOutput("Module1", "var1");
    QVariant val2 = engine.getOutput("Module1", "var2");

    QCOMPARE(val1.toInt(), 100);
    QCOMPARE(val2.toString(), QString("test"));

    engine.clearOutputs();
    QVERIFY(!engine.hasOutput("Module1", "var1"));
}

void TestRunEngine::testStatistics() {
    RunEngine& engine = RunEngine::instance();

    TestExecutionModule* module = new TestExecutionModule("StatTestModule");
    module->initialize();
    engine.addModule(module);

    // Run a few times
    engine.runOnce();
    engine.runOnce();
    engine.runOnce();

    QVERIFY(engine.totalRuns() >= 3);

    delete module;
}

QTEST_MAIN(TestRunEngine)
#include "test_runengine.moc"
