#define DEEPLUX_BUILD_TESTS 1
#include "core/base/ModuleBase.h"
#include "core/engine/FlowExecutionService.h"
#include "core/engine/RunEngine.h"
#include "core/model/Project.h"

#include <QElapsedTimer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>
#include <atomic>
#include <stdexcept>
#include <thread>

using namespace DeepLux;

namespace {

/// 非阻塞探针：记录执行线程与次数（B5.1 计划用例探针）。
class ServiceProbeModule : public ModuleBase {
public:
    explicit ServiceProbeModule(const QString& name = QStringLiteral("ServiceProbe")) {
        m_moduleId = QStringLiteral("com.deeplux.test.service.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    std::atomic<Qt::HANDLE> threadId{nullptr};
    std::atomic<int> runs{0};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        threadId.store(QThread::currentThreadId());
        ++runs;
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

/// 阻塞探针：进入 process 后等待测试放行（确定性屏障）。
class BlockingServiceProbe : public ModuleBase {
public:
    explicit BlockingServiceProbe(const QString& name) {
        m_moduleId = QStringLiteral("com.deeplux.test.service.blocking.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    QSemaphore entered;
    QSemaphore release;
    std::atomic<int> runs{0};
    std::atomic<Qt::HANDLE> threadId{nullptr};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        threadId.store(QThread::currentThreadId());
        ++runs;
        entered.release();
        // 确定性放行：release 信号量；stop() 取消时协作退出（20ms 仅作取消轮询防挂死）。
        while (!isCancellationRequested()) {
            if (release.tryAcquire(1, 20)) {
                break;
            }
        }
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

/// 记录执行顺序的探针（Step/用户暂停用）。
class CountingServiceProbe : public ModuleBase {
public:
    CountingServiceProbe(const QString& name, QStringList* log) : m_log(log) {
        m_moduleId = QStringLiteral("com.deeplux.test.service.count.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }
    std::atomic<int> runs{0};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        ++runs;
        if (m_log) {
            m_log->append(name());
        }
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QStringList* m_log = nullptr;
};

/// 顺序控制图 A->B->C（A 可阻塞/带断点），用于暂停/恢复语义。
struct ServiceFlow {
    Project project;
    QSemaphore entered;
    QSemaphore release;
    QStringList log;
    std::atomic<int> aRuns{0};
    std::atomic<int> bRuns{0};
    std::atomic<int> cRuns{0};
};

class ServiceFlowProbe : public ModuleBase {
public:
    ServiceFlowProbe(const QString& name, ServiceFlow* flow, bool blocking, std::atomic<int>* counter)
        : m_flow(flow), m_blocking(blocking), m_counter(counter) {
        m_moduleId = QStringLiteral("com.deeplux.test.service.flow.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        if (m_blocking) {
            m_flow->entered.release();
            m_flow->release.acquire();
        }
        if (m_flow) {
            m_flow->log.append(name());
        }
        if (m_counter) {
            ++(*m_counter);
        }
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    ServiceFlow* m_flow = nullptr;
    bool m_blocking = false;
    std::atomic<int>* m_counter = nullptr;
};

bool loadServiceFlow(ServiceFlow& flow, bool blockFirst, bool firstBreakpoint) {
    ModuleInstance a;
    a.id = QStringLiteral("A");
    a.moduleId = QStringLiteral("A");
    a.breakpoint = firstBreakpoint;
    ModuleInstance b;
    b.id = QStringLiteral("B");
    b.moduleId = QStringLiteral("B");
    ModuleInstance c;
    c.id = QStringLiteral("C");
    c.moduleId = QStringLiteral("C");
    flow.project.addModule(a);
    flow.project.addModule(b);
    flow.project.addModule(c);

    ModuleConnection ab;
    ab.fromModuleId = QStringLiteral("A");
    ab.toModuleId = QStringLiteral("B");
    ab.fromPort = QStringLiteral("next");
    ab.toPort = QStringLiteral("control");
    ab.edgeType = QStringLiteral("control");
    flow.project.addConnection(ab);
    ModuleConnection bc;
    bc.fromModuleId = QStringLiteral("B");
    bc.toModuleId = QStringLiteral("C");
    bc.fromPort = QStringLiteral("next");
    bc.toPort = QStringLiteral("control");
    bc.edgeType = QStringLiteral("control");
    flow.project.addConnection(bc);

    const bool loaded = RunEngine::instance().loadProject(
        &flow.project, [&flow, blockFirst](const ModuleInstance& inst) -> ModuleBase* {
            if (inst.id == QLatin1String("A")) {
                return new ServiceFlowProbe(QStringLiteral("A"), &flow, blockFirst, &flow.aRuns);
            }
            if (inst.id == QLatin1String("B")) {
                return new ServiceFlowProbe(QStringLiteral("B"), &flow, false, &flow.bRuns);
            }
            return new ServiceFlowProbe(QStringLiteral("C"), &flow, false, &flow.cRuns);
        });
    if (loaded) {
        // 探针未注册插件（默认 gui）：Service 工作线程执行必须以 worker 亲和内联执行。
        for (const QString& name : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}) {
            RunEngine::instance().setModuleExecutionAffinityForTest(name, ExecutionAffinity::Worker);
        }
    }
    return loaded;
}

} // namespace

class TestFlowExecutionService : public QObject {
    Q_OBJECT
private slots:
    void init() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        engine.stop();
        engine.clearFinalizeHookForTest();
        engine.clearExecutionFaultInjectionForTest();
        service.stop();
        service.clearAfterStartSettledHookForTest();
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Idle, 5000);
        engine.stop();
        engine.clearModules();
    }

    // B5.1 计划用例：Single 在服务工作线程执行并完成。
    void testSingleRunsOnWorkerThreadAndFinishes() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(probe.runs.load(), 1);
        QVERIFY2(probe.threadId.load() != QThread::currentThreadId(), "Single job must run on the service worker");
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QCOMPARE(service.currentJob(), FlowExecutionService::JobType::None);
        QVERIFY(!engine.isBusy());
    }

    // B5.1 计划用例：工作线程执行期间主线程可见 Running/Single 状态。
    void testStateIsVisibleAcrossThreads() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        BlockingServiceProbe probe(QStringLiteral("BlockingProbe"));
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("BlockingProbe"), ExecutionAffinity::Worker);

        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QVERIFY2(probe.entered.tryAcquire(1, 5000), "single job must enter module execution");
        QCOMPARE(service.state(), FlowExecutionService::State::Running);
        QCOMPARE(service.currentJob(), FlowExecutionService::JobType::Single);
        QVERIFY2(engine.isBusy(), "job lease must keep the engine busy during execution");
        probe.release.release();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(!engine.isBusy());
    }

    // B5.2：Step 每次一步，跨 job 保留单步位置。
    void testStepRunsOneModulePerJob() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        QStringList log;
        CountingServiceProbe first(QStringLiteral("First"), &log);
        CountingServiceProbe second(QStringLiteral("Second"), &log);
        QVERIFY(first.initialize());
        QVERIFY(second.initialize());
        engine.addModule(&first);
        engine.addModule(&second);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("First"), ExecutionAffinity::Worker);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("Second"), ExecutionAffinity::Worker);

        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.stepOnce());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(log, QStringList({QStringLiteral("First")}));
        QCOMPARE(service.currentJob(), FlowExecutionService::JobType::None);

        QVERIFY(service.stepOnce());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 2, 5000);
        QCOMPARE(log, QStringList({QStringLiteral("First"), QStringLiteral("Second")}));
    }

    // A bounded service cycle preserves the caller's identities and authorization snapshot.
    void testBoundedCyclePreservesIdentityAndAuthorization() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        FlowExecutionService::JobRequest request;
        request.maxCycles = 3;
        request.intervalMs = 30;
        request.authorization = {{QStringLiteral("executionAuthorizationId"), QStringLiteral("exec-1")},
                                 {QStringLiteral("confirmationId"), QStringLiteral("confirm-1")},
                                 {QStringLiteral("callId"), QStringLiteral("call-1")}};
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startCycle(&request));
        QVERIFY(!request.runId.isEmpty());
        QVERIFY(!request.sessionId.isEmpty());
        QCOMPARE(service.activeJobRequestSnapshot().authorization, request.authorization);
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() >= 1, 5000);
        const RunObservation current = service.currentRunObservation();
        QCOMPARE(current.sessionId, request.sessionId);
        QVERIFY(!current.runId.isEmpty());

        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), false);
        QCOMPARE(probe.runs.load(), 3);
        const RunObservation session = engine.runObservation(request.sessionId);
        QCOMPARE(session.runId, request.sessionId);
        QCOMPARE(session.maxCycles, 3);
        QCOMPARE(session.completedCycles, 3);
        QCOMPARE(session.lastSuccessfulRunId, service.currentRunObservation().runId);
    }

    // The accepted identity is queryable before the first engine run starts; a stop in this
    // deterministic window must close both the run and cycle-session observations.
    void testAcceptedCycleIsObservableAndCancelableBeforeFirstRun() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        QSemaphore hookEntered;
        QSemaphore releaseHook;
        service.setAfterStartSettledHookForTest([&]() {
            hookEntered.release();
            releaseHook.acquire();
        });

        FlowExecutionService::JobRequest request;
        request.maxCycles = 2;
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startCycle(&request));
        QVERIFY(!request.runId.isEmpty());
        QVERIFY(!request.sessionId.isEmpty());
        QVERIFY(hookEntered.tryAcquire(1, 5000));

        const RunObservation pendingRun = engine.runObservation(request.runId);
        QCOMPARE(pendingRun.runId, request.runId);
        QCOMPARE(pendingRun.sessionId, request.sessionId);
        QCOMPARE(pendingRun.state, RunState::Running);
        QVERIFY(!pendingRun.finished);
        QCOMPARE(service.currentRunObservation().runId, request.runId);
        const RunObservation pendingSession = engine.runObservation(request.sessionId);
        QCOMPARE(pendingSession.runId, request.sessionId);
        QCOMPARE(pendingSession.state, RunState::Running);
        QVERIFY(!pendingSession.finished);
        QCOMPARE(probe.runs.load(), 0);

        service.stop();
        releaseHook.release();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(0).toInt(), static_cast<int>(FlowExecutionService::JobType::Cycle));
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);

        const RunObservation cancelledRun = engine.runObservation(request.runId);
        QCOMPARE(cancelledRun.runId, request.runId);
        QVERIFY(cancelledRun.finished);
        QVERIFY(!cancelledRun.result.success);
        QCOMPARE(cancelledRun.result.errorCode, ExecError::Cancelled);
        QCOMPARE(cancelledRun.state, RunState::Stopped);
        const RunObservation cancelledSession = engine.runObservation(request.sessionId);
        QCOMPARE(cancelledSession.runId, request.sessionId);
        QVERIFY(cancelledSession.finished);
        QVERIFY(!cancelledSession.result.success);
        QCOMPARE(cancelledSession.result.errorCode, ExecError::Cancelled);
        QCOMPARE(cancelledSession.state, RunState::Stopped);
        QCOMPARE(probe.runs.load(), 0);
        service.clearAfterStartSettledHookForTest();
    }

    // B5.2：Cycle 至少两轮；轮间 job 租约保持 isBusy，维护被拒绝。
    void testCycleRunsAtLeastTwoRoundsAndKeepsEngineBusy() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        // 停止时不产生下一轮：在 jobFinished 发出瞬间记录运行次数（DirectConnection 在工作者线程）。
        std::atomic<int> runsAtFinish{-1};
        const QMetaObject::Connection conn = connect(
            &service, &FlowExecutionService::jobFinished, this,
            [&runsAtFinish, &probe](FlowExecutionService::JobType, bool) { runsAtFinish = probe.runs.load(); },
            Qt::DirectConnection);
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);

        QVERIFY(service.startCycle());
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() >= 2, 5000);
        QCOMPARE(service.state(), FlowExecutionService::State::Running);
        QCOMPARE(service.currentJob(), FlowExecutionService::JobType::Cycle);
        QVERIFY2(engine.isBusy(), "Cycle lease must keep the engine busy (including between rounds)");
        const int moduleCount = engine.modules().size();
        QVERIFY2(!engine.loadProject(nullptr), "loadProject must be rejected during a Cycle job");
        engine.clearModules();
        QCOMPARE(engine.modules().size(), moduleCount);

        service.stop();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), true); // stopped
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(!engine.isBusy());
        QVERIFY(runsAtFinish.load() >= 2);
        QCOMPARE(probe.runs.load(), runsAtFinish.load()); // jobFinished 之后不得再有新轮
        disconnect(conn);
    }

    // B5.2：stop 立即返回、无下一轮、终态 Idle，停止的 job 上报 stopped。
    void testStopEndsJobImmediatelyAndReportsStopped() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        BlockingServiceProbe probe(QStringLiteral("BlockingProbe"));
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("BlockingProbe"), ExecutionAffinity::Worker);

        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QVERIFY(probe.entered.tryAcquire(1, 5000));
        QElapsedTimer timer;
        timer.start();
        service.stop();
        QVERIFY2(timer.elapsed() < 1000, "stop() must return immediately");
        probe.release.release();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(!engine.isBusy());
        QVERIFY(service.lastJobStopped());
    }

    // B5.2：shutdown 非 Idle 返回 false；Idle 返回 true，之后可再次启动（线程重建）。
    void testShutdownOnlyWhenIdle() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        BlockingServiceProbe probe(QStringLiteral("BlockingProbe"));
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("BlockingProbe"), ExecutionAffinity::Worker);

        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QVERIFY(probe.entered.tryAcquire(1, 5000));
        QVERIFY2(!service.shutdown(), "shutdown must return false while a job is active");
        probe.release.release();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Idle, 5000);
        QVERIFY2(service.shutdown(), "shutdown must succeed when Idle");
        // t11 F3：Idle 且无线程时重复 shutdown 仍为 true（幂等；true 只在真实销毁后返回）。
        QVERIFY2(service.shutdown(), "shutdown must be idempotent when Idle");

        // 线程销毁后可按需重建。
        QSignalSpy nextSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        probe.release.release(); // 放行第二次执行的阻塞探针
        QTRY_COMPARE_WITH_TIMEOUT(nextSpy.count(), 1, 5000);
        QCOMPARE(probe.runs.load(), 2);
    }

    // B5.2：非 Idle 时启动/单步拒绝（不排队），continue/pause 状态校验。
    void testStartRejectedWhileBusy() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        BlockingServiceProbe probe(QStringLiteral("BlockingProbe"));
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("BlockingProbe"), ExecutionAffinity::Worker);

        QVERIFY(!service.pauseRun());
        QVERIFY(!service.continueRun());
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QVERIFY(probe.entered.tryAcquire(1, 5000));
        QVERIFY(!service.startSingle());
        QVERIFY(!service.startCycle());
        QVERIFY(!service.stepOnce());
        probe.release.release();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QVERIFY(!service.continueRun()); // 不在 Paused
    }

    // B5.2/O2：用户暂停在模块边界生效；暂停期间无 runFinished；继续后 B/C 各执行一次。
    void testUserPauseAndResumeAtModuleBoundary() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceFlow flow;
        QVERIFY(loadServiceFlow(flow, /*blockFirst=*/true, /*firstBreakpoint=*/false));
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QSignalSpy runFinishedSpy(&engine, &RunEngine::runFinished);

        QVERIFY(service.startSingle());
        QVERIFY2(flow.entered.tryAcquire(1, 5000), "A must enter execution");
        QVERIFY(service.pauseRun());
        flow.release.release();
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Paused, 5000);
        QCOMPARE(flow.aRuns.load(), 1);
        QCOMPARE(flow.bRuns.load(), 0);
        QCOMPARE(flow.cRuns.load(), 0);
        QCOMPARE(runFinishedSpy.count(), 0); // 暂停期间不得发 runFinished
        QVERIFY(!engine.isPausedAtBreakpoint());
        QCOMPARE(engine.pauseReason(), PauseReason::User);

        QVERIFY(service.continueRun());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), false);
        QCOMPARE(flow.aRuns.load(), 1);
        QCOMPARE(flow.bRuns.load(), 1);
        QCOMPARE(flow.cRuns.load(), 1);
        QCOMPARE(flow.log, QStringList({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}));
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
    }

    // B5.2：断点暂停经 Service 暴露 Paused；continueRun 恢复（Resume 段）后完成。
    void testBreakpointPauseAndResume() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceFlow flow;
        QVERIFY(loadServiceFlow(flow, /*blockFirst=*/false, /*firstBreakpoint=*/true));
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);

        QVERIFY(service.startSingle());
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Paused, 5000);
        QVERIFY(engine.isPausedAtBreakpoint());
        QCOMPARE(engine.pauseReason(), PauseReason::Breakpoint);
        QCOMPARE(flow.aRuns.load(), 0);
        QCOMPARE(finishedSpy.count(), 0);

        QVERIFY(service.continueRun());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(flow.aRuns.load(), 1);
        QCOMPARE(flow.bRuns.load(), 1);
        QCOMPARE(flow.cRuns.load(), 1);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
    }

    // B5.2：Cycle 暂停/继续（轮间由 Service 自身等待）。
    void testCyclePauseAndResume() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        std::atomic<int> runsAtPause{-1};
        const QMetaObject::Connection conn = connect(
            &service, &FlowExecutionService::jobStateChanged, this,
            [&runsAtPause, &probe](FlowExecutionService::State state, FlowExecutionService::JobType) {
                if (state == FlowExecutionService::State::Paused) {
                    runsAtPause = probe.runs.load();
                }
            },
            Qt::DirectConnection);
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);

        QVERIFY(service.startCycle());
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() >= 2, 5000);
        QVERIFY(service.pauseRun());
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Paused, 5000);
        // t11 F2：state 的 QTRY 可能先于 Paused 信号的 DirectConnection 回调可见；
        // 必须先在回调写入的屏障上等待，再做相等比较（不得读早于回调）。
        QTRY_VERIFY_WITH_TIMEOUT(runsAtPause.load() >= 2, 5000);
        QCOMPARE(probe.runs.load(), runsAtPause.load()); // 暂停期间无新轮
        QVERIFY(service.continueRun());
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() > runsAtPause.load(), 5000);
        service.stop();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        disconnect(conn);
    }

    // t11 F1：Paused/Stopping 只接受 continueRun()/stop()，pauseRun 必须返回 false。
    void testPauseRejectedWhenAlreadyPaused() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startCycle());
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() >= 1, 5000);
        QVERIFY(service.pauseRun());
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Paused, 5000);
        QVERIFY2(!service.pauseRun(), "pauseRun must be rejected while Paused (only continueRun/stop)");
        QVERIFY2(!service.pauseRun(), "repeated pauseRun must stay rejected while Paused");
        service.stop(); // Paused 下只接受 continueRun()/stop()
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(!engine.isBusy());
    }

    // t11 F1：Cycle 暂停中重复 pauseRun 后 continueRun——不得因残留请求再次进入 Paused，
    // 后续轮次必须正常执行。
    void testRepeatedPauseWhilePausedDoesNotRePauseCycle() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        std::atomic<int> runsAtPause{-1};
        std::atomic<int> pausedCount{0};
        const QMetaObject::Connection conn = connect(
            &service, &FlowExecutionService::jobStateChanged, this,
            [&runsAtPause, &pausedCount, &probe](FlowExecutionService::State state, FlowExecutionService::JobType) {
                if (state == FlowExecutionService::State::Paused) {
                    ++pausedCount;
                    runsAtPause = probe.runs.load();
                }
            },
            Qt::DirectConnection);
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);

        QVERIFY(service.startCycle());
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() >= 2, 5000);
        QVERIFY(service.pauseRun());
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Paused, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(runsAtPause.load() >= 2, 5000);
        // 暂停中重复 pauseRun 被拒绝，且不得登记残留请求。
        QVERIFY(!service.pauseRun());
        QVERIFY(!service.pauseRun());
        QVERIFY(service.continueRun());
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() > runsAtPause.load(), 5000); // 后续轮正常执行
        QCOMPARE(pausedCount.load(), 1);                                        // 未再次进入 Paused
        service.stop();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
        QCOMPARE(pausedCount.load(), 1);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        disconnect(conn);
    }

    // t8 低覆盖：Cycle 两轮之间（lease 保持 busy）已由 testCycleRunsAtLeastTwoRounds 覆盖；
    // 这里补 *ForJob 的 start_rejected 分支（状态不允许启动，非 owner/cancel 失败）。
    void testOwnedStartRejectedWhilePaused() {
        RunEngine& engine = RunEngine::instance();
        ServiceFlow flow;
        QVERIFY(loadServiceFlow(flow, /*blockFirst=*/false, /*firstBreakpoint=*/true));

        RunEngine::JobToken token;
        QString reason;
        QVERIFY(engine.beginExternalJob(&token, &reason));
        QVERIFY2(engine.runOnceForJob(token, &reason), qPrintable(reason));
        QVERIFY(engine.isPausedAtBreakpoint());

        QString rejectedReason;
        QVERIFY(!engine.runOnceForJob(token, &rejectedReason));
        QCOMPARE(rejectedReason, QStringLiteral("start_rejected"));
        QCOMPARE(flow.aRuns.load(), 0);

        engine.stop();
        QVERIFY(engine.endExternalJob(token));
    }

    // t6 O2：引擎空闲（Cycle 轮间/启动窗口）时 pauseForJob 返回 false 属正常，
    // 不得当成失败；随后启动仍可正常执行。
    void testPauseRequestWhileEngineIdleIsNotFailure() {
        RunEngine& engine = RunEngine::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        RunEngine::JobToken token;
        QVERIFY(engine.beginExternalJob(&token, nullptr));
        QVERIFY2(!engine.pauseForJob(token), "engine idle must refuse the module-boundary pause request");
        QString reason;
        QVERIFY2(engine.runOnceForJob(token, &reason), qPrintable(reason));
        QCOMPARE(probe.runs.load(), 1);
        QVERIFY(engine.endExternalJob(token));
    }

    // t8 低覆盖：job 内引擎异常后 Service 正常收尾，引擎可再次运行。
    void testJobInternalExceptionAllowsNextRun() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        engine.setExecutionFaultInjectionForTest([]() { throw std::runtime_error("injected service job fault"); });
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QSignalSpy failedSpy(&service, &FlowExecutionService::jobFailed);
        QVERIFY(service.startSingle());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count() + failedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.count(), 1); // 引擎层异常被 finalizer 收尾为失败运行，Service 任务正常结束
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(!engine.isBusy());
        QCOMPARE(probe.runs.load(), 0); // 注入点在模块执行前

        engine.clearExecutionFaultInjectionForTest();
        QSignalSpy nextSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QTRY_COMPARE_WITH_TIMEOUT(nextSpy.count(), 1, 5000);
        QCOMPARE(probe.runs.load(), 1);
    }
};

QTEST_MAIN(TestFlowExecutionService)
#include "test_flowexecutionservice.moc"
