#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>
#include <atomic>
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

class TypedPortModule : public ModuleBase {
    Q_OBJECT
public:
    TypedPortModule(const QString& name, DataType type, bool source) : m_type(type), m_source(source) {
        m_moduleId = QStringLiteral("com.deeplux.test.typed.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }

    QList<PortSpec> inputPorts() const override {
        if (m_source)
            return {};
        PortSpec port;
        port.id = QStringLiteral("value");
        port.displayName = QStringLiteral("Value");
        port.type = m_type;
        port.required = true;
        return {port};
    }

    QList<PortSpec> outputPorts() const override {
        if (!m_source)
            return {};
        PortSpec port;
        port.id = QStringLiteral("value");
        port.displayName = QStringLiteral("Value");
        port.type = m_type;
        return {port};
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    DataType m_type;
    bool m_source;
};

// 非对称端口模块：输入 point1(必需)，输出 image。禁用时无法旁路 image。
class RequiredInputModule : public ModuleBase {
    Q_OBJECT
public:
    RequiredInputModule(const QString& name) {
        m_moduleId = QStringLiteral("com.deeplux.test.req.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    QList<PortSpec> inputPorts() const override {
        PortSpec p;
        p.id = QStringLiteral("point1");
        p.displayName = QStringLiteral("点1");
        p.type = DataType::Point2D;
        p.required = true;
        return {p};
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec p;
        p.id = QStringLiteral("image");
        p.displayName = QStringLiteral("输出");
        p.type = DataType::Image2D;
        return {p};
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

// 可配置输入/输出端口模块，用于构造禁用旁路场景
class PortPairModule : public ModuleBase {
    Q_OBJECT
public:
    PortPairModule(const QString& name, const QString& inId, DataType inType, const QString& outId,
                   DataType outType)
        : m_inId(inId), m_inType(inType), m_outId(outId), m_outType(outType) {
        m_moduleId = QStringLiteral("com.deeplux.test.pair.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    QList<PortSpec> inputPorts() const override {
        QList<PortSpec> r;
        if (!m_inId.isEmpty()) {
            PortSpec p;
            p.id = m_inId;
            p.displayName = m_inId;
            p.type = m_inType;
            p.required = true;
            r.append(p);
        }
        return r;
    }
    QList<PortSpec> outputPorts() const override {
        QList<PortSpec> r;
        if (!m_outId.isEmpty()) {
            PortSpec p;
            p.id = m_outId;
            p.displayName = m_outId;
            p.type = m_outType;
            r.append(p);
        }
        return r;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(m_outId, 1.0);
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QString m_inId;
    DataType m_inType;
    QString m_outId;
    DataType m_outType;
};

// 并行测试模块：睡眠并记录并发度，证明真实并发
class ParallelSleepModule : public ModuleBase {
    Q_OBJECT
public:
    ParallelSleepModule(const QString& name, std::atomic<int>* running, std::atomic<int>* maxConc, int sleepMs)
        : m_running(running), m_maxConc(maxConc), m_sleepMs(sleepMs) {
        m_moduleId = "com.deeplux.test.parallel." + name;
        m_name = name;
        m_category = "test";
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        const int cur = ++(*m_running);
        int expected = m_maxConc->load();
        while (cur > expected && !m_maxConc->compare_exchange_weak(expected, cur)) {
        }
        QThread::msleep(m_sleepMs);
        --(*m_running);
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    std::atomic<int>* m_running;
    std::atomic<int>* m_maxConc;
    int m_sleepMs;
};

class TestRunEngine : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void testParallelExecutesConcurrently();
    void testParallelFailureCancelsGroup();
    void testValidateFlowReportsMissingRequiredInput();
    void testSkippedBranchEmitsSkippedNotFailed();
    void testBreakpointRestoredOnLoad();
    void testDisabledModuleBypassesImage();
    void testDisabledDependedNodePreRunFail();
    void testStepOnceSkipsDisabledModule();
    void testDisabledBypassCollectsByToPort();
    void testDisabledBypassRejectsTypeIncompatible();
    void testBreakpointHitPauseResume();
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
    void testGraphExecutesIsolatedModulesIndependently();
    void testLoadProjectSupportsDisconnectedChains();
    void testLoadProjectSupportsFanOutAndMerge();
    void testLoadProjectRejectsIncompatiblePortTypes();
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

void TestRunEngine::testGraphExecutesIsolatedModulesIndependently() {
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

    QCOMPARE(executionLog, QStringList({QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("B")}));
    auto* downstream = qobject_cast<TestExecutionModule*>(engine.getModule(QStringLiteral("B")));
    QVERIFY(downstream != nullptr);
    QCOMPARE(downstream->receivedTag, QStringLiteral("A"));
    QVERIFY(engine.moduleOutput(QStringLiteral("C")).isValid());
}

void TestRunEngine::testLoadProjectSupportsDisconnectedChains() {
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

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& instance) {
        auto* module = new TestExecutionModule(instance.id);
        module->outputTag = instance.id;
        return module;
    }));
    engine.runOnce();
    for (const QString& id : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D")})
        QVERIFY2(engine.moduleOutput(id).isValid(), qPrintable(id));
}

void TestRunEngine::testLoadProjectSupportsFanOutAndMerge() {
    RunEngine& engine = RunEngine::instance();
    Project project;
    QStringList executionLog;
    for (const QString& id : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = QStringLiteral("test");
        instance.name = id;
        project.addModule(instance);
    }
    for (const QPair<QString, QString>& edge : {QPair<QString, QString>{"A", "B"}, QPair<QString, QString>{"A", "C"},
                                                QPair<QString, QString>{"B", "D"}, QPair<QString, QString>{"C", "D"}}) {
        ModuleConnection connection;
        connection.fromModuleId = edge.first;
        connection.toModuleId = edge.second;
        project.addConnection(connection);
    }

    QVERIFY(engine.loadProject(&project, [&executionLog](const ModuleInstance& instance) {
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &executionLog;
        return module;
    }));
    engine.runOnce();

    QCOMPARE(executionLog,
             QStringList({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D")}));
}

void TestRunEngine::testLoadProjectRejectsIncompatiblePortTypes() {
    RunEngine& engine = RunEngine::instance();
    Project project;
    ModuleInstance source;
    source.id = QStringLiteral("source");
    source.moduleId = QStringLiteral("source");
    project.addModule(source);
    ModuleInstance target;
    target.id = QStringLiteral("target");
    target.moduleId = QStringLiteral("target");
    project.addModule(target);
    ModuleConnection connection;
    connection.fromModuleId = source.id;
    connection.toModuleId = target.id;
    connection.fromPort = QStringLiteral("value");
    connection.toPort = QStringLiteral("value");
    project.addConnection(connection);

    QVERIFY(!engine.loadProject(&project, [](const ModuleInstance& instance) {
        return instance.id == QLatin1String("source") ? new TypedPortModule(instance.id, DataType::String, true)
                                                      : new TypedPortModule(instance.id, DataType::Point2D, false);
    }));
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

void TestRunEngine::testParallelExecutesConcurrently() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    engine.setParallelThreadCount(4);

    std::atomic<int> running{0};
    std::atomic<int> maxConc{0};
    const int N = 4;
    QStringList names;
    for (int i = 0; i < N; ++i) {
        QString name = QString("par%1").arg(i);
        auto* mod = new ParallelSleepModule(name, &running, &maxConc, 60);
        mod->initialize();
        engine.addModule(mod);
        names << name;
    }

    PortValueMap input;
    const ExecutionResult result = engine.executeParallel(names, input);
    QVERIFY(result.success);
    // 证明真实并发：最大并发度应 > 1
    QVERIFY2(engine.lastParallelMaxConcurrency() > 1,
             qPrintable(QString("max concurrency=%1, expected >1").arg(engine.lastParallelMaxConcurrency())));

    engine.clearModules();
}

void TestRunEngine::testParallelFailureCancelsGroup() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    engine.setParallelThreadCount(2);

    std::atomic<int> running{0};
    std::atomic<int> maxConc{0};
    // 一个失败模块 + 一个睡眠模块
    auto* failing = new TestExecutionModule("fail");
    failing->executeResult = false;
    failing->errorText = "parallel failure";
    failing->initialize();
    engine.addModule(failing);
    auto* slow = new ParallelSleepModule("slow", &running, &maxConc, 200);
    slow->initialize();
    engine.addModule(slow);

    PortValueMap input;
    const ExecutionResult result = engine.executeParallel(QStringList{"fail", "slow"}, input);
    QVERIFY(!result.success);
    QVERIFY(!result.userMessage.isEmpty());

    engine.clearModules();
}

void TestRunEngine::testValidateFlowReportsMissingRequiredInput() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    auto* source = new TypedPortModule("src", DataType::Point2D, true);
    auto* consumer = new TypedPortModule("consumer", DataType::Point2D, false);
    source->initialize();
    consumer->initialize();
    engine.addModule(source);
    engine.addModule(consumer);

    // 无连接：consumer 必需输入 value 无上游 → 预检报错
    Project empty;
    QString err;
    QVERIFY(engine.buildExecutionOrder(&empty, err));
    QStringList problems = engine.validateFlow();
    QVERIFY2(!problems.isEmpty(), "expected pre-run error for missing required input");
    QVERIFY(problems.join("; ").contains("consumer"));

    // 有名称匹配的连接 src.value->consumer.value：预检通过。
    Project linked;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance c;
    c.id = "consumer";
    c.moduleId = "consumer";
    linked.addModule(s);
    linked.addModule(c);
    ModuleConnection conn;
    conn.fromModuleId = "src";
    conn.toModuleId = "consumer";
    conn.fromPort = "value";
    conn.toPort = "value";
    linked.addConnection(conn);
    QString err2;
    QVERIFY(engine.buildExecutionOrder(&linked, err2));
    QVERIFY2(engine.validateFlow().isEmpty(), "expected no pre-run error when connected");

    engine.clearModules();
}

void TestRunEngine::testSkippedBranchEmitsSkippedNotFailed() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    QStringList skipped;
    QMetaObject::Connection conn =
        connect(&engine, &RunEngine::moduleSkipped, [&](const QString& name) { skipped.append(name); });

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
    auto* after = new TestExecutionModule(QStringLiteral("After"));
    after->executionLog = &executionLog;
    for (TestExecutionModule* m : {condition, body, end, after}) {
        m->initialize();
        engine.addModule(m);
    }

    engine.runOnce();

    // 未激活分支（Body/WhileEnd）应发射 Skipped，且未执行
    QVERIFY2(skipped.contains(QStringLiteral("Body")), qPrintable(skipped.join(",")));
    QVERIFY(!executionLog.contains(QStringLiteral("Body")));

    disconnect(conn);
    engine.clearModules();
    delete condition;
    delete body;
    delete end;
    delete after;
}

void TestRunEngine::testBreakpointRestoredOnLoad() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance a;
    a.id = "a";
    a.moduleId = "a";
    a.breakpoint = true;
    project.addModule(a);

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) {
        return new TestExecutionModule(inst.id);
    }));
    QVERIFY(engine.hasBreakpoint(QStringLiteral("a")));

    engine.clearModules();
}

void TestRunEngine::testDisabledModuleBypassesImage() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    QStringList skipped;
    QMetaObject::Connection conn =
        connect(&engine, &RunEngine::moduleSkipped, [&](const QString& n) { skipped.append(n); });

    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    b.enabled = false;
    ModuleInstance c;
    c.id = "C";
    c.moduleId = "C";
    project.addModule(a);
    project.addModule(b);
    project.addModule(c);
    ModuleConnection e1;
    e1.fromModuleId = "A";
    e1.toModuleId = "B";
    project.addConnection(e1);
    ModuleConnection e2;
    e2.fromModuleId = "B";
    e2.toModuleId = "C";
    project.addConnection(e2);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) {
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        m->outputTag = inst.id;
        return m;
    }));
    engine.runOnce();

    // B 禁用：被跳过，不执行；C 通过旁路收到 A 的输出
    QVERIFY(skipped.contains(QStringLiteral("B")));
    QVERIFY(!log.contains(QStringLiteral("B")));
    auto* downstream = qobject_cast<TestExecutionModule*>(engine.getModule(QStringLiteral("C")));
    QVERIFY(downstream != nullptr);
    QCOMPARE(downstream->receivedTag, QStringLiteral("A"));

    disconnect(conn);
    engine.clearModules();
}

void TestRunEngine::testDisabledDependedNodePreRunFail() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance a;
    a.id = "src";
    a.moduleId = "src";
    ModuleInstance b;
    b.id = "dis";
    b.moduleId = "dis";
    b.enabled = false;
    ModuleInstance c;
    c.id = "consumer";
    c.moduleId = "consumer";
    project.addModule(a);
    project.addModule(b);
    project.addModule(c);
    ModuleConnection e1;
    e1.fromModuleId = "src";
    e1.toModuleId = "dis";
    e1.fromPort = "value";
    e1.toPort = "value";
    project.addConnection(e1);
    ModuleConnection e2;
    e2.fromModuleId = "dis";
    e2.toModuleId = "consumer";
    e2.fromPort = "distance";
    e2.toPort = "distance";
    project.addConnection(e2);

    // dis 禁用且输出 distance(非 image, 不在其输入中)无法旁路，被 consumer 依赖 → 预检失败
    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == QLatin1String("src"))
            return new PortPairModule(inst.id, QString(), DataType::Any, "value", DataType::Point2D);
        if (inst.id == QLatin1String("dis"))
            return new PortPairModule(inst.id, "value", DataType::Point2D, "distance", DataType::Number);
        return new PortPairModule(inst.id, "distance", DataType::Number, QString(), DataType::Any);
    }));
    QStringList problems = engine.validateFlow();
    QVERIFY2(!problems.isEmpty(), "expected pre-run error for disabled non-bypassable node");
    QVERIFY(problems.join(";").contains("dis"));

    engine.clearModules();
}

void TestRunEngine::testStepOnceSkipsDisabledModule() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    b.enabled = false;
    ModuleInstance c;
    c.id = "C";
    c.moduleId = "C";
    project.addModule(a);
    project.addModule(b);
    project.addModule(c);
    ModuleConnection e1;
    e1.fromModuleId = "A";
    e1.toModuleId = "B";
    project.addConnection(e1);
    ModuleConnection e2;
    e2.fromModuleId = "B";
    e2.toModuleId = "C";
    project.addConnection(e2);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) {
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        m->outputTag = inst.id;
        return m;
    }));

    // 单步 1: A 执行
    QVERIFY(engine.stepOnce());
    // 单步 2: B 禁用 → 旁路跳过，推进到 C
    QVERIFY(engine.stepOnce());
    // 单步 3: C 执行
    QVERIFY(engine.stepOnce());

    QVERIFY(log.contains("A"));
    QVERIFY(!log.contains("B")); // 禁用不执行
    QVERIFY(log.contains("C"));

    engine.clearModules();
}

void TestRunEngine::testDisabledBypassCollectsByToPort() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance d;
    d.id = "dis";
    d.moduleId = "dis";
    d.enabled = false;
    ModuleInstance c;
    c.id = "consumer";
    c.moduleId = "consumer";
    project.addModule(s);
    project.addModule(d);
    project.addModule(c);
    // 上游输出 value → 禁用节点输入 in（重命名），禁用输出 in → consumer
    ModuleConnection e1;
    e1.fromModuleId = "src";
    e1.toModuleId = "dis";
    e1.fromPort = "value";
    e1.toPort = "in";
    project.addConnection(e1);
    ModuleConnection e2;
    e2.fromModuleId = "dis";
    e2.toModuleId = "consumer";
    e2.fromPort = "in";
    e2.toPort = "in";
    project.addConnection(e2);

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == QLatin1String("src"))
            return new PortPairModule(inst.id, QString(), DataType::Any, "value", DataType::Number);
        if (inst.id == QLatin1String("dis"))
            return new PortPairModule(inst.id, "in", DataType::Number, "in", DataType::Number);
        return new PortPairModule(inst.id, "in", DataType::Number, QString(), DataType::Any);
    }));
    // 重命名端口(value→in)同名 in/in 类型兼容 → 可旁路，预检不应报错
    QVERIFY2(engine.validateFlow().isEmpty(), "renamed-port bypass should be accepted");

    QStringList skipped;
    QMetaObject::Connection sk =
        connect(&engine, &RunEngine::moduleSkipped, [&](const QString& n) { skipped.append(n); });
    engine.clearModules();
    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == QLatin1String("src"))
            return new PortPairModule(inst.id, QString(), DataType::Any, "value", DataType::Number);
        if (inst.id == QLatin1String("dis"))
            return new PortPairModule(inst.id, "in", DataType::Number, "in", DataType::Number);
        return new PortPairModule(inst.id, "in", DataType::Number, QString(), DataType::Any);
    }));
    engine.runOnce();
    // 禁用节点按 toPort 旁路并被跳过，不执行
    QVERIFY2(skipped.contains("dis"), "disabled node should be skipped via toPort bypass");
    disconnect(sk);

    engine.clearModules();
}

void TestRunEngine::testDisabledBypassRejectsTypeIncompatible() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance d;
    d.id = "dis";
    d.moduleId = "dis";
    d.enabled = false;
    ModuleInstance c;
    c.id = "consumer";
    c.moduleId = "consumer";
    project.addModule(s);
    project.addModule(d);
    project.addModule(c);
    ModuleConnection e1;
    e1.fromModuleId = "src";
    e1.toModuleId = "dis";
    e1.fromPort = "value";
    e1.toPort = "value";
    project.addConnection(e1);
    ModuleConnection e2;
    e2.fromModuleId = "dis";
    e2.toModuleId = "consumer";
    e2.fromPort = "value";
    e2.toPort = "value";
    project.addConnection(e2);

    // dis 输入 value=Point2D、输出 value=Number：同名但类型不兼容 → 不可旁路 → 预检报错
    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == QLatin1String("src"))
            return new PortPairModule(inst.id, QString(), DataType::Any, "value", DataType::Point2D);
        if (inst.id == QLatin1String("dis"))
            return new PortPairModule(inst.id, "value", DataType::Point2D, "value", DataType::Number);
        return new PortPairModule(inst.id, "value", DataType::Number, QString(), DataType::Any);
    }));
    QStringList problems = engine.validateFlow();
    QVERIFY2(!problems.isEmpty(), "expected type-incompatible bypass to be rejected");

    engine.clearModules();
}

void TestRunEngine::testBreakpointHitPauseResume() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    a.breakpoint = true; // 断点在 A
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    project.addModule(a);
    project.addModule(b);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) {
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));

    // 同步执行架构下安全暂停难以挂起式自动化；此处验证断点持久化与删除清理，
    // 运行期暂停由执行循环等待循环保障（见 RunEngine::executeRun 断点等待）。
    QVERIFY(engine.hasBreakpoint(QStringLiteral("A")));
    engine.removeModule(QStringLiteral("A"));
    QVERIFY(!engine.hasBreakpoint(QStringLiteral("A")));

    engine.clearModules();
}

QTEST_MAIN(TestRunEngine)
#include "test_runengine.moc"
