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

// 带控制端口的模块，用于控制边契约测试
class ControlPortModule : public ModuleBase {
    Q_OBJECT
public:
    ControlPortModule(const QString& name, bool hasControlOut, bool hasControlIn)
        : m_hasControlOut(hasControlOut), m_hasControlIn(hasControlIn) {
        m_moduleId = QStringLiteral("com.deeplux.test.ctrl.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    QList<PortSpec> inputPorts() const override {
        QList<PortSpec> ports;
        PortSpec data;
        data.id = QStringLiteral("image");
        data.displayName = QStringLiteral("输入");
        data.type = DataType::Image2D;
        ports.append(data);
        if (m_hasControlIn) {
            PortSpec ctrl;
            ctrl.id = QStringLiteral("ctrl_in");
            ctrl.displayName = QStringLiteral("控制输入");
            ctrl.type = DataType::Boolean;
            ctrl.control = true;
            ports.append(ctrl);
        }
        return ports;
    }
    QList<PortSpec> outputPorts() const override {
        QList<PortSpec> ports;
        PortSpec data;
        data.id = QStringLiteral("image");
        data.displayName = QStringLiteral("输出");
        data.type = DataType::Image2D;
        ports.append(data);
        if (m_hasControlOut) {
            PortSpec ctrl;
            ctrl.id = QStringLiteral("ctrl_out");
            ctrl.displayName = QStringLiteral("控制输出");
            ctrl.type = DataType::Boolean;
            ctrl.control = true;
            ports.append(ctrl);
        }
        return ports;
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
    bool m_hasControlOut;
    bool m_hasControlIn;
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
    PortPairModule(const QString& name, const QString& inId, DataType inType, const QString& outId, DataType outType)
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
    void testBreakpointDoesNotBlockRun();
    void testBreakpointPreservesFailureAcrossResume();
    void testMultipleBreakpointsCanResumeAndStop();
    void testRemoveModuleClearsTopology();
    // 阶段 C: 显式控制边契约
    void testDataEdgeToControlPortRejected();
    void testControlEdgeToDataPortRejected();
    void testControlEdgeValidAccepted();
    void testImplicitControlPortsAccepted();
    void testDuplicateFourTupleRejected();
    void testDifferentPortSameNodePairAccepted();
    void testControlEdgeCycleRejected();
    void testNoControlEdgesBehaviorUnchanged();
    // 阶段 C 复核(P2): 回归测试
    void testIllegalEdgeTypeRejected();
    void testNoDataEdgeBackwardControlAccepted();
    void testControlSelfLoopRejected();
    void testEdgeTypeRoundTripPreservesInference();
    // 阶段 D1: 顺序与 If（显式控制图）
    void testExplicitControlTrueBranch();
    void testExplicitControlFalseBranch();
    void testExplicitControlSkipsInactiveBranch();
    void testExplicitControlMergeAfterIf();
    void testExplicitControlStepOnce();
    void testExplicitControlBackwardEdgeRunAndStep();
    void testExplicitControlBreakpointResume();
    void testLegacyBreakpointPreservesPipelineData();
    // 阶段 D2: Loop/While/StopWhile（显式控制图）
    void testExplicitLoopZeroIterations();
    void testExplicitLoopThreeIterations();
    void testExplicitWhileFalseZeroIterations();
    void testExplicitStopWhileTerminatesLoop();
    void testDisabledExplicitLoopExitsThroughDone();
    void testExplicitLoopMultiModuleBody();
    void testExplicitLoopAfterPredecessor();
    void testLoopSelfEdgeRejected();
    void testLoopDoneBackEdgeRejected();
    void testNestedStopLoopPreservesOuterCounter();
    // 阶段 D 复核回归
    void testExplicitDataFanInWaitsForAllInputs();
    void testExplicitFailureDoesNotActivateSuccessor();
    void testExplicitLoopClearsInactiveBranchOutputs();
    void testExplicitWhileStopsAtMaxIterations();
    void testExplicitLoopStepOnceTraversesIterations();
    void testExplicitLoopCancellationStopsRun();
    // 阶段 E1: 多输入聚合
    void testMultiInputCollectsOrderedList();
    void testMultiInputTypeMismatchFails();
    void testMultiInputWaitsForAllUpstreams();
    void testControlJoinAllWaitsForAllSources();
    void testControlJoinTracksEdgesNotSources();
    void testControlJoinPoliciesArePerPort();
    // 阶段 E2: Parallel 接入主循环
    void testParallelBatchExecutesConcurrently();
    void testParallelFailureCancelsRemaining();
    void testParallelNonThreadSafeFallsBackSequential();
    void testParallelMixedThreadSafetyFallsBackSequential();
    void testParallelSingleThreadFailureDoesNotDeadlock();
    void testParallelBatchHonorsDisabledModule();
    void testParallelBatchHonorsBreakpoint();
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

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) { return new TestExecutionModule(inst.id); }));
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

    // 断点持久化与删除清理
    QVERIFY(engine.hasBreakpoint(QStringLiteral("A")));
    engine.removeModule(QStringLiteral("A"));
    QVERIFY(!engine.hasBreakpoint(QStringLiteral("A")));

    engine.clearModules();
}

void TestRunEngine::testBreakpointDoesNotBlockRun() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    a.breakpoint = true;
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

    QSignalSpy hitSpy(&engine, &RunEngine::breakpointHit);
    QSignalSpy finishedSpy(&engine, &RunEngine::runFinished);
    RunResult result{};
    const QMetaObject::Connection resultConnection =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& value) { result = value; });
    bool signalSawPausedState = false;
    connect(&engine, &RunEngine::breakpointHit, this, [&engine, &signalSawPausedState]() {
        signalSawPausedState = engine.state() == RunState::Paused && engine.isPausedAtBreakpoint();
    });

    // 第一次运行：命中断点 A，暂停（A 尚未执行），runOnce 返回（非阻塞）
    engine.runOnce();
    QCOMPARE(hitSpy.count(), 1);
    QCOMPARE(hitSpy.first().first().toString(), QStringLiteral("A"));
    QVERIFY2(!log.contains("A"), "breakpoint should pause before A executes");
    QVERIFY2(engine.isPausedAtBreakpoint(), "should be paused at breakpoint");
    QCOMPARE(engine.state(), RunState::Paused);
    QVERIFY(signalSawPausedState);
    QCOMPARE(finishedSpy.count(), 0);

    // 继续：恢复执行 A 和 B
    QTest::qWait(200);
    engine.resume();
    QVERIFY(log.contains("A"));
    QVERIFY(log.contains("B"));
    QVERIFY(!engine.isPausedAtBreakpoint());
    QCOMPARE(finishedSpy.count(), 1);
    QVERIFY2(result.elapsedMs < 150, "breakpoint wait time must not be counted as execution time");
    disconnect(resultConnection);

    engine.clearModules();
}

void TestRunEngine::testBreakpointPreservesFailureAcrossResume() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance failing;
    failing.id = QStringLiteral("Failing");
    failing.moduleId = failing.id;
    ModuleInstance paused;
    paused.id = QStringLiteral("Paused");
    paused.moduleId = paused.id;
    paused.breakpoint = true;
    project.addModule(failing);
    project.addModule(paused);

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) {
        auto* module = new TestExecutionModule(inst.id);
        if (inst.id == QStringLiteral("Failing")) {
            module->executeResult = false;
            module->errorText = QStringLiteral("failure before breakpoint");
        }
        return module;
    }));

    QSignalSpy finishedSpy(&engine, &RunEngine::runFinished);
    RunResult result{};
    const QMetaObject::Connection resultConnection =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& value) { result = value; });
    engine.runOnce();
    QVERIFY(engine.isPausedAtBreakpoint());
    QCOMPARE(finishedSpy.count(), 0);

    engine.resume();
    QCOMPARE(finishedSpy.count(), 1);
    QVERIFY(!result.success);
    QCOMPARE(result.errorMessage, QStringLiteral("failure before breakpoint"));
    disconnect(resultConnection);
}

void TestRunEngine::testMultipleBreakpointsCanResumeAndStop() {
    RunEngine& engine = RunEngine::instance();
    Project project;
    for (const QString& id : {QStringLiteral("A"), QStringLiteral("B")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        instance.breakpoint = true;
        project.addModule(instance);
    }

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) {
        auto* module = new TestExecutionModule(inst.id);
        module->executionLog = &log;
        return module;
    }));

    QSignalSpy hitSpy(&engine, &RunEngine::breakpointHit);
    engine.runOnce();
    QCOMPARE(hitSpy.count(), 1);
    QCOMPARE(hitSpy.at(0).first().toString(), QStringLiteral("A"));

    engine.resume();
    QCOMPARE(log, QStringList({QStringLiteral("A")}));
    QCOMPARE(hitSpy.count(), 2);
    QCOMPARE(hitSpy.at(1).first().toString(), QStringLiteral("B"));
    QVERIFY(engine.isPausedAtBreakpoint());

    engine.stop();
    QCOMPARE(engine.state(), RunState::Stopped);
    QVERIFY(!engine.isPausedAtBreakpoint());
    QCOMPARE(log, QStringList({QStringLiteral("A")}));
}

void TestRunEngine::testRemoveModuleClearsTopology() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    project.addModule(a);
    project.addModule(b);
    ModuleConnection e;
    e.fromModuleId = "A";
    e.toModuleId = "B";
    project.addConnection(e);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) {
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));

    // 删除 A 后运行不应报 Module not found，且 B 仍执行
    engine.removeModule(QStringLiteral("A"));
    engine.runOnce();
    QVERIFY(!log.contains("A"));
    QVERIFY(log.contains("B"));

    engine.clearModules();
}

// ---------------------------------------------------------------------------
// 阶段 C: 显式控制边契约测试
// ---------------------------------------------------------------------------

void TestRunEngine::testDataEdgeToControlPortRejected() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance t;
    t.id = "tgt";
    t.moduleId = "tgt";
    project.addModule(s);
    project.addModule(t);
    ModuleConnection c;
    c.fromModuleId = "src";
    c.toModuleId = "tgt";
    c.fromPort = "image";
    c.toPort = "ctrl_in";
    c.edgeType = "data";
    project.addConnection(c);
    QVERIFY2(!engine.loadProject(&project,
                                 [](const ModuleInstance& inst) -> ModuleBase* {
                                     if (inst.id == QLatin1String("src"))
                                         return new ControlPortModule(inst.id, false, false);
                                     return new ControlPortModule(inst.id, false, true);
                                 }),
             "should reject data edge to control port");
    engine.clearModules();
}

void TestRunEngine::testControlEdgeToDataPortRejected() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance t;
    t.id = "tgt";
    t.moduleId = "tgt";
    project.addModule(s);
    project.addModule(t);
    ModuleConnection dataC;
    dataC.fromModuleId = "src";
    dataC.toModuleId = "tgt";
    dataC.fromPort = "image";
    dataC.toPort = "image";
    dataC.edgeType = "data";
    project.addConnection(dataC);
    ModuleConnection c;
    c.fromModuleId = "src";
    c.toModuleId = "tgt";
    c.fromPort = "ctrl_out";
    c.toPort = "image";
    c.edgeType = "control";
    project.addConnection(c);
    QVERIFY2(!engine.loadProject(&project,
                                 [](const ModuleInstance& inst) -> ModuleBase* {
                                     if (inst.id == QLatin1String("src"))
                                         return new ControlPortModule(inst.id, true, false);
                                     return new ControlPortModule(inst.id, false, false);
                                 }),
             "should reject control edge to data port");
    engine.clearModules();
}

void TestRunEngine::testControlEdgeValidAccepted() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance t;
    t.id = "tgt";
    t.moduleId = "tgt";
    project.addModule(s);
    project.addModule(t);
    ModuleConnection dataC;
    dataC.fromModuleId = "src";
    dataC.toModuleId = "tgt";
    dataC.fromPort = "image";
    dataC.toPort = "image";
    dataC.edgeType = "data";
    project.addConnection(dataC);
    ModuleConnection ctrlC;
    ctrlC.fromModuleId = "src";
    ctrlC.toModuleId = "tgt";
    ctrlC.fromPort = "ctrl_out";
    ctrlC.toPort = "ctrl_in";
    ctrlC.edgeType = "control";
    project.addConnection(ctrlC);
    QVERIFY2(engine.loadProject(
                 &project,
                 [](const ModuleInstance& inst) -> ModuleBase* { return new ControlPortModule(inst.id, true, true); }),
             "valid control edge should be accepted");
    engine.clearModules();
}

void TestRunEngine::testImplicitControlPortsAccepted() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance t;
    t.id = "tgt";
    t.moduleId = "tgt";
    project.addModule(s);
    project.addModule(t);
    ModuleConnection dataC;
    dataC.fromModuleId = "src";
    dataC.toModuleId = "tgt";
    dataC.fromPort = "image";
    dataC.toPort = "image";
    project.addConnection(dataC);
    ModuleConnection ctrlC;
    ctrlC.fromModuleId = "src";
    ctrlC.toModuleId = "tgt";
    ctrlC.fromPort = "next";
    ctrlC.toPort = "control";
    ctrlC.edgeType = "control";
    project.addConnection(ctrlC);
    QVERIFY2(engine.loadProject(&project,
                                [](const ModuleInstance& inst) {
                                    auto* m = new TestExecutionModule(inst.id);
                                    m->initialize();
                                    return m;
                                }),
             "implicit control ports should be accepted");
    engine.clearModules();
}

void TestRunEngine::testDuplicateFourTupleRejected() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance t;
    t.id = "tgt";
    t.moduleId = "tgt";
    project.addModule(s);
    project.addModule(t);
    ModuleConnection c1;
    c1.fromModuleId = "src";
    c1.toModuleId = "tgt";
    c1.fromPort = "image";
    c1.toPort = "image";
    project.addConnection(c1);
    ModuleConnection c2 = c1;
    project.addConnection(c2);
    QVERIFY2(!engine.loadProject(&project,
                                 [](const ModuleInstance& inst) {
                                     auto* m = new TestExecutionModule(inst.id);
                                     m->initialize();
                                     return m;
                                 }),
             "duplicate 4-tuple should be rejected");
    engine.clearModules();
}

void TestRunEngine::testDifferentPortSameNodePairAccepted() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance t;
    t.id = "tgt";
    t.moduleId = "tgt";
    project.addModule(s);
    project.addModule(t);
    ModuleConnection c1;
    c1.fromModuleId = "src";
    c1.toModuleId = "tgt";
    c1.fromPort = "image";
    c1.toPort = "image";
    c1.edgeType = "data";
    project.addConnection(c1);
    ModuleConnection c2;
    c2.fromModuleId = "src";
    c2.toModuleId = "tgt";
    c2.fromPort = "ctrl_out";
    c2.toPort = "ctrl_in";
    c2.edgeType = "control";
    project.addConnection(c2);
    QVERIFY2(engine.loadProject(
                 &project,
                 [](const ModuleInstance& inst) -> ModuleBase* { return new ControlPortModule(inst.id, true, true); }),
             "different-port same-pair should be accepted");
    engine.clearModules();
}

void TestRunEngine::testControlEdgeCycleRejected() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    project.addModule(a);
    project.addModule(b);
    ModuleConnection dataA;
    dataA.fromModuleId = "A";
    dataA.toModuleId = "B";
    dataA.fromPort = "image";
    dataA.toPort = "image";
    dataA.edgeType = "data";
    project.addConnection(dataA);
    ModuleConnection ctrlBack;
    ctrlBack.fromModuleId = "B";
    ctrlBack.toModuleId = "A";
    ctrlBack.fromPort = "next";
    ctrlBack.toPort = "control";
    ctrlBack.edgeType = "control";
    project.addConnection(ctrlBack);
    QVERIFY2(!engine.loadProject(&project,
                                 [](const ModuleInstance& inst) {
                                     auto* m = new TestExecutionModule(inst.id);
                                     m->initialize();
                                     return m;
                                 }),
             "control edge cycle should be rejected");
    engine.clearModules();
}

void TestRunEngine::testNoControlEdgesBehaviorUnchanged() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    QStringList log;
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    project.addModule(a);
    project.addModule(b);
    ModuleConnection c;
    c.fromModuleId = "A";
    c.toModuleId = "B";
    c.fromPort = "image";
    c.toPort = "image";
    c.edgeType = "data";
    project.addConnection(c);
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) {
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));
    engine.runOnce();
    QCOMPARE(log, QStringList({"A", "B"}));
    engine.clearModules();
}

// ---------------------------------------------------------------------------
// 阶段 C 复核(P2): 回归测试
// ---------------------------------------------------------------------------

void TestRunEngine::testIllegalEdgeTypeRejected() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance s;
    s.id = "src";
    s.moduleId = "src";
    ModuleInstance t;
    t.id = "tgt";
    t.moduleId = "tgt";
    project.addModule(s);
    project.addModule(t);
    ModuleConnection c;
    c.fromModuleId = "src";
    c.toModuleId = "tgt";
    c.fromPort = "image";
    c.toPort = "image";
    c.edgeType = "bogus";
    project.addConnection(c);
    QVERIFY2(!engine.loadProject(&project,
                                 [](const ModuleInstance& inst) {
                                     auto* m = new TestExecutionModule(inst.id);
                                     m->initialize();
                                     return m;
                                 }),
             "illegal edgeType should be rejected");
    engine.clearModules();
}

void TestRunEngine::testNoDataEdgeBackwardControlAccepted() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // 仅有控制边 B.next -> A.control，无数据边
    // A、B 无数据依赖，拓扑序可以是 [A, B] 或 [B, A]
    // 无论哪种顺序，B->A 控制边都不是环（不形成回路）
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    project.addModule(a);
    project.addModule(b);
    ModuleConnection ctrl;
    ctrl.fromModuleId = "B";
    ctrl.toModuleId = "A";
    ctrl.fromPort = "next";
    ctrl.toPort = "control";
    ctrl.edgeType = "control";
    project.addConnection(ctrl);
    QVERIFY2(engine.loadProject(&project,
                                [](const ModuleInstance& inst) {
                                    auto* m = new TestExecutionModule(inst.id);
                                    m->initialize();
                                    return m;
                                }),
             "backward control edge without data edges should be accepted (no cycle)");
    engine.clearModules();
}

void TestRunEngine::testControlSelfLoopRejected() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // A.next -> A.control：控制自环
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    project.addModule(a);
    ModuleConnection ctrl;
    ctrl.fromModuleId = "A";
    ctrl.toModuleId = "A";
    ctrl.fromPort = "next";
    ctrl.toPort = "control";
    ctrl.edgeType = "control";
    project.addConnection(ctrl);
    QVERIFY2(!engine.loadProject(&project,
                                 [](const ModuleInstance& inst) {
                                     auto* m = new TestExecutionModule(inst.id);
                                     m->initialize();
                                     return m;
                                 }),
             "control self-loop should be rejected");
    engine.clearModules();
}

void TestRunEngine::testEdgeTypeRoundTripPreservesInference() {
    // 缺失 edgeType 的 next->control 连接：toJson 不写 edgeType，
    // fromJson 读回空字符串，引擎应重新推断为 control 而非 data
    ModuleConnection orig;
    orig.fromModuleId = "B";
    orig.toModuleId = "A";
    orig.fromPort = "next";
    orig.toPort = "control";
    // edgeType 为空——引擎应推断为 control

    const QJsonObject json = orig.toJson();
    // toJson 只在 edgeType 非空时写入，因此 JSON 中不应有 edgeType 字段
    QVERIFY2(!json.contains("edgeType"), "toJson should omit empty edgeType");
    QVERIFY2(json.contains("fromPort"), "fromPort should be present");

    ModuleConnection restored = ModuleConnection::fromJson(json);
    QVERIFY2(restored.edgeType.isEmpty(), "fromJson should preserve empty edgeType for engine inference");
    QCOMPARE(restored.fromPort, QString("next"));
    QCOMPARE(restored.toPort, QString("control"));

    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    project.addModule(a);
    project.addModule(b);
    project.addConnection(restored);

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project, [](const ModuleInstance& inst) { return new TestExecutionModule(inst.id); }),
             "round-tripped next->control edge should be inferred as control");
    engine.clearModules();
}

// ---------------------------------------------------------------------------
// 阶段 D1: 顺序与 If（显式控制图）
// ---------------------------------------------------------------------------

// 带控制输出的 If-like 模块：输出 true/false 控制端口
class IfControlModule : public ModuleBase {
    Q_OBJECT
public:
    bool forceTrue = true;
    QStringList* execLog = nullptr;
    IfControlModule(const QString& name, bool forceTrueBranch) : forceTrue(forceTrueBranch) {
        m_moduleId = QStringLiteral("com.deeplux.test.if.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec t;
        t.id = "true";
        t.displayName = "true";
        t.type = DataType::Boolean;
        t.control = true;
        PortSpec f;
        f.id = "false";
        f.displayName = "false";
        f.type = DataType::Boolean;
        f.control = true;
        PortSpec img;
        img.id = "image";
        img.displayName = "image";
        img.type = DataType::Image2D;
        return {img, t, f};
    }
    ControlFlowType flowControlType() const override {
        return ControlFlowType::Conditional;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData("if_result", forceTrue);
        if (execLog)
            execLog->append(name());
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

void TestRunEngine::testExplicitControlTrueBranch() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // If(true) -> true -> BodyTrue ; false -> BodyFalse (skipped)
    Project project;
    ModuleInstance cond;
    cond.id = "cond";
    cond.moduleId = "cond";
    ModuleInstance bt;
    bt.id = "bodyTrue";
    bt.moduleId = "bodyTrue";
    ModuleInstance bf;
    bf.id = "bodyFalse";
    bf.moduleId = "bodyFalse";
    bf.breakpoint = true;
    project.addModule(cond);
    project.addModule(bt);
    project.addModule(bf);
    // 数据边
    ModuleConnection dataCond;
    dataCond.fromModuleId = "cond";
    dataCond.toModuleId = "bodyTrue";
    dataCond.fromPort = "image";
    dataCond.toPort = "image";
    dataCond.edgeType = "data";
    project.addConnection(dataCond);
    ModuleConnection dataCond2;
    dataCond2.fromModuleId = "cond";
    dataCond2.toModuleId = "bodyFalse";
    dataCond2.fromPort = "image";
    dataCond2.toPort = "image";
    dataCond2.edgeType = "data";
    project.addConnection(dataCond2);
    // 控制边
    ModuleConnection ctrlTrue;
    ctrlTrue.fromModuleId = "cond";
    ctrlTrue.toModuleId = "bodyTrue";
    ctrlTrue.fromPort = "true";
    ctrlTrue.toPort = "control";
    ctrlTrue.edgeType = "control";
    project.addConnection(ctrlTrue);
    ModuleConnection ctrlFalse;
    ctrlFalse.fromModuleId = "cond";
    ctrlFalse.toModuleId = "bodyFalse";
    ctrlFalse.fromPort = "false";
    ctrlFalse.toPort = "control";
    ctrlFalse.edgeType = "control";
    project.addConnection(ctrlFalse);

    QStringList log;
    QStringList skipped;
    QSignalSpy hitSpy(&engine, &RunEngine::breakpointHit);
    QMetaObject::Connection skipConn =
        connect(&engine, &RunEngine::moduleSkipped, [&](const QString& n) { skipped.append(n); });
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == QLatin1String("cond")) {
            auto* m = new IfControlModule(inst.id, true);
            m->execLog = &log;
            return m;
        }
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));
    engine.runOnce();
    QVERIFY(log.contains("cond"));
    QVERIFY(log.contains("bodyTrue"));
    QVERIFY(!log.contains("bodyFalse"));
    QVERIFY(skipped.contains("bodyFalse"));
    QCOMPARE(hitSpy.count(), 0);
    QVERIFY(!engine.isPausedAtBreakpoint());
    disconnect(skipConn);
    engine.clearModules();
}

void TestRunEngine::testExplicitControlFalseBranch() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance cond;
    cond.id = "cond";
    cond.moduleId = "cond";
    ModuleInstance bt;
    bt.id = "bodyTrue";
    bt.moduleId = "bodyTrue";
    ModuleInstance bf;
    bf.id = "bodyFalse";
    bf.moduleId = "bodyFalse";
    project.addModule(cond);
    project.addModule(bt);
    project.addModule(bf);
    ModuleConnection dataCond;
    dataCond.fromModuleId = "cond";
    dataCond.toModuleId = "bodyTrue";
    dataCond.fromPort = "image";
    dataCond.toPort = "image";
    dataCond.edgeType = "data";
    project.addConnection(dataCond);
    ModuleConnection dataCond2;
    dataCond2.fromModuleId = "cond";
    dataCond2.toModuleId = "bodyFalse";
    dataCond2.fromPort = "image";
    dataCond2.toPort = "image";
    dataCond2.edgeType = "data";
    project.addConnection(dataCond2);
    ModuleConnection ctrlTrue;
    ctrlTrue.fromModuleId = "cond";
    ctrlTrue.toModuleId = "bodyTrue";
    ctrlTrue.fromPort = "true";
    ctrlTrue.toPort = "control";
    ctrlTrue.edgeType = "control";
    project.addConnection(ctrlTrue);
    ModuleConnection ctrlFalse;
    ctrlFalse.fromModuleId = "cond";
    ctrlFalse.toModuleId = "bodyFalse";
    ctrlFalse.fromPort = "false";
    ctrlFalse.toPort = "control";
    ctrlFalse.edgeType = "control";
    project.addConnection(ctrlFalse);

    QStringList log;
    QStringList skipped;
    QMetaObject::Connection skipConn =
        connect(&engine, &RunEngine::moduleSkipped, [&](const QString& n) { skipped.append(n); });
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == QLatin1String("cond")) {
            auto* m = new IfControlModule(inst.id, false);
            m->execLog = &log;
            return m;
        }
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));
    engine.runOnce();
    QVERIFY(log.contains("cond"));
    QVERIFY(!log.contains("bodyTrue"));
    QVERIFY(log.contains("bodyFalse"));
    QVERIFY(skipped.contains("bodyTrue"));
    disconnect(skipConn);
    engine.clearModules();
}

void TestRunEngine::testExplicitControlSkipsInactiveBranch() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // Cond(true) -> bodyTrue; bodyFalse should be Skipped not failure
    Project project;
    ModuleInstance cond;
    cond.id = "cond";
    cond.moduleId = "cond";
    ModuleInstance bt;
    bt.id = "bodyTrue";
    bt.moduleId = "bodyTrue";
    ModuleInstance bf;
    bf.id = "bodyFalse";
    bf.moduleId = "bodyFalse";
    project.addModule(cond);
    project.addModule(bt);
    project.addModule(bf);
    ModuleConnection ctrlTrue;
    ctrlTrue.fromModuleId = "cond";
    ctrlTrue.toModuleId = "bodyTrue";
    ctrlTrue.fromPort = "true";
    ctrlTrue.toPort = "control";
    ctrlTrue.edgeType = "control";
    project.addConnection(ctrlTrue);
    ModuleConnection ctrlFalse;
    ctrlFalse.fromModuleId = "cond";
    ctrlFalse.toModuleId = "bodyFalse";
    ctrlFalse.fromPort = "false";
    ctrlFalse.toPort = "control";
    ctrlFalse.edgeType = "control";
    project.addConnection(ctrlFalse);

    QStringList skipped;
    bool finished = false;
    bool runSucceeded = false;
    const QMetaObject::Connection resultConn = connect(&engine, &RunEngine::runFinished, [&](const RunResult& result) {
        finished = true;
        runSucceeded = result.success;
    });
    QMetaObject::Connection skipConn =
        connect(&engine, &RunEngine::moduleSkipped, [&](const QString& n) { skipped.append(n); });
    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == QLatin1String("cond"))
            return new IfControlModule(inst.id, true);
        return new TestExecutionModule(inst.id);
    }));
    engine.runOnce();
    QVERIFY(skipped.contains("bodyFalse"));
    QVERIFY(finished);
    QVERIFY(runSucceeded);
    disconnect(resultConn);
    disconnect(skipConn);
    engine.clearModules();
}

void TestRunEngine::testExplicitControlMergeAfterIf() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // cond(true) -> bodyTrue -> merge ; cond(false) -> bodyFalse -> merge
    Project project;
    ModuleInstance cond;
    cond.id = "cond";
    cond.moduleId = "cond";
    ModuleInstance bt;
    bt.id = "bodyTrue";
    bt.moduleId = "bodyTrue";
    ModuleInstance bf;
    bf.id = "bodyFalse";
    bf.moduleId = "bodyFalse";
    ModuleInstance merge;
    merge.id = "merge";
    merge.moduleId = "merge";
    project.addModule(cond);
    project.addModule(bt);
    project.addModule(bf);
    project.addModule(merge);
    // 数据边
    ModuleConnection dt;
    dt.fromModuleId = "bodyTrue";
    dt.toModuleId = "merge";
    dt.fromPort = "image";
    dt.toPort = "image";
    dt.edgeType = "data";
    project.addConnection(dt);
    ModuleConnection df;
    df.fromModuleId = "bodyFalse";
    df.toModuleId = "merge";
    df.fromPort = "image";
    df.toPort = "image";
    df.edgeType = "data";
    project.addConnection(df);
    // 控制边
    ModuleConnection ctrlT;
    ctrlT.fromModuleId = "cond";
    ctrlT.toModuleId = "bodyTrue";
    ctrlT.fromPort = "true";
    ctrlT.toPort = "control";
    ctrlT.edgeType = "control";
    project.addConnection(ctrlT);
    ModuleConnection ctrlF;
    ctrlF.fromModuleId = "cond";
    ctrlF.toModuleId = "bodyFalse";
    ctrlF.fromPort = "false";
    ctrlF.toPort = "control";
    ctrlF.edgeType = "control";
    project.addConnection(ctrlF);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == QLatin1String("cond")) {
            auto* m = new IfControlModule(inst.id, true);
            m->execLog = &log;
            return m;
        }
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));
    engine.runOnce();
    QVERIFY(log.contains("cond"));
    QVERIFY(log.contains("bodyTrue"));
    QVERIFY(log.contains("merge"));
    QVERIFY(!log.contains("bodyFalse"));
    engine.clearModules();
}

void TestRunEngine::testExplicitControlStepOnce() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // cond(true) -> bodyTrue -> after
    Project project;
    ModuleInstance cond;
    cond.id = "cond";
    cond.moduleId = "cond";
    ModuleInstance bt;
    bt.id = "bodyTrue";
    bt.moduleId = "bodyTrue";
    ModuleInstance after;
    after.id = "after";
    after.moduleId = "after";
    project.addModule(cond);
    project.addModule(bt);
    project.addModule(after);
    // 控制边
    ModuleConnection ctrlT;
    ctrlT.fromModuleId = "cond";
    ctrlT.toModuleId = "bodyTrue";
    ctrlT.fromPort = "true";
    ctrlT.toPort = "control";
    ctrlT.edgeType = "control";
    project.addConnection(ctrlT);
    // next -> after (implicit)
    ModuleConnection ctrlAfter;
    ctrlAfter.fromModuleId = "bodyTrue";
    ctrlAfter.toModuleId = "after";
    ctrlAfter.fromPort = "next";
    ctrlAfter.toPort = "control";
    ctrlAfter.edgeType = "control";
    project.addConnection(ctrlAfter);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == QLatin1String("cond")) {
            auto* m = new IfControlModule(inst.id, true);
            m->execLog = &log;
            return m;
        }
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));
    // step 1: cond
    QVERIFY(engine.stepOnce());
    QVERIFY(log.contains("cond"));
    QVERIFY(!log.contains("bodyTrue"));
    // step 2: bodyTrue
    QVERIFY(engine.stepOnce());
    QVERIFY(log.contains("bodyTrue"));
    QVERIFY(!log.contains("after"));
    // step 3: after
    QVERIFY(engine.stepOnce());
    QVERIFY(log.contains("after"));
    engine.clearModules();
}

void TestRunEngine::testExplicitControlBackwardEdgeRunAndStep() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    project.addModule(a);
    project.addModule(b);
    ModuleConnection control;
    control.fromModuleId = "B";
    control.toModuleId = "A";
    control.fromPort = "next";
    control.toPort = "control";
    control.edgeType = "control";
    project.addConnection(control);

    QStringList log;
    const auto load = [&]() {
        return engine.loadProject(&project, [&log](const ModuleInstance& instance) {
            auto* module = new TestExecutionModule(instance.id);
            module->executionLog = &log;
            return module;
        });
    };

    QVERIFY(load());
    engine.runOnce();
    QCOMPARE(log, QStringList({QStringLiteral("B"), QStringLiteral("A")}));

    engine.clearModules();
    log.clear();
    QVERIFY(load());
    QVERIFY(engine.stepOnce());
    QCOMPARE(log, QStringList({QStringLiteral("B")}));
    QVERIFY(engine.stepOnce());
    QCOMPARE(log, QStringList({QStringLiteral("B"), QStringLiteral("A")}));
    engine.clearModules();
}

void TestRunEngine::testExplicitControlBreakpointResume() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    b.breakpoint = true;
    project.addModule(a);
    project.addModule(b);
    ModuleConnection control;
    control.fromModuleId = "A";
    control.toModuleId = "B";
    control.fromPort = "next";
    control.toPort = "control";
    control.edgeType = "control";
    project.addConnection(control);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& instance) {
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &log;
        return module;
    }));
    QSignalSpy hitSpy(&engine, &RunEngine::breakpointHit);

    engine.runOnce();
    QCOMPARE(log, QStringList({QStringLiteral("A")}));
    QCOMPARE(hitSpy.count(), 1);
    QVERIFY(engine.isPausedAtBreakpoint());

    engine.resume();
    QCOMPARE(log, QStringList({QStringLiteral("A"), QStringLiteral("B")}));
    QCOMPARE(hitSpy.count(), 1);
    QVERIFY(!engine.isPausedAtBreakpoint());
    engine.clearModules();
}

void TestRunEngine::testLegacyBreakpointPreservesPipelineData() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    Project project;
    ModuleInstance source;
    source.id = "source";
    source.moduleId = "source";
    ModuleInstance sink;
    sink.id = "sink";
    sink.moduleId = "sink";
    sink.breakpoint = true;
    project.addModule(source);
    project.addModule(sink);

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& instance) {
        auto* module = new TestExecutionModule(instance.id);
        if (instance.id == QLatin1String("source"))
            module->outputTag = QStringLiteral("preserved");
        return module;
    }));
    auto* sinkModule = qobject_cast<TestExecutionModule*>(engine.getModule(QStringLiteral("sink")));
    QVERIFY(sinkModule);

    engine.runOnce();
    QVERIFY(engine.isPausedAtBreakpoint());
    engine.resume();
    QCOMPARE(sinkModule->receivedTag, QStringLiteral("preserved"));
    engine.clearModules();
}

// ---------------------------------------------------------------------------
// 阶段 D2: Loop/While/StopWhile（显式控制图）
// ---------------------------------------------------------------------------

class LoopControlModule : public ModuleBase {
    Q_OBJECT
public:
    QStringList* execLog = nullptr;
    int loopCount = 3;
    LoopControlModule(const QString& name, int count) : loopCount(count) {
        m_moduleId = QStringLiteral("com.deeplux.test.loop.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec body;
        body.id = "body";
        body.displayName = "body";
        body.type = DataType::Boolean;
        body.control = true;
        PortSpec done;
        done.id = "done";
        done.displayName = "done";
        done.type = DataType::Boolean;
        done.control = true;
        PortSpec img;
        img.id = "image";
        img.displayName = "image";
        img.type = DataType::Image2D;
        return {img, body, done};
    }
    ControlFlowType flowControlType() const override {
        return ControlFlowType::Loop;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        if (execLog)
            execLog->append(name());
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class WhileControlModule : public ModuleBase {
    Q_OBJECT
public:
    QStringList* execLog = nullptr;
    bool forceTrue = true;
    WhileControlModule(const QString& name, bool forceTrueBranch) : forceTrue(forceTrueBranch) {
        m_moduleId = QStringLiteral("com.deeplux.test.while.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec body;
        body.id = "body";
        body.displayName = "body";
        body.type = DataType::Boolean;
        body.control = true;
        PortSpec done;
        done.id = "done";
        done.displayName = "done";
        done.type = DataType::Boolean;
        done.control = true;
        PortSpec img;
        img.id = "image";
        img.displayName = "image";
        img.type = DataType::Image2D;
        return {img, body, done};
    }
    ControlFlowType flowControlType() const override {
        return ControlFlowType::While;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData("while_result", forceTrue);
        if (execLog)
            execLog->append(name());
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class StopWhileControlModule : public ModuleBase {
    Q_OBJECT
public:
    QStringList* execLog = nullptr;
    StopWhileControlModule(const QString& name) {
        m_moduleId = QStringLiteral("com.deeplux.test.stopwhile.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec stop;
        stop.id = "stop";
        stop.displayName = "stop";
        stop.type = DataType::Boolean;
        stop.control = true;
        PortSpec img;
        img.id = "image";
        img.displayName = "image";
        img.type = DataType::Image2D;
        return {img, stop};
    }
    ControlFlowType flowControlType() const override {
        return ControlFlowType::StopLoop;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        if (execLog)
            execLog->append(name());
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class NumberFlowModule : public ModuleBase {
public:
    NumberFlowModule(const QString& name, const QStringList& inputs = {}, const QString& output = QString(),
                     bool inputsRequired = true, QStringList* observations = nullptr, double outputValue = 1.0)
        : m_inputs(inputs), m_output(output), m_inputsRequired(inputsRequired), m_observations(observations),
          m_outputValue(outputValue) {
        m_moduleId = QStringLiteral("com.deeplux.test.number.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
        setPorts(inputPorts(), outputPorts());
    }

    QList<PortSpec> inputPorts() const override {
        QList<PortSpec> ports;
        for (const QString& id : m_inputs) {
            PortSpec port;
            port.id = id;
            port.displayName = id;
            port.type = DataType::Number;
            port.required = m_inputsRequired;
            ports.append(port);
        }
        return ports;
    }

    QList<PortSpec> outputPorts() const override {
        if (m_output.isEmpty())
            return {};
        PortSpec port;
        port.id = m_output;
        port.displayName = m_output;
        port.type = DataType::Number;
        return {port};
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        if (m_observations) {
            QString presence;
            for (const QString& id : m_inputs)
                presence.append(input.hasData(id) ? QLatin1Char('1') : QLatin1Char('0'));
            m_observations->append(presence);
        }
        if (!m_output.isEmpty())
            output.setData(m_output, m_outputValue);
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QStringList m_inputs;
    QString m_output;
    bool m_inputsRequired;
    QStringList* m_observations;
    double m_outputValue;
};

class AlternatingConditionModule : public ModuleBase {
public:
    explicit AlternatingConditionModule(const QString& name) {
        m_moduleId = QStringLiteral("com.deeplux.test.alternating.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
        setPorts({}, outputPorts());
    }

    QList<PortSpec> outputPorts() const override {
        PortSpec truePort;
        truePort.id = QStringLiteral("true");
        truePort.displayName = QStringLiteral("true");
        truePort.type = DataType::Boolean;
        truePort.control = true;
        PortSpec falsePort = truePort;
        falsePort.id = QStringLiteral("false");
        falsePort.displayName = QStringLiteral("false");
        return {truePort, falsePort};
    }

    ControlFlowType flowControlType() const override {
        return ControlFlowType::Conditional;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("if_result"), m_iteration++ == 0);
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    int m_iteration = 0;
};

static ModuleConnection controlConnection(const QString& from, const QString& port, const QString& to,
                                          const QString& toPort = QStringLiteral("control")) {
    ModuleConnection connection;
    connection.fromModuleId = from;
    connection.fromPort = port;
    connection.toModuleId = to;
    connection.toPort = toPort;
    connection.edgeType = QStringLiteral("control");
    return connection;
}

static ModuleConnection dataConnection(const QString& from, const QString& fromPort, const QString& to,
                                       const QString& toPort) {
    ModuleConnection connection;
    connection.fromModuleId = from;
    connection.fromPort = fromPort;
    connection.toModuleId = to;
    connection.toPort = toPort;
    connection.edgeType = QStringLiteral("data");
    return connection;
}

void TestRunEngine::testExplicitDataFanInWaitsForAllInputs() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("D"), QStringLiteral("C")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        project.addModule(instance);
    }
    project.addConnection(dataConnection("A", "a", "C", "a"));
    project.addConnection(dataConnection("B", "b", "D", "b"));
    project.addConnection(dataConnection("D", "d", "C", "d"));
    project.addConnection(controlConnection("A", "next", "C"));
    project.addConnection(controlConnection("B", "next", "D"));
    project.addConnection(controlConnection("D", "next", "C"));

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("A"))
            return new NumberFlowModule(instance.id, {}, QStringLiteral("a"));
        if (instance.id == QLatin1String("B"))
            return new NumberFlowModule(instance.id, {}, QStringLiteral("b"));
        if (instance.id == QLatin1String("D"))
            return new NumberFlowModule(instance.id, {QStringLiteral("b")}, QStringLiteral("d"));
        return new NumberFlowModule(instance.id, {QStringLiteral("a"), QStringLiteral("d")});
    }));

    QObject signalContext;
    QStringList executionOrder;
    connect(&engine, &RunEngine::moduleStarted, &signalContext,
            [&executionOrder](const QString& moduleName) { executionOrder.append(moduleName); });
    engine.runOnce();

    QCOMPARE(executionOrder, QStringList({"A", "B", "D", "C"}));
    engine.clearModules();
}

void TestRunEngine::testExplicitFailureDoesNotActivateSuccessor() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id : {QStringLiteral("Fail"), QStringLiteral("Next")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("Fail", "next", "Next"));

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& instance) {
        auto* module = new TestExecutionModule(instance.id);
        module->executeResult = instance.id != QLatin1String("Fail");
        return module;
    }));

    QObject signalContext;
    QStringList executionOrder;
    QStringList skipped;
    RunResult result;
    connect(&engine, &RunEngine::moduleStarted, &signalContext,
            [&executionOrder](const QString& moduleName) { executionOrder.append(moduleName); });
    connect(&engine, &RunEngine::moduleSkipped, &signalContext,
            [&skipped](const QString& moduleName) { skipped.append(moduleName); });
    connect(&engine, &RunEngine::runFinished, &signalContext,
            [&result](const RunResult& runResult) { result = runResult; });
    engine.runOnce();

    QCOMPARE(executionOrder, QStringList({"Fail"}));
    QVERIFY(skipped.contains(QStringLiteral("Next")));
    QVERIFY(!result.success);
    engine.clearModules();
}

void TestRunEngine::testExplicitLoopClearsInactiveBranchOutputs() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id : {QStringLiteral("Loop"), QStringLiteral("Cond"), QStringLiteral("True"),
                              QStringLiteral("False"), QStringLiteral("Merge"), QStringLiteral("After")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        if (id == QLatin1String("Loop"))
            instance.params[QStringLiteral("loopCount")] = 2;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("Loop", "body", "Cond"));
    project.addConnection(controlConnection("Cond", "true", "True"));
    project.addConnection(controlConnection("Cond", "false", "False"));
    project.addConnection(controlConnection("True", "next", "Merge"));
    project.addConnection(controlConnection("False", "next", "Merge"));
    project.addConnection(controlConnection("Merge", "next", "Loop"));
    project.addConnection(controlConnection("Loop", "done", "After"));
    project.addConnection(dataConnection("True", "value", "Merge", "trueValue"));
    project.addConnection(dataConnection("False", "value", "Merge", "falseValue"));

    QStringList observations;
    QVERIFY(engine.loadProject(&project, [&observations](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("Loop"))
            return new LoopControlModule(instance.id, 2);
        if (instance.id == QLatin1String("Cond"))
            return new AlternatingConditionModule(instance.id);
        if (instance.id == QLatin1String("True") || instance.id == QLatin1String("False"))
            return new NumberFlowModule(instance.id, {}, QStringLiteral("value"));
        if (instance.id == QLatin1String("Merge")) {
            return new NumberFlowModule(instance.id, {QStringLiteral("trueValue"), QStringLiteral("falseValue")},
                                        QString(), false, &observations);
        }
        return new TestExecutionModule(instance.id);
    }));

    engine.runOnce();

    QCOMPARE(observations, QStringList({"10", "01"}));
    engine.clearModules();
}

void TestRunEngine::testExplicitWhileStopsAtMaxIterations() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id : {QStringLiteral("While"), QStringLiteral("Body"), QStringLiteral("After")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        if (id == QLatin1String("While"))
            instance.params[QStringLiteral("maxIterations")] = 2;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("While", "body", "Body"));
    project.addConnection(controlConnection("Body", "next", "While"));
    project.addConnection(controlConnection("While", "done", "After"));

    QStringList executionLog;
    QVERIFY(engine.loadProject(&project, [&executionLog](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("While")) {
            auto* module = new WhileControlModule(instance.id, true);
            module->execLog = &executionLog;
            return module;
        }
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &executionLog;
        return module;
    }));

    engine.runOnce();

    QCOMPARE(executionLog, QStringList({"While", "Body", "While", "Body", "While", "After"}));
    engine.clearModules();
}

void TestRunEngine::testExplicitLoopStepOnceTraversesIterations() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id : {QStringLiteral("Loop"), QStringLiteral("Body"), QStringLiteral("After")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        if (id == QLatin1String("Loop"))
            instance.params[QStringLiteral("loopCount")] = 2;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("Loop", "body", "Body"));
    project.addConnection(controlConnection("Body", "next", "Loop"));
    project.addConnection(controlConnection("Loop", "done", "After"));

    QStringList executionLog;
    QVERIFY(engine.loadProject(&project, [&executionLog](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("Loop")) {
            auto* module = new LoopControlModule(instance.id, 2);
            module->execLog = &executionLog;
            return module;
        }
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &executionLog;
        return module;
    }));

    const QStringList expected = {"Loop", "Body", "Loop", "Body", "Loop", "After"};
    for (int step = 0; step < expected.size(); ++step) {
        QVERIFY(engine.stepOnce());
        QCOMPARE(executionLog, expected.mid(0, step + 1));
    }
    engine.clearModules();
}

void TestRunEngine::testExplicitLoopCancellationStopsRun() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id : {QStringLiteral("Loop"), QStringLiteral("Body"), QStringLiteral("After")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        if (id == QLatin1String("Loop"))
            instance.params[QStringLiteral("loopCount")] = 100;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("Loop", "body", "Body"));
    project.addConnection(controlConnection("Body", "next", "Loop"));
    project.addConnection(controlConnection("Loop", "done", "After"));

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("Loop"))
            return new LoopControlModule(instance.id, 100);
        if (instance.id == QLatin1String("Body"))
            return new StopAwareModule;
        return new TestExecutionModule(instance.id);
    }));
    auto* body = qobject_cast<StopAwareModule*>(engine.getModule(QStringLiteral("Body")));
    QVERIFY(body);

    QObject signalContext;
    QStringList executionOrder;
    RunResult result;
    connect(&engine, &RunEngine::moduleStarted, &signalContext,
            [&executionOrder](const QString& moduleName) { executionOrder.append(moduleName); });
    connect(&engine, &RunEngine::runFinished, &signalContext,
            [&result](const RunResult& runResult) { result = runResult; });
    engine.runOnce();

    QCOMPARE(executionOrder, QStringList({"Loop", "Body"}));
    QVERIFY(body->hadCancellationToken);
    QVERIFY(body->sawCancellation);
    QVERIFY(!result.success);
    engine.clearModules();
}

// ---------------------------------------------------------------------------
// 阶段 E1: 多输入聚合测试
// ---------------------------------------------------------------------------

// 多输入聚合模块：两个 Number 上游 -> 聚合为 QVariantList
class MultiInputAggregatorModule : public ModuleBase {
    Q_OBJECT
public:
    QStringList* observedValues = nullptr;
    explicit MultiInputAggregatorModule(const QString& name, QStringList* obs = nullptr) : observedValues(obs) {
        m_moduleId = QStringLiteral("com.deeplux.test.agg.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
        PortSpec in;
        in.id = "values";
        in.displayName = "values";
        in.type = DataType::Number;
        in.required = true;
        in.multiple = true;
        PortSpec out;
        out.id = "result";
        out.displayName = "result";
        out.type = DataType::Number;
        setPorts({in}, {out});
    }
    QList<PortSpec> inputPorts() const override {
        PortSpec in;
        in.id = "values";
        in.displayName = "values";
        in.type = DataType::Number;
        in.required = true;
        in.multiple = true;
        return {in};
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec out;
        out.id = "result";
        out.displayName = "result";
        out.type = DataType::Number;
        return {out};
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        if (observedValues) {
            const QVariant v = input.data("values");
            if (v.type() == QVariant::List) {
                const QVariantList list = v.toList();
                QStringList parts;
                for (const QVariant& item : list)
                    parts.append(item.toString());
                observedValues->append(parts.join(","));
            } else {
                observedValues->append(v.toString());
            }
        }
        output.setData("result", 42.0);
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

void TestRunEngine::testMultiInputCollectsOrderedList() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    // A(1) -> C, B(2) -> C, C 聚合 values 端口（multiple=true）
    Project project;
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance c;
    c.id = "C";
    c.moduleId = "C";
    // 就绪顺序为 B、A；连接插入顺序故意为 A、B，验证聚合遵循执行顺序。
    project.addModule(b);
    project.addModule(a);
    project.addModule(c);
    // A -> C.values, B -> C.values（fromPort="val" 非 "image"）
    project.addConnection(dataConnection("A", "val", "C", "values"));
    project.addConnection(dataConnection("B", "val", "C", "values"));

    QStringList observed;
    QVERIFY(engine.loadProject(&project, [&observed](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "C")
            return new MultiInputAggregatorModule("C", &observed);
        return new NumberFlowModule(inst.id, {}, "val", true, nullptr, inst.id == "A" ? 1.0 : 2.0);
    }));

    RunResult result;
    QMetaObject::Connection resultConn =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& r) { result = r; });
    engine.runOnce();
    disconnect(resultConn);

    QVERIFY(result.success);
    QCOMPARE(observed.size(), 1);
    QCOMPARE(observed.first(), QStringLiteral("2,1"));
    engine.clearModules();
}

void TestRunEngine::testMultiInputTypeMismatchFails() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    // A(Image2D via "image") -> C.values(Number, multiple=true) → 类型不匹配
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance c;
    c.id = "C";
    c.moduleId = "C";
    project.addModule(a);
    project.addModule(c);
    // TestExecutionModule 输出 image 端口（Image2D），C 期望 Number
    project.addConnection(dataConnection("A", "image", "C", "values"));

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "C")
            return new MultiInputAggregatorModule("C");
        auto* m = new TestExecutionModule(inst.id);
        m->initialize();
        return m;
    }));

    RunResult result;
    QMetaObject::Connection resultConn =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& r) { result = r; });
    engine.runOnce();
    disconnect(resultConn);
    // 应失败（类型不匹配：TestExecutionModule 输出 Image2D，不是 Number）
    QVERIFY(!result.success);
    engine.clearModules();
}

void TestRunEngine::testMultiInputWaitsForAllUpstreams() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    // A -> C, B -> C(multiple). C 不应在 A 完成前执行
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    ModuleInstance c;
    c.id = "C";
    c.moduleId = "C";
    project.addModule(a);
    project.addModule(b);
    project.addModule(c);
    project.addConnection(dataConnection("A", "val", "C", "values"));
    project.addConnection(dataConnection("B", "val", "C", "values"));

    QStringList observed;
    QVERIFY(engine.loadProject(&project, [&observed](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "C")
            return new MultiInputAggregatorModule("C", &observed);
        return new NumberFlowModule(inst.id, {}, "val");
    }));

    QStringList execLog;
    QMetaObject::Connection logConn =
        connect(&engine, &RunEngine::moduleStarted, this, [&execLog](const QString& name) { execLog.append(name); });
    RunResult result;
    QMetaObject::Connection resultConn =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& r) { result = r; });
    engine.runOnce();
    disconnect(logConn);
    disconnect(resultConn);

    QVERIFY(result.success);
    QVERIFY(execLog.indexOf("A") < execLog.indexOf("C"));
    QVERIFY(execLog.indexOf("B") < execLog.indexOf("C"));
    QCOMPARE(observed.size(), 1);
    QVERIFY(observed.first().split(",").size() == 2);
    engine.clearModules();
}

// 控制汇合 All 策略模块
class ControlJoinAllModule : public ModuleBase {
    Q_OBJECT
public:
    QStringList* execLog = nullptr;
    explicit ControlJoinAllModule(const QString& name, QStringList* log = nullptr) : execLog(log) {
        m_moduleId = QStringLiteral("com.deeplux.test.joinall.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
        PortSpec ctrl;
        ctrl.id = "control";
        ctrl.displayName = "control";
        ctrl.type = DataType::Boolean;
        ctrl.control = true;
        ctrl.multiple = true;
        ctrl.joinPolicy = ControlJoinPolicy::All;
        PortSpec out;
        out.id = "image";
        out.displayName = "image";
        out.type = DataType::Image2D;
        setPorts({ctrl}, {out});
    }
    QList<PortSpec> inputPorts() const override {
        PortSpec ctrl;
        ctrl.id = "control";
        ctrl.displayName = "control";
        ctrl.type = DataType::Boolean;
        ctrl.control = true;
        ctrl.multiple = true;
        ctrl.joinPolicy = ControlJoinPolicy::All;
        return {ctrl};
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec out;
        out.id = "image";
        out.displayName = "image";
        out.type = DataType::Image2D;
        return {out};
    }
    ControlFlowType flowControlType() const override {
        return ControlFlowType::Sequential;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        if (execLog)
            execLog->append(name());
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class ControlJoinMixedModule : public ModuleBase {
    Q_OBJECT
public:
    QStringList* execLog = nullptr;
    explicit ControlJoinMixedModule(const QString& name, QStringList* log = nullptr) : execLog(log) {
        m_moduleId = QStringLiteral("com.deeplux.test.joinmixed.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
        setPorts(inputPorts(), outputPorts());
    }
    QList<PortSpec> inputPorts() const override {
        PortSpec all;
        all.id = QStringLiteral("all");
        all.displayName = all.id;
        all.type = DataType::Boolean;
        all.control = true;
        all.multiple = true;
        all.joinPolicy = ControlJoinPolicy::All;
        PortSpec any = all;
        any.id = QStringLiteral("any");
        any.displayName = any.id;
        any.joinPolicy = ControlJoinPolicy::Any;
        return {all, any};
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec out;
        out.id = QStringLiteral("image");
        out.displayName = out.id;
        out.type = DataType::Image2D;
        return {out};
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        if (execLog)
            execLog->append(name());
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class ParallelForkModule : public ModuleBase {
    Q_OBJECT
public:
    explicit ParallelForkModule(const QString& name) {
        m_moduleId = QStringLiteral("com.deeplux.test.parallelfork.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
        setPorts({}, outputPorts());
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec branch;
        branch.id = QStringLiteral("branch");
        branch.displayName = branch.id;
        branch.type = DataType::Boolean;
        branch.control = true;
        return {branch};
    }
    ControlFlowType flowControlType() const override {
        return ControlFlowType::Parallel;
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

void TestRunEngine::testControlJoinAllWaitsForAllSources() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    // A.next -> C.control, B.next -> C.control, C 声明 joinPolicy=All
    // C 不应在两个控制边都触发前执行
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    ModuleInstance c;
    c.id = "C";
    c.moduleId = "C";
    project.addModule(a);
    project.addModule(b);
    project.addModule(c);
    project.addConnection(controlConnection("A", "next", "C"));
    project.addConnection(controlConnection("B", "next", "C"));

    QStringList execLog;
    QVERIFY(engine.loadProject(&project, [&execLog](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "C")
            return new ControlJoinAllModule("C", &execLog);
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &execLog;
        return m;
    }));

    RunResult result;
    QMetaObject::Connection resultConn =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& r) { result = r; });
    engine.runOnce();
    disconnect(resultConn);

    QVERIFY(result.success);
    QVERIFY(execLog.contains("A"));
    QVERIFY(execLog.contains("B"));
    QVERIFY(execLog.contains("C"));
    QVERIFY(execLog.indexOf("A") < execLog.indexOf("C"));
    QVERIFY(execLog.indexOf("B") < execLog.indexOf("C"));
    engine.clearModules();
}

void TestRunEngine::testControlJoinTracksEdgesNotSources() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    ModuleInstance source;
    source.id = QStringLiteral("Source");
    source.moduleId = source.id;
    ModuleInstance join;
    join.id = QStringLiteral("Join");
    join.moduleId = join.id;
    project.addModule(source);
    project.addModule(join);
    project.addConnection(controlConnection("Source", "next", "Join", "allA"));
    project.addConnection(controlConnection("Source", "next", "Join", "allB"));

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("Join")) {
            class TwoAllPortsModule : public ControlJoinMixedModule {
            public:
                using ControlJoinMixedModule::ControlJoinMixedModule;
                QList<PortSpec> inputPorts() const override {
                    QList<PortSpec> ports = ControlJoinMixedModule::inputPorts();
                    ports[0].id = QStringLiteral("allA");
                    ports[0].displayName = ports[0].id;
                    ports[1].id = QStringLiteral("allB");
                    ports[1].displayName = ports[1].id;
                    ports[1].joinPolicy = ControlJoinPolicy::All;
                    return ports;
                }
            };
            return new TwoAllPortsModule(instance.id, &log);
        }
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &log;
        return module;
    }));

    engine.runOnce();
    QCOMPARE(log, QStringList({QStringLiteral("Source"), QStringLiteral("Join")}));
    engine.clearModules();
}

void TestRunEngine::testControlJoinPoliciesArePerPort() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id :
         {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("Cond"), QStringLiteral("Join")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("A", "next", "Join", "all"));
    project.addConnection(controlConnection("B", "next", "Join", "all"));
    project.addConnection(controlConnection("Cond", "true", "Join", "any"));
    project.addConnection(controlConnection("Cond", "false", "Join", "any"));

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("Join"))
            return new ControlJoinMixedModule(instance.id, &log);
        if (instance.id == QLatin1String("Cond")) {
            auto* module = new IfControlModule(instance.id, true);
            module->execLog = &log;
            return module;
        }
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &log;
        return module;
    }));

    engine.runOnce();
    QVERIFY(log.contains(QStringLiteral("Join")));
    QVERIFY(log.indexOf(QStringLiteral("A")) < log.indexOf(QStringLiteral("Join")));
    QVERIFY(log.indexOf(QStringLiteral("B")) < log.indexOf(QStringLiteral("Join")));
    QVERIFY(log.indexOf(QStringLiteral("Cond")) < log.indexOf(QStringLiteral("Join")));
    engine.clearModules();
}

// ---------------------------------------------------------------------------
// 阶段 E2: Parallel 接入主循环测试
// ---------------------------------------------------------------------------

// 线程安全的 sleep 模块（用于验证并发）
class ThreadSafeSleepModule : public ModuleBase {
    Q_OBJECT
public:
    int sleepMs = 50;
    QStringList* execLog = nullptr;
    bool sawCancellation = false;
    explicit ThreadSafeSleepModule(const QString& name, int ms = 50, QStringList* log = nullptr)
        : sleepMs(ms), execLog(log) {
        m_moduleId = QStringLiteral("com.deeplux.test.sleep.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
        setThreadSafe(true);
        PortSpec out;
        out.id = "image";
        out.displayName = "image";
        out.type = DataType::Image2D;
        setPorts({}, {out});
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec out;
        out.id = "image";
        out.displayName = "image";
        out.type = DataType::Image2D;
        return {out};
    }
    ControlFlowType flowControlType() const override {
        return ControlFlowType::Sequential;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        // 协作式取消：分片 sleep，检查取消令牌
        int remaining = sleepMs;
        while (remaining > 0) {
            int chunk = qMin(remaining, 10);
            QThread::msleep(chunk);
            remaining -= chunk;
            auto* tok = cancellationToken();
            if (tok && tok->isCancelledFast()) {
                sawCancellation = true;
                output = input;
                return true;
            }
        }
        output = input;
        if (execLog)
            execLog->append(name());
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

void TestRunEngine::testParallelBatchExecutesConcurrently() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    engine.setParallelThreadCount(2);

    // Parallel 显式分叉到 A、B；两个线程安全分支各 sleep 100ms，最后汇合到 C。
    Project project;
    ModuleInstance fork;
    fork.id = QStringLiteral("Fork");
    fork.moduleId = fork.id;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    ModuleInstance c;
    c.id = "C";
    c.moduleId = "C";
    project.addModule(fork);
    project.addModule(a);
    project.addModule(b);
    project.addModule(c);
    project.addConnection(controlConnection("Fork", "branch", "A"));
    project.addConnection(controlConnection("Fork", "branch", "B"));
    project.addConnection(controlConnection("A", "next", "C"));
    project.addConnection(controlConnection("B", "next", "C"));

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "Fork")
            return new ParallelForkModule(inst.id);
        if (inst.id == "C")
            return new ControlJoinAllModule(inst.id);
        return new ThreadSafeSleepModule(inst.id, 100);
    }));

    RunResult result;
    QMetaObject::Connection conn =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& r) { result = r; });
    engine.runOnce();
    disconnect(conn);

    QVERIFY(result.success);
    // A||B 并行约 100ms；顺序则约 200ms。
    QVERIFY2(result.elapsedMs < 200, qPrintable(QString("parallel should be < 200ms, got %1").arg(result.elapsedMs)));
    QVERIFY2(engine.lastParallelMaxConcurrency() >= 2,
             qPrintable(QString("max concurrency should be >= 2, got %1").arg(engine.lastParallelMaxConcurrency())));
    engine.clearModules();
    engine.setParallelThreadCount(1);
}

// 线程安全的快速失败模块
class ThreadSafeFailModule : public ModuleBase {
    Q_OBJECT
public:
    explicit ThreadSafeFailModule(const QString& name) {
        m_moduleId = QStringLiteral("com.deeplux.test.fail.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
        setThreadSafe(true);
        PortSpec out;
        out.id = "image";
        out.displayName = "image";
        out.type = DataType::Image2D;
        setPorts({}, {out});
    }
    QList<PortSpec> outputPorts() const override {
        PortSpec out;
        out.id = "image";
        out.displayName = "image";
        out.type = DataType::Image2D;
        return {out};
    }
    ControlFlowType flowControlType() const override {
        return ControlFlowType::Sequential;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        return false; // 总是失败
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

void TestRunEngine::testParallelFailureCancelsRemaining() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    engine.setParallelThreadCount(2);

    // A fails immediately, B sleeps 200ms — B should be cancelled
    // 控制边汇合到 C
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    ModuleInstance c;
    c.id = "C";
    c.moduleId = "C";
    project.addModule(a);
    project.addModule(b);
    project.addModule(c);
    project.addConnection(controlConnection("A", "next", "C"));
    project.addConnection(controlConnection("B", "next", "C"));

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "A")
            return new ThreadSafeFailModule("A");
        if (inst.id == "B")
            return new ThreadSafeSleepModule("B", 200);
        return new ThreadSafeSleepModule("C", 10);
    }));

    RunResult result;
    QMetaObject::Connection conn =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& r) { result = r; });
    engine.runOnce();
    disconnect(conn);

    QVERIFY(!result.success);
    // A 失败后取消 B（200ms），B 协作退出，总耗时应远小于 200ms
    QVERIFY2(result.elapsedMs < 150,
             qPrintable(QString("should not wait for B's 200ms, got %1").arg(result.elapsedMs)));
    engine.clearModules();
    engine.setParallelThreadCount(1);
}

void TestRunEngine::testParallelNonThreadSafeFallsBackSequential() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    // 两个模块标记为非线程安全，控制边汇合到 C → 顺序执行
    Project project;
    ModuleInstance a;
    a.id = "A";
    a.moduleId = "A";
    ModuleInstance b;
    b.id = "B";
    b.moduleId = "B";
    ModuleInstance c;
    c.id = "C";
    c.moduleId = "C";
    project.addModule(a);
    project.addModule(b);
    project.addModule(c);
    project.addConnection(controlConnection("A", "next", "C"));
    project.addConnection(controlConnection("B", "next", "C"));

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) -> ModuleBase* {
        auto* m = new ThreadSafeSleepModule(inst.id, 50, &log);
        m->setThreadSafe(false);
        return m;
    }));

    RunResult result;
    QMetaObject::Connection conn =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& r) { result = r; });
    engine.runOnce();
    disconnect(conn);

    QVERIFY(result.success);
    // 顺序执行 A+B：总耗时应 >= 100ms（50+50）
    QVERIFY2(result.elapsedMs >= 95, qPrintable(QString("sequential should be >= 95ms, got %1").arg(result.elapsedMs)));
    engine.clearModules();
}

void TestRunEngine::testParallelMixedThreadSafetyFallsBackSequential() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    engine.setParallelThreadCount(2);

    Project project;
    for (const QString& id : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("Join")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("A", "next", "Join"));
    project.addConnection(controlConnection("B", "next", "Join"));

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("Join"))
            return new ControlJoinAllModule(instance.id);
        auto* module = new ThreadSafeSleepModule(instance.id, 60);
        if (instance.id == QLatin1String("A"))
            module->setThreadSafe(false);
        return module;
    }));

    QElapsedTimer timer;
    timer.start();
    engine.runOnce();
    QVERIFY2(timer.elapsed() >= 110, "unsafe first module must not overlap a thread-safe sibling");
    QVERIFY(engine.lastParallelMaxConcurrency() <= 1);
    engine.clearModules();
}

void TestRunEngine::testParallelSingleThreadFailureDoesNotDeadlock() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    engine.setParallelThreadCount(1);

    Project project;
    for (const QString& id : {QStringLiteral("Fail"), QStringLiteral("Slow"), QStringLiteral("Join")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("Fail", "next", "Join"));
    project.addConnection(controlConnection("Slow", "next", "Join"));

    QVERIFY(engine.loadProject(&project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("Fail"))
            return new ThreadSafeFailModule(instance.id);
        if (instance.id == QLatin1String("Slow"))
            return new ThreadSafeSleepModule(instance.id, 200);
        return new ControlJoinAllModule(instance.id);
    }));

    RunResult result;
    const QMetaObject::Connection resultConnection =
        connect(&engine, &RunEngine::runFinished, this, [&result](const RunResult& value) { result = value; });
    QElapsedTimer timer;
    timer.start();
    engine.runOnce();
    disconnect(resultConnection);
    QVERIFY(!result.success);
    QVERIFY2(timer.elapsed() < 500, "single-thread failure must return instead of deadlocking");
    engine.clearModules();
}

void TestRunEngine::testParallelBatchHonorsDisabledModule() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    engine.setParallelThreadCount(2);

    Project project;
    ModuleInstance a;
    a.id = QStringLiteral("A");
    a.moduleId = a.id;
    ModuleInstance disabled;
    disabled.id = QStringLiteral("Disabled");
    disabled.moduleId = disabled.id;
    disabled.enabled = false;
    ModuleInstance join;
    join.id = QStringLiteral("Join");
    join.moduleId = join.id;
    project.addModule(a);
    project.addModule(disabled);
    project.addModule(join);
    project.addConnection(controlConnection("A", "next", "Join"));
    project.addConnection(controlConnection("Disabled", "next", "Join"));

    QStringList executionLog;
    QVERIFY(engine.loadProject(&project, [&executionLog](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("Join"))
            return new ControlJoinAllModule(instance.id);
        return new ThreadSafeSleepModule(instance.id, 20, &executionLog);
    }));
    QSignalSpy skippedSpy(&engine, &RunEngine::moduleSkipped);

    engine.runOnce();
    QVERIFY(!executionLog.contains(QStringLiteral("Disabled")));
    QCOMPARE(skippedSpy.count(), 1);
    QCOMPARE(skippedSpy.first().first().toString(), QStringLiteral("Disabled"));
    engine.clearModules();
}

void TestRunEngine::testParallelBatchHonorsBreakpoint() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    engine.setParallelThreadCount(2);

    Project project;
    ModuleInstance a;
    a.id = QStringLiteral("A");
    a.moduleId = a.id;
    ModuleInstance paused;
    paused.id = QStringLiteral("Paused");
    paused.moduleId = paused.id;
    paused.breakpoint = true;
    ModuleInstance join;
    join.id = QStringLiteral("Join");
    join.moduleId = join.id;
    project.addModule(a);
    project.addModule(paused);
    project.addModule(join);
    project.addConnection(controlConnection("A", "next", "Join"));
    project.addConnection(controlConnection("Paused", "next", "Join"));

    QStringList executionLog;
    QVERIFY(engine.loadProject(&project, [&executionLog](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("Join"))
            return new ControlJoinAllModule(instance.id, &executionLog);
        return new ThreadSafeSleepModule(instance.id, 20, &executionLog);
    }));
    QSignalSpy hitSpy(&engine, &RunEngine::breakpointHit);

    engine.runOnce();
    QCOMPARE(hitSpy.count(), 1);
    QVERIFY(engine.isPausedAtBreakpoint());
    QVERIFY(!executionLog.contains(QStringLiteral("Paused")));

    engine.resume();
    QVERIFY(executionLog.contains(QStringLiteral("Paused")));
    QVERIFY(executionLog.contains(QStringLiteral("Join")));
    engine.clearModules();
}

void TestRunEngine::testExplicitLoopZeroIterations() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // Loop(count=0) -> body(should not execute) -> done -> after
    Project project;
    ModuleInstance loop;
    loop.id = "loop";
    loop.moduleId = "loop";
    loop.params["loopCount"] = 0;
    ModuleInstance body;
    body.id = "body";
    body.moduleId = "body";
    ModuleInstance after;
    after.id = "after";
    after.moduleId = "after";
    project.addModule(loop);
    project.addModule(body);
    project.addModule(after);
    // Control edges: loop.body -> body, loop.done -> after, body.next -> loop (back-edge)
    ModuleConnection ctrlBody;
    ctrlBody.fromModuleId = "loop";
    ctrlBody.toModuleId = "body";
    ctrlBody.fromPort = "body";
    ctrlBody.toPort = "control";
    ctrlBody.edgeType = "control";
    project.addConnection(ctrlBody);
    ModuleConnection ctrlDone;
    ctrlDone.fromModuleId = "loop";
    ctrlDone.toModuleId = "after";
    ctrlDone.fromPort = "done";
    ctrlDone.toPort = "control";
    ctrlDone.edgeType = "control";
    project.addConnection(ctrlDone);
    ModuleConnection ctrlBack;
    ctrlBack.fromModuleId = "body";
    ctrlBack.toModuleId = "loop";
    ctrlBack.fromPort = "next";
    ctrlBack.toPort = "control";
    ctrlBack.edgeType = "control";
    project.addConnection(ctrlBack);

    QStringList log;
    QStringList skipped;
    QMetaObject::Connection skipConn =
        connect(&engine, &RunEngine::moduleSkipped, [&](const QString& n) { skipped.append(n); });
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "loop") {
            auto* m = new LoopControlModule(inst.id, inst.params["loopCount"].toInt(3));
            m->execLog = &log;
            return m;
        }
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));
    engine.runOnce();
    QCOMPARE(log, QStringList({"loop", "after"}));
    QVERIFY(skipped.contains("body"));
    disconnect(skipConn);
    engine.clearModules();
}

void TestRunEngine::testExplicitLoopThreeIterations() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // Loop(count=3) -> body x3 -> done -> after
    Project project;
    ModuleInstance loop;
    loop.id = "loop";
    loop.moduleId = "loop";
    loop.params["loopCount"] = 3;
    ModuleInstance body;
    body.id = "body";
    body.moduleId = "body";
    ModuleInstance after;
    after.id = "after";
    after.moduleId = "after";
    project.addModule(loop);
    project.addModule(body);
    project.addModule(after);
    ModuleConnection ctrlBody;
    ctrlBody.fromModuleId = "loop";
    ctrlBody.toModuleId = "body";
    ctrlBody.fromPort = "body";
    ctrlBody.toPort = "control";
    ctrlBody.edgeType = "control";
    project.addConnection(ctrlBody);
    ModuleConnection ctrlDone;
    ctrlDone.fromModuleId = "loop";
    ctrlDone.toModuleId = "after";
    ctrlDone.fromPort = "done";
    ctrlDone.edgeType = "control";
    ctrlDone.toPort = "control";
    project.addConnection(ctrlDone);
    ModuleConnection ctrlBack;
    ctrlBack.fromModuleId = "body";
    ctrlBack.toModuleId = "loop";
    ctrlBack.fromPort = "next";
    ctrlBack.toPort = "control";
    ctrlBack.edgeType = "control";
    project.addConnection(ctrlBack);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "loop") {
            auto* m = new LoopControlModule(inst.id, inst.params["loopCount"].toInt(3));
            m->execLog = &log;
            return m;
        }
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));
    engine.runOnce();
    QCOMPARE(log, QStringList({"loop", "body", "loop", "body", "loop", "body", "loop", "after"}));
    engine.clearModules();
}

void TestRunEngine::testExplicitWhileFalseZeroIterations() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // While(false) -> body(0 times) -> done -> after
    Project project;
    ModuleInstance whileMod;
    whileMod.id = "whileMod";
    whileMod.moduleId = "whileMod";
    whileMod.params["maxIterations"] = 100;
    ModuleInstance body;
    body.id = "body";
    body.moduleId = "body";
    ModuleInstance after;
    after.id = "after";
    after.moduleId = "after";
    project.addModule(whileMod);
    project.addModule(body);
    project.addModule(after);
    ModuleConnection ctrlBody;
    ctrlBody.fromModuleId = "whileMod";
    ctrlBody.toModuleId = "body";
    ctrlBody.fromPort = "body";
    ctrlBody.toPort = "control";
    ctrlBody.edgeType = "control";
    project.addConnection(ctrlBody);
    ModuleConnection ctrlDone;
    ctrlDone.fromModuleId = "whileMod";
    ctrlDone.toModuleId = "after";
    ctrlDone.fromPort = "done";
    ctrlDone.toPort = "control";
    ctrlDone.edgeType = "control";
    project.addConnection(ctrlDone);
    ModuleConnection ctrlBack;
    ctrlBack.fromModuleId = "body";
    ctrlBack.toModuleId = "whileMod";
    ctrlBack.fromPort = "next";
    ctrlBack.toPort = "control";
    ctrlBack.edgeType = "control";
    project.addConnection(ctrlBack);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "whileMod") {
            auto* m = new WhileControlModule(inst.id, false); // condition false
            m->execLog = &log;
            return m;
        }
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));
    engine.runOnce();
    // While executes once (done), body 0 times, after 1 time
    QVERIFY(log.contains("whileMod"));
    QVERIFY(!log.contains("body"));
    QVERIFY(log.contains("after"));
    engine.clearModules();
}

void TestRunEngine::testExplicitStopWhileTerminatesLoop() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();
    // While(true) -> body -> StopWhile -> done (loop terminated by StopWhile)
    Project project;
    ModuleInstance whileMod;
    whileMod.id = "whileMod";
    whileMod.moduleId = "whileMod";
    whileMod.params["maxIterations"] = 100;
    ModuleInstance body;
    body.id = "body";
    body.moduleId = "body";
    ModuleInstance stopMod;
    stopMod.id = "stopMod";
    stopMod.moduleId = "stopMod";
    ModuleInstance after;
    after.id = "after";
    after.moduleId = "after";
    project.addModule(whileMod);
    project.addModule(body);
    project.addModule(stopMod);
    project.addModule(after);
    // while.body -> body, body.next -> stopMod, stopMod.stop -> whileMod(done target = after)
    ModuleConnection ctrlBody;
    ctrlBody.fromModuleId = "whileMod";
    ctrlBody.toModuleId = "body";
    ctrlBody.fromPort = "body";
    ctrlBody.toPort = "control";
    ctrlBody.edgeType = "control";
    project.addConnection(ctrlBody);
    ModuleConnection ctrlToStop;
    ctrlToStop.fromModuleId = "body";
    ctrlToStop.toModuleId = "stopMod";
    ctrlToStop.fromPort = "next";
    ctrlToStop.toPort = "control";
    ctrlToStop.edgeType = "control";
    project.addConnection(ctrlToStop);
    // StopWhile.stop -> after (jumps to after, skipping back-edge to whileMod)
    ModuleConnection ctrlStop;
    ctrlStop.fromModuleId = "stopMod";
    ctrlStop.toModuleId = "after";
    ctrlStop.fromPort = "stop";
    ctrlStop.toPort = "control";
    ctrlStop.edgeType = "control";
    project.addConnection(ctrlStop);
    // while.done -> after (normal exit path)
    ModuleConnection ctrlDone;
    ctrlDone.fromModuleId = "whileMod";
    ctrlDone.toModuleId = "after";
    ctrlDone.fromPort = "done";
    ctrlDone.toPort = "control";
    ctrlDone.edgeType = "control";
    project.addConnection(ctrlDone);

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& inst) -> ModuleBase* {
        if (inst.id == "whileMod") {
            auto* m = new WhileControlModule(inst.id, true); // condition true
            m->execLog = &log;
            return m;
        }
        if (inst.id == "stopMod") {
            auto* m = new StopWhileControlModule(inst.id);
            m->execLog = &log;
            return m;
        }
        auto* m = new TestExecutionModule(inst.id);
        m->executionLog = &log;
        return m;
    }));
    engine.runOnce();
    // While executes once (body), StopWhile executes, after executes
    // Body only runs once (StopWhile terminates the loop)
    QVERIFY(log.contains("whileMod"));
    QCOMPARE(log.count("body"), 1);
    QVERIFY(log.contains("stopMod"));
    QVERIFY(log.contains("after"));
    engine.clearModules();
}

void TestRunEngine::testDisabledExplicitLoopExitsThroughDone() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    ModuleInstance loop;
    loop.id = QStringLiteral("loop");
    loop.moduleId = QStringLiteral("loop");
    loop.enabled = false;
    loop.params[QStringLiteral("loopCount")] = 3;
    ModuleInstance body;
    body.id = QStringLiteral("body");
    body.moduleId = QStringLiteral("body");
    ModuleInstance after;
    after.id = QStringLiteral("after");
    after.moduleId = QStringLiteral("after");
    project.addModule(loop);
    project.addModule(body);
    project.addModule(after);
    project.addConnection(controlConnection("loop", "body", "body"));
    project.addConnection(controlConnection("body", "next", "loop"));
    project.addConnection(controlConnection("loop", "done", "after"));

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("loop")) {
            auto* module = new LoopControlModule(instance.id, 3);
            module->execLog = &log;
            return module;
        }
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &log;
        return module;
    }));
    engine.runOnce();

    QCOMPARE(log, QStringList({QStringLiteral("after")}));
    engine.clearModules();
}

void TestRunEngine::testExplicitLoopMultiModuleBody() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id :
         {QStringLiteral("loop"), QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("after")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        if (id == QLatin1String("loop"))
            instance.params[QStringLiteral("loopCount")] = 3;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("loop", "body", "first"));
    project.addConnection(controlConnection("first", "next", "second"));
    project.addConnection(controlConnection("second", "next", "loop"));
    project.addConnection(controlConnection("loop", "done", "after"));

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("loop")) {
            auto* module = new LoopControlModule(instance.id, 3);
            module->execLog = &log;
            return module;
        }
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &log;
        return module;
    }));
    engine.runOnce();

    QCOMPARE(log, QStringList({"loop", "first", "second", "loop", "first", "second", "loop", "first", "second", "loop",
                               "after"}));
    engine.clearModules();
}

void TestRunEngine::testExplicitLoopAfterPredecessor() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id :
         {QStringLiteral("start"), QStringLiteral("loop"), QStringLiteral("body"), QStringLiteral("after")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        if (id == QLatin1String("loop"))
            instance.params[QStringLiteral("loopCount")] = 1;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("start", "next", "loop"));
    project.addConnection(controlConnection("loop", "body", "body"));
    project.addConnection(controlConnection("body", "next", "loop"));
    project.addConnection(controlConnection("loop", "done", "after"));

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("loop")) {
            auto* module = new LoopControlModule(instance.id, 1);
            module->execLog = &log;
            return module;
        }
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &log;
        return module;
    }));
    engine.runOnce();

    QCOMPARE(log, QStringList({"start", "loop", "body", "loop", "after"}));
    engine.clearModules();
}

void TestRunEngine::testLoopSelfEdgeRejected() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    ModuleInstance loop;
    loop.id = QStringLiteral("loop");
    loop.moduleId = QStringLiteral("loop");
    loop.params[QStringLiteral("loopCount")] = 0;
    project.addModule(loop);
    project.addConnection(controlConnection("loop", "next", "loop"));

    QVERIFY(!engine.loadProject(&project,
                                [](const ModuleInstance& instance) { return new LoopControlModule(instance.id, 0); }));
    engine.clearModules();
}

void TestRunEngine::testLoopDoneBackEdgeRejected() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id : {QStringLiteral("loop"), QStringLiteral("body"), QStringLiteral("after")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("loop", "body", "body"));
    project.addConnection(controlConnection("loop", "done", "after"));
    project.addConnection(controlConnection("after", "next", "loop"));

    QVERIFY(!engine.loadProject(&project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("loop"))
            return new LoopControlModule(instance.id, 1);
        return new TestExecutionModule(instance.id);
    }));
    engine.clearModules();
}

void TestRunEngine::testNestedStopLoopPreservesOuterCounter() {
    RunEngine& engine = RunEngine::instance();
    engine.clearModules();

    Project project;
    for (const QString& id : {QStringLiteral("outer"), QStringLiteral("inner"), QStringLiteral("stop"),
                              QStringLiteral("tail"), QStringLiteral("after")}) {
        ModuleInstance instance;
        instance.id = id;
        instance.moduleId = id;
        if (id == QLatin1String("outer"))
            instance.params[QStringLiteral("loopCount")] = 2;
        if (id == QLatin1String("inner"))
            instance.params[QStringLiteral("maxIterations")] = 100;
        project.addModule(instance);
    }
    project.addConnection(controlConnection("outer", "body", "inner"));
    project.addConnection(controlConnection("inner", "body", "stop"));
    project.addConnection(controlConnection("stop", "stop", "tail"));
    project.addConnection(controlConnection("tail", "next", "outer"));
    project.addConnection(controlConnection("outer", "done", "after"));

    QStringList log;
    QVERIFY(engine.loadProject(&project, [&log](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("outer")) {
            auto* module = new LoopControlModule(instance.id, 2);
            module->execLog = &log;
            return module;
        }
        if (instance.id == QLatin1String("inner")) {
            auto* module = new WhileControlModule(instance.id, true);
            module->execLog = &log;
            return module;
        }
        if (instance.id == QLatin1String("stop")) {
            auto* module = new StopWhileControlModule(instance.id);
            module->execLog = &log;
            return module;
        }
        auto* module = new TestExecutionModule(instance.id);
        module->executionLog = &log;
        return module;
    }));
    engine.runOnce();

    QCOMPARE(log, QStringList({"outer", "inner", "stop", "tail", "outer", "inner", "stop", "tail", "outer", "after"}));
    engine.clearModules();
}

QTEST_MAIN(TestRunEngine)
#include "test_runengine.moc"
