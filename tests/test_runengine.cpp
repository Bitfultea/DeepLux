#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/common/CancellationToken.h>
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
    int executeCount = 0;
    bool executeResult = true;
    QString errorText;
    QString secondErrorText;
    QStringList* executionLog = nullptr;
    QString outputTag;
    QString receivedTag;
    QString controlResultKey;
    bool controlResult = true;
    ControlFlowType controlType = ControlFlowType::Sequential;

    ControlFlowType flowControlType() const override {
        return controlType;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        executeCalled = true;
        ++executeCount;
        if (executionLog) {
            executionLog->append(instanceName().isEmpty() ? name() : instanceName());
        }
        receivedTag = input.data(QStringLiteral("tag")).toString();
        if (outputTag.isEmpty()) {
            output = input;
        } else {
            QImage image(8, 8, QImage::Format_RGB32);
            image.fill(Qt::white);
            output = ImageData(image);
            output.setData("tag", outputTag);
        }
        if (!controlResultKey.isEmpty()) {
            output.setData(controlResultKey, controlResult);
        }
        if (!executeResult && !errorText.isEmpty()) {
            emit errorOccurred(errorText);
        }
        if (!executeResult && !secondErrorText.isEmpty()) {
            emit errorOccurred(secondErrorText);
        }
        return executeResult;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class StopAwareModule : public ModuleBase {
    Q_OBJECT

public:
    StopAwareModule() {
        m_moduleId = "com.deeplux.test.stopaware";
        m_name = "StopAware";
        m_category = "test";
    }

    bool hadCancellationToken = false;
    bool sawCancellation = false;

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        hadCancellationToken = cancellationToken() != nullptr;
        RunEngine::instance().stop();
        sawCancellation = isCancellationRequested();
        return true;
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
    void testStopPropagatesCancellationToRunningModule();
    void testAddModule();
    void testRemoveModule();
    void testClearModules();
    void testModuleExecution();
    void testFailedRunReportsFailureAndPreservesModuleError();
    void testFalseControlResultIsNotExecutionFailure();
    void testConditionalWithoutEndSkipsOneBodyModule();
    void testWhileFalseSkipsLoopBody();
    void testWhileWithoutEndSkipsOneBodyModule();
    void testLoopWithoutEndRepeatsOneBodyModule();
    void testEmptyCycleDoesNotRemainBusy();
    void testStepOnceExecutesOneModulePerClick();
    void testStepOnceStoresIntermediateOutputByModule();
    void testLoadProjectUsesInstanceIdsForDuplicateModuleNames();
    void testLoadProjectExecutesInConnectionTopologyOrder();
    void testConnectedFlowIgnoresIsolatedModuleData();
    void testLoadProjectRejectsDisconnectedConnectedChains();
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

void TestRunEngine::testStopPropagatesCancellationToRunningModule() {
    RunEngine& engine = RunEngine::instance();

    StopAwareModule* module = new StopAwareModule();
    module->initialize();
    engine.addModule(module);

    engine.runOnce();

    QVERIFY(module->hadCancellationToken);
    QVERIFY(module->sawCancellation);
    QVERIFY(engine.cancellationToken()->isCancelled());

    engine.clearModules();
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

void TestRunEngine::testFailedRunReportsFailureAndPreservesModuleError() {
    RunEngine& engine = RunEngine::instance();
    auto* module = new TestExecutionModule("FailingModule");
    module->executeResult = false;
    module->errorText = QStringLiteral("camera unavailable");
    module->secondErrorText = QStringLiteral("operation cancelled");
    module->initialize();
    engine.addModule(module);

    RunResult result{};
    bool received = false;
    const int failuresBefore = engine.failedRuns();
    const QMetaObject::Connection connection =
        connect(&engine, &RunEngine::runFinished, this, [&result, &received](const RunResult& value) {
            result = value;
            received = true;
        });

    engine.runOnce();
    disconnect(connection);

    QVERIFY(received);
    QVERIFY(!result.success);
    QCOMPARE(result.errorMessage, QStringLiteral("camera unavailable"));
    QCOMPARE(engine.failedRuns(), failuresBefore + 1);
    QCOMPARE(engine.moduleOutput(QStringLiteral("FailingModule")).data(QStringLiteral("error")).toString(),
             QStringLiteral("camera unavailable"));

    engine.clearModules();
    delete module;
}

void TestRunEngine::testFalseControlResultIsNotExecutionFailure() {
    RunEngine& engine = RunEngine::instance();
    QStringList executionLog;

    auto* condition = new TestExecutionModule(QStringLiteral("Condition"));
    condition->controlType = ControlFlowType::Conditional;
    condition->controlResultKey = QStringLiteral("condition_result");
    condition->controlResult = false;
    condition->executionLog = &executionLog;
    auto* body = new TestExecutionModule(QStringLiteral("Body"));
    body->executionLog = &executionLog;
    auto* end = new TestExecutionModule(QStringLiteral("End"));
    end->controlType = ControlFlowType::ConditionalEnd;
    end->executionLog = &executionLog;
    auto* after = new TestExecutionModule(QStringLiteral("After"));
    after->executionLog = &executionLog;

    for (TestExecutionModule* module : {condition, body, end, after}) {
        module->initialize();
        engine.addModule(module);
    }

    RunResult lastResult{};
    const QMetaObject::Connection resultConnection = connect(
        &engine, &RunEngine::runFinished, this, [&lastResult](const RunResult& result) { lastResult = result; });
    engine.runOnce();

    QVERIFY(lastResult.success);
    QCOMPARE(executionLog, QStringList({QStringLiteral("Condition"), QStringLiteral("After")}));

    executionLog.clear();
    QVERIFY(engine.stepOnce());
    QVERIFY(lastResult.success);
    QCOMPARE(executionLog, QStringList({QStringLiteral("Condition")}));
    QVERIFY(engine.stepOnce());
    QVERIFY(lastResult.success);
    QCOMPARE(executionLog, QStringList({QStringLiteral("Condition"), QStringLiteral("After")}));
    disconnect(resultConnection);

    engine.clearModules();
    delete condition;
    delete body;
    delete end;
    delete after;
}

void TestRunEngine::testConditionalWithoutEndSkipsOneBodyModule() {
    RunEngine& engine = RunEngine::instance();
    QStringList executionLog;

    auto* condition = new TestExecutionModule(QStringLiteral("Condition"));
    condition->controlType = ControlFlowType::Conditional;
    condition->controlResultKey = QStringLiteral("condition_result");
    condition->controlResult = false;
    condition->executionLog = &executionLog;
    auto* body = new TestExecutionModule(QStringLiteral("Body"));
    body->executionLog = &executionLog;
    auto* after = new TestExecutionModule(QStringLiteral("After"));
    after->executionLog = &executionLog;

    for (TestExecutionModule* module : {condition, body, after}) {
        module->initialize();
        engine.addModule(module);
    }

    engine.runOnce();

    QCOMPARE(executionLog, QStringList({QStringLiteral("Condition"), QStringLiteral("After")}));

    engine.clearModules();
    delete condition;
    delete body;
    delete after;
}

void TestRunEngine::testWhileFalseSkipsLoopBody() {
    RunEngine& engine = RunEngine::instance();
    QStringList executionLog;

    auto* condition = new TestExecutionModule(QStringLiteral("While"));
    condition->controlType = ControlFlowType::While;
    condition->controlResultKey = QStringLiteral("while_result");
    condition->controlResult = false;
    condition->executionLog = &executionLog;
    auto* body = new TestExecutionModule(QStringLiteral("Body"));
    body->executionLog = &executionLog;
    auto* end = new TestExecutionModule(QStringLiteral("WhileEnd"));
    end->controlType = ControlFlowType::WhileEnd;
    end->executionLog = &executionLog;
    auto* after = new TestExecutionModule(QStringLiteral("After"));
    after->executionLog = &executionLog;

    for (TestExecutionModule* module : {condition, body, end, after}) {
        module->initialize();
        engine.addModule(module);
    }

    engine.runOnce();

    QCOMPARE(executionLog, QStringList({QStringLiteral("While"), QStringLiteral("After")}));

    engine.clearModules();
    delete condition;
    delete body;
    delete end;
    delete after;
}

void TestRunEngine::testWhileWithoutEndSkipsOneBodyModule() {
    RunEngine& engine = RunEngine::instance();
    QStringList executionLog;

    auto* condition = new TestExecutionModule(QStringLiteral("While"));
    condition->controlType = ControlFlowType::While;
    condition->controlResultKey = QStringLiteral("while_result");
    condition->controlResult = false;
    condition->executionLog = &executionLog;
    auto* body = new TestExecutionModule(QStringLiteral("Body"));
    body->executionLog = &executionLog;
    auto* after = new TestExecutionModule(QStringLiteral("After"));
    after->executionLog = &executionLog;

    for (TestExecutionModule* module : {condition, body, after}) {
        module->initialize();
        engine.addModule(module);
    }

    engine.runOnce();

    QCOMPARE(executionLog, QStringList({QStringLiteral("While"), QStringLiteral("After")}));

    engine.clearModules();
    delete condition;
    delete body;
    delete after;
}

void TestRunEngine::testLoopWithoutEndRepeatsOneBodyModule() {
    RunEngine& engine = RunEngine::instance();

    auto* loop = new TestExecutionModule(QStringLiteral("Loop"));
    loop->controlType = ControlFlowType::Loop;
    loop->setParam(QStringLiteral("loopCount"), 3);
    auto* body = new TestExecutionModule(QStringLiteral("Body"));
    auto* after = new TestExecutionModule(QStringLiteral("After"));

    for (TestExecutionModule* module : {loop, body, after}) {
        module->initialize();
        engine.addModule(module);
    }

    engine.runOnce();

    QCOMPARE(body->executeCount, 3);
    QCOMPARE(after->executeCount, 1);

    engine.clearModules();
    delete loop;
    delete body;
    delete after;
}

void TestRunEngine::testEmptyCycleDoesNotRemainBusy() {
    RunEngine& engine = RunEngine::instance();
    QSignalSpy finishedSpy(&engine, &RunEngine::runFinished);

    engine.start();

    QVERIFY(!engine.isBusy());
    QVERIFY(!engine.isCycleMode());
    QCOMPARE(finishedSpy.count(), 1);
    QTest::qWait(250);
    QCOMPARE(finishedSpy.count(), 1);
}

void TestRunEngine::testStepOnceExecutesOneModulePerClick() {
    RunEngine& engine = RunEngine::instance();
    QStringList executionLog;

    TestExecutionModule* first = new TestExecutionModule("First");
    TestExecutionModule* second = new TestExecutionModule("Second");
    first->executionLog = &executionLog;
    second->executionLog = &executionLog;
    first->initialize();
    second->initialize();
    engine.addModule(first);
    engine.addModule(second);

    QVERIFY(engine.stepOnce());
    QCOMPARE(executionLog, QStringList({"First"}));
    QVERIFY(first->executeCalled);
    QVERIFY(!second->executeCalled);

    first->executeCalled = false;
    QVERIFY(engine.stepOnce());
    QCOMPARE(executionLog, QStringList({"First", "Second"}));
    QVERIFY(!first->executeCalled);
    QVERIFY(second->executeCalled);

    second->executeCalled = false;
    QVERIFY(engine.stepOnce());
    QCOMPARE(executionLog, QStringList({"First", "Second", "First"}));
    QVERIFY(first->executeCalled);
    QVERIFY(!second->executeCalled);

    engine.clearModules();
    delete first;
    delete second;
}

void TestRunEngine::testStepOnceStoresIntermediateOutputByModule() {
    RunEngine& engine = RunEngine::instance();

    TestExecutionModule* first = new TestExecutionModule("First");
    TestExecutionModule* second = new TestExecutionModule("Second");
    first->setInstanceName(QStringLiteral("first_1"));
    second->setInstanceName(QStringLiteral("second_1"));
    first->outputTag = QStringLiteral("first");
    second->outputTag = QStringLiteral("second");
    first->initialize();
    second->initialize();
    engine.addModule(first);
    engine.addModule(second);

    QVERIFY(engine.stepOnce());
    QCOMPARE(engine.moduleOutput(QStringLiteral("first_1")).data(QStringLiteral("tag")).toString(),
             QStringLiteral("first"));
    QVERIFY(!engine.moduleOutput(QStringLiteral("second_1")).isValid());

    QVERIFY(engine.stepOnce());
    QCOMPARE(engine.moduleOutput(QStringLiteral("second_1")).data(QStringLiteral("tag")).toString(),
             QStringLiteral("second"));

    engine.clearModules();
    delete first;
    delete second;
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

void TestRunEngine::testConnectedFlowIgnoresIsolatedModuleData() {
    RunEngine& engine = RunEngine::instance();
    Project project;
    QStringList executionLog;

    for (const QString& id : {QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("B")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = QStringLiteral("test");
        instance.name = id;
        project.addModule(instance);
    }
    ModuleConnection connection;
    connection.fromModuleId = QStringLiteral("A");
    connection.toModuleId = QStringLiteral("B");
    project.addConnection(connection);

    QVERIFY(engine.loadProject(&project, [&executionLog](const ModuleInstance& instance) {
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &executionLog;
        module->outputTag = instance.id;
        return module;
    }));

    engine.runOnce();

    QCOMPARE(executionLog, QStringList({QStringLiteral("A"), QStringLiteral("B")}));
    auto* downstream = qobject_cast<TestExecutionModule*>(engine.getModule(QStringLiteral("B")));
    QVERIFY(downstream != nullptr);
    QCOMPARE(downstream->receivedTag, QStringLiteral("A"));
    QVERIFY(!engine.moduleOutput(QStringLiteral("C")).isValid());
}

void TestRunEngine::testLoadProjectRejectsDisconnectedConnectedChains() {
    RunEngine& engine = RunEngine::instance();
    Project project;

    for (const QString& id : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = QStringLiteral("test");
        instance.name = id;
        project.addModule(instance);
    }
    ModuleConnection first;
    first.fromModuleId = QStringLiteral("A");
    first.toModuleId = QStringLiteral("B");
    project.addConnection(first);
    ModuleConnection second;
    second.fromModuleId = QStringLiteral("C");
    second.toModuleId = QStringLiteral("D");
    project.addConnection(second);

    QVERIFY(!engine.loadProject(&project,
                                [](const ModuleInstance& instance) { return new TestExecutionModule(instance.id); }));
    QVERIFY(engine.modules().isEmpty());
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
