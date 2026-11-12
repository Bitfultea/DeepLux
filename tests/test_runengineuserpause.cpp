#include <core/engine/EngineTestSupport.h>
#define DEEPLUX_BUILD_TESTS 1
#include "core/base/ModuleBase.h"
#include "core/engine/RunEngine.h"
#include "core/model/Project.h"

#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>
#include <atomic>
#include <thread>

using namespace DeepLux;

namespace {

struct PauseFlow {
    Project project;
    QSemaphore entered; // A 进入执行的确定性屏障
    QSemaphore release; // 测试放行 A 的确定性屏障
    QStringList log;
    std::atomic<int> aCalls{0};
    std::atomic<int> bCalls{0};
    std::atomic<int> cCalls{0};
};

} // namespace

/**
 * 用户暂停协议探针（后台执行设计 §5.3 关键语义 4）：
 * - blocking=true：进入 process 后阻塞，直到测试释放；用于把 pauseForJob 精确落在 A 执行中。
 * - log/counter：证明暂停点之后的下游模块只在 resume 后各执行一次。
 */
class PauseProbeModule : public ModuleBase {
public:
    PauseProbeModule(const QString& name, PauseFlow* flow, bool blocking, std::atomic<int>* counter)
        : m_flow(flow), m_blocking(blocking), m_counter(counter) {
        m_moduleId = QStringLiteral("com.deeplux.test.pause.") + name;
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
    PauseFlow* m_flow = nullptr;
    bool m_blocking = false;
    std::atomic<int>* m_counter = nullptr;
};

namespace {

// 顺序控制图 A->B->C；A 可阻塞、可带断点，B/C 只记录执行。
bool loadPauseFlow(PauseFlow& flow, bool blockFirst, bool firstBreakpoint) {
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
                return new PauseProbeModule(QStringLiteral("A"), &flow, blockFirst, &flow.aCalls);
            }
            if (inst.id == QLatin1String("B")) {
                return new PauseProbeModule(QStringLiteral("B"), &flow, false, &flow.bCalls);
            }
            return new PauseProbeModule(QStringLiteral("C"), &flow, false, &flow.cCalls);
        });
    if (loaded) {
        // 探针模块未注册插件（默认 gui）：后台线程执行必须以 worker 亲和内联执行。
        for (const QString& name : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}) {
            RunEngineTestAccess(RunEngine::instance())
                .setModuleExecutionAffinityForTest(name, ExecutionAffinity::Worker);
        }
    }
    return loaded;
}

} // namespace

class TestRunEngineUserPause : public QObject {
    Q_OBJECT
private slots:
    void init() {
        RunEngine::instance().stop();
        RunEngineTestAccess(RunEngine::instance()).clearFinalizeHookForTest();
        RunEngineTestAccess(RunEngine::instance()).clearExecutionFaultInjectionForTest();
        RunEngineTestAccess(RunEngine::instance()).clearBeforeJobStartHookForTest();
        RunEngine::instance().clearModules();
    }

    // B4 Step 1（计划用例）：A->B->C，A 执行中请求暂停；A 结束后停在 B 前；
    // resume 后 B/C 各执行一次，暂停期间不触发 runFinished。
    void testPauseResumeExecutesEachDownstreamModuleOnce() {
        RunEngine& engine = RunEngine::instance();
        PauseFlow flow;
        QVERIFY(loadPauseFlow(flow, /*blockFirst=*/true, /*firstBreakpoint=*/false));
        QSignalSpy finishedSpy(&engine, &RunEngine::runFinished);
        QSignalSpy pauseSpy(&engine, &RunEngine::pauseHit);

        RunEngine::JobToken token;
        QString reason;
        QVERIFY2(engine.beginExternalJob(&token, &reason), qPrintable(reason));

        std::atomic<bool> started{false};
        std::thread runner([&]() { started.store(engine.runOnceForJob(token, &reason)); });
        QVERIFY(flow.entered.tryAcquire(1, 2000)); // A 已在执行中（确定性屏障）
        QVERIFY(engine.pauseForJob(token));
        flow.release.release(); // 放行 A，让执行循环走到 B 前的模块边界
        runner.join();

        // 先采集事实并在断言前释放 job 租约，避免失败用例污染后续用例。
        const bool paused = engine.state() == RunState::Paused;
        const bool pausedAtBreakpoint = engine.isPausedAtBreakpoint();
        const PauseReason reasonAtPause = engine.pauseReason();
        const int callsAtPauseA = flow.aCalls.load();
        const int callsAtPauseB = flow.bCalls.load();
        const int callsAtPauseC = flow.cCalls.load();
        const int finishedAtPause = finishedSpy.count();
        bool resumed = false;
        if (paused) {
            resumed = engine.resumeForJob(token, &reason);
        }
        const QStringList logAfterResume = flow.log;
        const int callsAfterA = flow.aCalls.load();
        const int callsAfterB = flow.bCalls.load();
        const int callsAfterC = flow.cCalls.load();
        const int finishedAfterResume = finishedSpy.count();
        const RunState finalState = engine.state();
        const PauseReason reasonAfterResume = engine.pauseReason();
        const bool ended = engine.endExternalJob(token);

        QVERIFY(started.load());
        QCOMPARE(paused, true);
        QVERIFY(!pausedAtBreakpoint);
        QCOMPARE(reasonAtPause, PauseReason::User);
        QCOMPARE(callsAtPauseA, 1);
        QCOMPARE(callsAtPauseB, 0);
        QCOMPARE(callsAtPauseC, 0);
        QCOMPARE(finishedAtPause, 0);
        // pauseHit 在后台执行线程发出，经排队投递到主线程；QTRY 仅等待投递，不构造顺序。
        QTRY_COMPARE_WITH_TIMEOUT(pauseSpy.count(), 1, 2000);
        QCOMPARE(pauseSpy.first().at(0).toString(), QStringLiteral("B")); // 停在下一待执行模块 B 前
        QCOMPARE(pauseSpy.first().at(1).value<PauseReason>(), PauseReason::User);
        QVERIFY2(resumed, qPrintable(reason));
        QCOMPARE(callsAfterA, 1); // A 不重复执行
        QCOMPARE(callsAfterB, 1);
        QCOMPARE(callsAfterC, 1);
        QCOMPARE(logAfterResume, QStringList({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}));
        QCOMPARE(finishedAfterResume, 1);
        QCOMPARE(finalState, RunState::Idle);
        QCOMPARE(reasonAfterResume, PauseReason::None);
        QVERIFY(ended);
    }

    // 设计 §5.3：stop() 清除用户暂停请求与暂停上下文，防止旧 resume 重启。
    void testStopWhileUserPausedClearsResumeContext() {
        RunEngine& engine = RunEngine::instance();
        PauseFlow flow;
        QVERIFY(loadPauseFlow(flow, true, false));
        QSignalSpy finishedSpy(&engine, &RunEngine::runFinished);

        RunEngine::JobToken token;
        QString reason;
        QVERIFY(engine.beginExternalJob(&token, &reason));
        std::atomic<bool> started{false};
        std::thread runner([&]() { started.store(engine.runOnceForJob(token, &reason)); });
        QVERIFY(flow.entered.tryAcquire(1, 2000));
        QVERIFY(engine.pauseForJob(token));
        flow.release.release();
        runner.join();
        const bool pausedBeforeStop = engine.state() == RunState::Paused;

        engine.stop();
        const RunState stoppedState = engine.state();
        const bool pausedAtBreakpoint = engine.isPausedAtBreakpoint();
        const PauseReason reasonAfterStop = engine.pauseReason();
        const bool resumeRejected = !engine.resumeForJob(token, &reason);
        const QString resumeReason = reason;
        const int finishedCount = finishedSpy.count();
        const bool ended = engine.endExternalJob(token);
        const bool busyAfterEnd = engine.isBusy();

        QVERIFY(pausedBeforeStop);
        QCOMPARE(stoppedState, RunState::Stopped);
        QVERIFY(!pausedAtBreakpoint);
        QCOMPARE(reasonAfterStop, PauseReason::None);
        QVERIFY(resumeRejected);
        QCOMPARE(resumeReason, QStringLiteral("external_job_cancelled"));
        QCOMPARE(finishedCount, 0);
        QVERIFY(ended);
        QVERIFY(!busyAfterEnd);
    }

    // t3 审查 O4：Paused 收尾窗口内被 stop 抢先时，暂停被放弃，不得再发 breakpointHit。
    void testStopInPauseFinalizeWindowSkipsBreakpointHit() {
        RunEngine& engine = RunEngine::instance();
        PauseFlow flow;
        QVERIFY(loadPauseFlow(flow, /*blockFirst=*/false, /*firstBreakpoint=*/true));
        QSignalSpy bpSpy(&engine, &RunEngine::breakpointHit);
        QSignalSpy finishedSpy(&engine, &RunEngine::runFinished);

        QSemaphore hookReached;
        RunEngineTestAccess(engine).setFinalizeHookForTest([&]() {
            hookReached.release();
            RunEngine::instance().stop();
        });
        engine.runOnce();
        QVERIFY(hookReached.tryAcquire(1, 2000));

        QCOMPARE(bpSpy.count(), 0);
        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(engine.state(), RunState::Stopped);
        QVERIFY(!engine.isPausedAtBreakpoint());
        QCOMPARE(flow.aCalls.load(), 0); // 断点暂停发生在 A 执行前
        RunEngineTestAccess(engine).clearFinalizeHookForTest();
    }

    // t3 审查 O6：stepOnce 的 stop 判定与单步态重置在同一临界区——收尾窗口内 stop 获胜后，
    // 下一次单步必须从流程起点开始（而不是继续执行暂停后的下一模块）。
    void testStopInStepFinalizeWindowResetsStepPosition() {
        RunEngine& engine = RunEngine::instance();
        QStringList log;
        PauseFlow flow;
        PauseProbeModule first(QStringLiteral("First"), &flow, false, nullptr);
        PauseProbeModule second(QStringLiteral("Second"), &flow, false, nullptr);
        QVERIFY(first.initialize());
        QVERIFY(second.initialize());
        engine.addModule(&first);
        engine.addModule(&second);

        QSemaphore hookReached;
        RunEngineTestAccess(engine).setFinalizeHookForTest([&]() {
            hookReached.release();
            RunEngine::instance().stop();
        });
        QVERIFY(engine.stepOnce());
        QVERIFY(hookReached.tryAcquire(1, 2000));
        QCOMPARE(engine.state(), RunState::Stopped);
        QCOMPARE(flow.log, QStringList({QStringLiteral("First")}));
        RunEngineTestAccess(engine).clearFinalizeHookForTest();

        // 收尾窗口 stop 已重置单步态：下一步从流程起点重新开始。
        QVERIFY(engine.stepOnce());
        QCOMPARE(flow.log, QStringList({QStringLiteral("First"), QStringLiteral("First")}));
    }
};

QTEST_MAIN(TestRunEngineUserPause)
#include "test_runengineuserpause.moc"
