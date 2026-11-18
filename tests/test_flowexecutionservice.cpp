#include <core/engine/EngineTestSupport.h>
#define DEEPLUX_BUILD_TESTS 1
#include "core/base/ModuleBase.h"
#include "core/engine/FlowExecutionService.h"
#include "core/engine/RunEngine.h"
#include "core/model/Project.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtGlobal>
#include <QtTest/QtTest>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
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
            RunEngineTestAccess(RunEngine::instance())
                .setModuleExecutionAffinityForTest(name, ExecutionAffinity::Worker);
        }
    }
    return loaded;
}

/// T2.1 NF 失败路径收尾：放行退出屏障、回收独立控制线程、清除钩子、恢复诊断阈值。
/// 所有等待都有界，保证任何 QVERIFY 提前返回时 worker 与控制线程都能安全退出，
/// 且钩子（捕获测试栈变量）在栈销毁前一定被清空。
struct ShutdownBarrierGuard {
    FlowExecutionService* service = nullptr;
    QSemaphore* release = nullptr;
    std::atomic<bool>* released = nullptr;
    std::thread* control = nullptr;
    bool controlStarted = false;
    int originalDeadlineMs = 10000;

    ~ShutdownBarrierGuard() {
        if (!service) {
            return;
        }
        // 1) 无论如何先放行，避免 worker 卡在退出屏障上导致 shutdown 无法返回。
        if (released && !released->exchange(true) && release) {
            release->release();
        }
        // 2) 回收独立控制线程（其内部 entered / 诊断轮询等待均有界，最长约 6s）。
        if (controlStarted && control && control->joinable()) {
            control->join();
        }
        // 3) 清除钩子并恢复默认阈值；必须在捕获的栈变量销毁前执行。
        FlowExecutionServiceTestAccess(*service).clearWorkerExitBarrierForTest();
        FlowExecutionServiceTestAccess(*service).setShutdownDeadlineForTest(originalDeadlineMs);
    }
};

/// Service 行为契约用例的统一安全收尾（T2.3 / T2.4 共用；非 NF、无生产修复提交）：
/// DirectConnection 槽与启动握手钩子捕获了本用例栈上的信号量/原子变量，engine 也持有
/// probe 栈对象指针。任何 QVERIFY/QCOMPARE 提前返回时，必须保证 worker 已真正退出、
/// 不再引用这些栈变量。声明顺序要求：本 guard 必须在被捕获变量之后声明（析构逆序 => 最先析构）。
///
/// P1 整改（2026-11-06 评审复现）：旧实现只等 state()==Idle，但「启动窗口」内 job 已出队、
/// state 仍为 Idle 而 isActive() 为 true（worker 在途），guard 会立即结束收尾，导致被捕获的
/// 栈变量在 worker 仍活跃时被销毁。现以「实际任务结束」(!isActive()) 为等待条件，并以
/// shutdown() 成功 join（返回 true）作为 worker 真正退出的依据；仍活跃或未能 join 时
/// 不得静默返回。shutdown() 的 joinWorkerThread() 采用 wait + 必要时无限 wait，
/// 不强杀、不 detach，返回 true 即代表线程已退出并 delete。
///
/// T2.4 复用：barrierRelease 在停止前无条件放行「进入/放行」屏障，clearHooks 清除捕获栈
/// 变量的测试钩子（runJob 已把钩子复制到局部副本，clear 不影响在途调用），starter 由 guard
/// 统一 join，保证失败路径不会因 std::thread 析构而 std::terminate。
struct ServiceContractCleanupGuard {
    FlowExecutionService* service = nullptr;
    QMetaObject::Connection connection;
    std::function<void()> barrierRelease;
    std::function<void()> clearHooks;
    std::thread* starter = nullptr;
    bool armed = false;

    /// 统一收尾：正常路径显式调用；析构作为失败路径兜底。
    ///
    /// **核心契约（2026-11-09 评审 P1 二次修正）：本函数在「实际任务已结束且
    /// shutdown() 已成功 join worker」之前绝不返回。** 返回即代表被捕获的栈对象
    /// （probe / 信号量 / 原子变量 / DirectConnection 槽）已不再被 worker 引用，
    /// 可以安全销毁。旧实现在诊断超时后返回 false，析构兜底也只 qFail() 后返回，
    /// 两者都会让栈继续展开而 worker 仍在途 —— 这正是本次 P1。
    ///
    /// 诊断阶段有界，仅用于产生告警；超时后继续等待实际退出（由外层 watchdog：
    /// CTest TIMEOUT / `timeout -k` 限时），并在硬上限到达时 qFatal() 直接
    /// 终止测试进程 —— 绝不带着活跃 worker 返回并继续栈展开。
    bool finish() {
        if (!service || !armed) {
            return true;
        }
        // 注意：armed 只在成功 join 之后清除。在此之前任何 QVERIFY/QCOMPARE
        // 提前 return 都会让析构兜底再次进入本函数，而不是跳过收尾。
        const auto pump = [] {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        };
        // 1) 先断连：后续 jobStateChanged(Running/Stopping/Idle) 不得再进入槽。
        QObject::disconnect(connection);
        // 2) 无条件放行「进入/放行」屏障并清除捕获栈变量的测试钩子：失败路径下 worker
        //    可能仍卡在钩子里，不清钩子会让下一次 job 引用已销毁的栈变量。
        if (barrierRelease) {
            barrierRelease();
        }
        if (clearHooks) {
            clearHooks();
        }
        // 3) 回收启动线程：其内部等待均有界，且屏障已放行，不会悬挂。
        if (starter && starter->joinable()) {
            starter->join();
        }
        // 4) 请求停止（幂等；job 已结束时直接返回）。
        service->stop();

        QElapsedTimer timer;
        timer.start();
        // 5) 有界诊断等待「实际任务结束」：isActive() 覆盖「job 已出队但 state 仍 Idle」
        //    的启动窗口，不能只以 state()==Idle 为依据。
        while (service->isActive() && timer.elapsed() < 10000) {
            pump();
        }
        // 6) 有界诊断 join：shutdown() 成功 join（返回 true）是 worker 真正退出的依据，
        //    它在任务仍活跃时只请求停止并返回 false，故需重试。
        bool joined = false;
        while (timer.elapsed() < 15000) {
            if (service->shutdown()) {
                joined = true;
                break;
            }
            service->stop();
            pump();
        }
        // 7) 诊断超时仍未 join：**不得返回**。继续等待实际退出，由外层 watchdog 限时。
        //    这是 P1 的关键修正点：旧实现在此 return false，析构只 qFail 后返回，
        //    两条路径都继续栈展开，worker 仍活跃时栈对象即被销毁。
        if (!joined || service->isActive()) {
            qWarning("ServiceContractCleanupGuard: bounded diagnostics expired after %lld ms "
                     "(joined=%d, isActive=%d); continuing to wait for the real worker exit. "
                     "The outer watchdog bounds this wait.",
                     static_cast<long long>(timer.elapsed()), joined ? 1 : 0, service->isActive() ? 1 : 0);
            for (;;) {
                service->stop();
                if (service->shutdown() && !service->isActive()) {
                    joined = true;
                    break;
                }
                pump();
                if (timer.elapsed() >= 60000) {
                    // 进程级终止：不返回、不栈展开，绝不销毁仍被引用的栈对象。
                    qCritical("ServiceContractCleanupGuard: worker did not exit within %lld ms; "
                              "terminating the test process instead of unwinding the stack while "
                              "the worker still references captured locals",
                              static_cast<long long>(timer.elapsed()));
                    std::abort(); // 不依赖 qFatal 是否被 Qt Test 消息处理器拦截
                }
            }
        }
        // 8) 仅在「实际任务结束 + shutdown() 成功 join」之后清除 armed。
        armed = false;
        return true;
    }

    ~ServiceContractCleanupGuard() {
        if (!armed) {
            return;
        }
        // finish() 在成功 join 之前不会返回；若触发硬上限则直接终止进程。
        // 因此这里不存在「带着活跃 worker 从析构返回」的路径。
        (void) finish();
    }
};

/// T2.5：收尾段故障注入的公共验证（普通 C++ 回调，不经过任何 Qt 信号槽边界）。
/// 钩子在收尾入口、endExternalJob() 之前执行：进入时必须仍持有引擎租约、Service 仍为
/// Running、显示 jobId 尚未清零；抛出后必须只上报一次 jobFailed，且租约/显示 jobId/
/// 活跃状态全部复位，清除钩子后同一 Service 能再次完成任务。
void verifyJobTeardownFaultRecovery(bool nonStandardException) {
    RunEngine& engine = RunEngine::instance();
    FlowExecutionService& service = FlowExecutionService::instance();
    ServiceProbeModule probe;
    QVERIFY(probe.initialize());
    engine.addModule(&probe);
    RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                  ExecutionAffinity::Worker);

    QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
    QSignalSpy failedSpy(&service, &FlowExecutionService::jobFailed);
    QVERIFY2(failedSpy.isValid(), "jobFailed must be observable");

    std::atomic_int hookRuns{0};
    std::atomic_bool leaseHeldAtHook{false};
    std::atomic_bool runningAtHook{false};
    std::atomic_bool displayIdSetAtHook{false};

    FlowExecutionServiceTestAccess(service).setJobTeardownHookForTest([&] {
        ++hookRuns;
        leaseHeldAtHook.store(engine.isBusy(), std::memory_order_release);
        runningAtHook.store(service.state() == FlowExecutionService::State::Running, std::memory_order_release);
        displayIdSetAtHook.store(engine.displayJobId() != 0, std::memory_order_release);
        if (nonStandardException) {
            throw 42; // 非 std::exception：覆盖 catch(...) 分支
        }
        throw std::runtime_error("injected teardown callback fault");
    });

    const auto clearTeardownHook = [&] { FlowExecutionServiceTestAccess(service).clearJobTeardownHookForTest(); };
    ServiceContractCleanupGuard guard{&service, {}, nullptr, clearTeardownHook, nullptr, true};

    QVERIFY2(service.startSingle(), "the job must start so the teardown hook runs with a lease");
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);

    // 终态只允许一个；资源必须真正释放（state==Idle 不能替代）。
    QCOMPARE(finishedSpy.count(), 0);
    QCOMPARE(hookRuns.load(), 1); // 每个 job 只调用一次，恢复路径不得二次触发
    QVERIFY2(leaseHeldAtHook.load(), "the teardown hook must run while the engine lease is held");
    QVERIFY2(runningAtHook.load(), "the teardown hook must run before the state is reset");
    QVERIFY2(displayIdSetAtHook.load(), "the teardown hook must run before the display jobId is cleared");
    QCOMPARE(service.state(), FlowExecutionService::State::Idle);
    QCOMPARE(service.currentJob(), FlowExecutionService::JobType::None);
    QVERIFY2(!service.isActive(), "the service must not stay active after teardown recovery");
    QVERIFY2(!engine.isBusy(), "the engine lease must be released after teardown recovery");
    QCOMPARE(engine.displayJobId(), quint64(0));
    const QString error = failedSpy.first().at(1).toString();
    QVERIFY2(error.contains(QStringLiteral("service_teardown_exception")), qPrintable(error));

    // 清除钩子后同一 Service 必须能再次完成一个任务。
    FlowExecutionServiceTestAccess(service).clearJobTeardownHookForTest();
    const int runsAfterFault = probe.runs.load();
    QSignalSpy nextFinished(&service, &FlowExecutionService::jobFinished);
    QVERIFY2(service.startSingle(), "the service must remain usable after a teardown fault");
    QTRY_COMPARE_WITH_TIMEOUT(nextFinished.count(), 1, 5000);
    QCOMPARE(probe.runs.load(), runsAfterFault + 1);
    QCOMPARE(service.state(), FlowExecutionService::State::Idle);

    guard.finish();
}

} // namespace

class TestFlowExecutionService : public QObject {
    Q_OBJECT
private slots:
    void init() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        engine.stop();
        RunEngineTestAccess(engine).clearFinalizeHookForTest();
        RunEngineTestAccess(engine).clearExecutionFaultInjectionForTest();
        service.stop();
        FlowExecutionServiceTestAccess(service).clearAfterStartSettledHookForTest();
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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("BlockingProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("First"),
                                                                      ExecutionAffinity::Worker);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("Second"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        QSemaphore hookEntered;
        QSemaphore releaseHook;
        FlowExecutionServiceTestAccess(service).setAfterStartSettledHookForTest([&]() {
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
        FlowExecutionServiceTestAccess(service).clearAfterStartSettledHookForTest();
    }

    // B5.2：Cycle 至少两轮；轮间 job 租约保持 isBusy，维护被拒绝。
    void testCycleRunsAtLeastTwoRoundsAndKeepsEngineBusy() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("BlockingProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("BlockingProbe"),
                                                                      ExecutionAffinity::Worker);

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

    // T2.1（NF，确定性整改）：worker 退出等待必须在诊断阈值处告警并继续 join（禁止 detach），
    // 且不能挂死。旧用例注入 0ms 后 wait(0) 可能成功（worker 正常退出），导致诊断不命中、
    // 用例 17/30 失败，不具确定性。本版以「退出屏障」保证 worker 在诊断发生前不能退出，
    // 再由独立控制线程放行；屏障等待不持有服务互斥锁。
    void testShutdownReportsDeadlineAndJoinsWorkerWithoutDetach() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe(QStringLiteral("DeadlineProbe"));
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("DeadlineProbe"),
                                                                      ExecutionAffinity::Worker);

        // 先跑一次，确保常驻 worker 已创建并回到空闲等待。
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Idle, 5000);

        const int baselineHits = FlowExecutionServiceTestAccess(service).shutdownDeadlineHitCountForTest();
        const int originalDeadlineMs = 10000;
        FlowExecutionServiceTestAccess(service).setShutdownDeadlineForTest(0);

        QSemaphore entered; // worker 已到达退出屏障
        QSemaphore release; // 独立控制线程放行
        std::atomic<bool> released{false};
        std::atomic<int> workerRunFinished{0};     // 屏障返回、worker 即将真正退出
        std::atomic<bool> shutdownReturned{false}; // 主线程在 shutdown() 返回后置位
        std::atomic<int> returnedBeforeRelease{-1};
        std::atomic<int> workerFinishedBeforeRelease{-1};
        std::atomic<int> workerAtBarrier{-1};

        FlowExecutionServiceTestAccess(service).setWorkerExitBarrierForTest([&]() {
            // 该钩子在 WorkerThread::run() 内、workerLoop() 返回之后执行，且不持有 m_mutex。
            entered.release();
            // 有界等待：任何失败路径下 worker 都能自行退出，绝不 hang。
            release.tryAcquire(1, 5000);
            workerRunFinished.store(1, std::memory_order_release);
        });

        std::thread control;
        ShutdownBarrierGuard guard{&service, &release, &released, &control, false, originalDeadlineMs};

        control = std::thread([&]() {
            // 1) 有界等待 worker 到达退出屏障（证明屏障确实生效）。
            const bool atBarrier = entered.tryAcquire(1, 3000);
            // 2) 有界等待本轮诊断计数增量；缺失诊断变体下不会命中，但仍有界放行。
            QElapsedTimer poll;
            poll.start();
            while (FlowExecutionServiceTestAccess(service).shutdownDeadlineHitCountForTest() <= baselineHits &&
                   poll.elapsed() < 2000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // 3) 放行前的「提前返回」观测窗口（与 T2.2 已批准的有界窗口同构）：
            //    正确实现下主线程仍阻塞在 shutdown() 的 join 中，shutdownReturned 必须
            //    保持 false；提前返回变体会在此 500ms 有界窗口内置位。窗口与放行都在
            //    本独立控制线程内完成，不依赖任何无界等待。
            QElapsedTimer earlyWindow;
            earlyWindow.start();
            while (!shutdownReturned.load(std::memory_order_acquire) && earlyWindow.elapsed() < 500) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // 4) 放行前记录状态，再由本独立控制线程放行退出屏障。
            returnedBeforeRelease.store(shutdownReturned.load(std::memory_order_acquire) ? 1 : 0,
                                        std::memory_order_release);
            workerFinishedBeforeRelease.store(workerRunFinished.load(std::memory_order_acquire),
                                              std::memory_order_release);
            workerAtBarrier.store(atBarrier ? 1 : 0, std::memory_order_release);
            if (!released.exchange(true)) {
                release.release();
            }
        });
        guard.controlStarted = true;

        QElapsedTimer timer;
        timer.start();
        const bool shutdownResult = service.shutdown();
        shutdownReturned.store(true, std::memory_order_release);
        const qint64 elapsedMs = timer.elapsed();

        // 收尾：控制线程内部等待均有界，join 不会挂死；随后清钩子、恢复阈值。
        control.join();
        guard.controlStarted = false;
        FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();
        FlowExecutionServiceTestAccess(service).setShutdownDeadlineForTest(originalDeadlineMs);

        // —— 断言（顺序即 NF 判据顺序）——
        QVERIFY2(shutdownResult, "idle shutdown must join the worker and return true");
        QVERIFY2(elapsedMs < 5000, "shutdown must not hang waiting for the worker");
        QCOMPARE(workerAtBarrier.load(std::memory_order_acquire), 1);
        QVERIFY2(FlowExecutionServiceTestAccess(service).shutdownDeadlineHitCountForTest() >= baselineHits + 1,
                 "本轮 shutdown 必须记录有界诊断（计数增量 >= 1）");
        QCOMPARE(returnedBeforeRelease.load(std::memory_order_acquire), 0);
        QCOMPARE(workerFinishedBeforeRelease.load(std::memory_order_acquire), 0);
        QVERIFY2(workerRunFinished.load(std::memory_order_acquire) == 1, "worker 必须真正退出后 shutdown 才返回");

        // Join（而非 detach）后可重建线程继续工作。
        QSignalSpy nextSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("BlockingProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

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
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        RunEngineTestAccess(engine).setExecutionFaultInjectionForTest(
            []() { throw std::runtime_error("injected service job fault"); });
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QSignalSpy failedSpy(&service, &FlowExecutionService::jobFailed);
        QVERIFY(service.startSingle());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count() + failedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.count(), 1); // 引擎层异常被 finalizer 收尾为失败运行，Service 任务正常结束
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(!engine.isBusy());
        QCOMPARE(probe.runs.load(), 0); // 注入点在模块执行前

        RunEngineTestAccess(engine).clearExecutionFaultInjectionForTest();
        QSignalSpy nextSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QTRY_COMPARE_WITH_TIMEOUT(nextSpy.count(), 1, 5000);
        QCOMPARE(probe.runs.load(), 1);
    }

    // ==== T2.5 正式红灯：启动段自身 C++ 异常必须收敛为 jobFailed（2026-11-10 评审裁定）====
    // 注入手法：复用既有 setStartHandshakeHookForTest()（std::function<void()>，由 runJob()
    // 在启动协议内直接调用），**不经过任何 Qt 信号槽边界**——槽内抛异常属 Qt 未定义行为，
    // 不得用作缺陷证据。基线（修复前）：异常逃出 workerLoop()/QThread::run() → std::terminate()，
    // 进程以 rc=134 abort，startSingle() 不返回。
    void testExceptionInJobStartupEmitsJobFailedNotTerminate() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QSignalSpy failedSpy(&service, &FlowExecutionService::jobFailed);
        QVERIFY2(failedSpy.isValid(), "jobFailed must be observable");

        FlowExecutionServiceTestAccess(service).setStartHandshakeHookForTest(
            [] { throw std::runtime_error("injected startup callback fault"); });

        // 失败路径收尾：清除捕获栈变量的钩子并 join worker。必须最后声明。
        ServiceContractCleanupGuard guard{
            &service, {},  nullptr, [&] { FlowExecutionServiceTestAccess(service).clearStartHandshakeHookForTest(); },
            nullptr,  true};

        // 崩溃基线即在此处 abort；修复后必须返回 false（启动未成立）。
        QVERIFY2(!service.startSingle(), "a startup fault must not report a successful start");

        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.count(), 0); // 一次 job 只允许一个终态信号
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QCOMPARE(service.currentJob(), FlowExecutionService::JobType::None);
        QVERIFY2(!engine.isBusy(), "a startup fault must not leave the engine leased");
        QCOMPARE(probe.runs.load(), 0); // 启动段失败：模块不得执行
        const QString error = failedSpy.first().at(1).toString();
        QVERIFY2(error.contains(QStringLiteral("service_startup_exception")), qPrintable(error));

        // 恢复：同一个 Service 清除钩子后必须能再次启动并完成。
        FlowExecutionServiceTestAccess(service).clearStartHandshakeHookForTest();
        QSignalSpy nextFinished(&service, &FlowExecutionService::jobFinished);
        QVERIFY2(service.startSingle(), "the service must remain usable after a startup fault");
        QTRY_COMPARE_WITH_TIMEOUT(nextFinished.count(), 1, 5000);
        QCOMPARE(probe.runs.load(), 1);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);

        guard.finish();
    }

    // ==== T2.5 收尾段：std::exception / 非 std::exception 都必须完成资源释放 ====
    // 注入手法：普通 C++ 回调 setJobTeardownHookForTest()，锁内快照、锁外调用，
    // 每个 job 只在收尾入口执行一次；不经过任何 Qt 信号槽边界。
    void testExceptionInJobTeardownEmitsJobFailedNotTerminate() {
        verifyJobTeardownFaultRecovery(false);
    }

    // catch(...) 分支（非 std::exception）此前不重试释放租约：state 回落 Idle 但引擎
    // 仍 busy，下一任务被永久拒绝。本用例把「真实恢复」而不是「状态为 Idle」作为判据。
    void testNonStandardExceptionInJobTeardownEmitsJobFailedNotTerminate() {
        verifyJobTeardownFaultRecovery(true);
    }

    // ==== T2.5 P1（2026-11-10 评审裁定）：线程入口兜底后必须能再次执行任务 ====
    //
    // 缺陷前提：WorkerThread::run() 捕获 workerLoop() 自身异常后调用
    // handleWorkerLoopFault()，但该函数只把 state 复位为 Idle，既未清理残留的
    // m_pendingJob（enqueue() 的拒绝条件之一是 m_pendingJob != None），也未回收
    // 已经退出的线程；而 ensureThreadLocked() 仅判断 m_thread 是否非空，不会重建。
    // 结果是故障后调用者必须手动 shutdown() 才能恢复——生产代码里没有任何
    // FlowExecutionService::shutdown() 调用点，等于该次故障后服务永久瘫痪。
    //
    // 注入手法（普通 C++ 回调，不经过任何 Qt 信号槽边界）：setWorkerLoopFaultHookForTest()
    // 在 workerLoop() 每轮循环入口、命令已出队而 runJob() 尚未进入处执行并抛出，
    // 等价于 workerLoop() 自身（取队列/加锁）抛出 C++ 异常。
    //
    // 判据：故障后无需任何手动 shutdown()，直接再次 startSingle() 必须成功并完成任务；
    // 只复位 state 不算恢复——必须同时满足引擎不忙、服务非活跃。
    void testServiceRestartsWithoutManualShutdownAfterWorkerFault() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QSignalSpy failedSpy(&service, &FlowExecutionService::jobFailed);
        QVERIFY2(failedSpy.isValid(), "jobFailed must be observable");

        std::atomic_int hookRuns{0};
        FlowExecutionServiceTestAccess(service).setWorkerLoopFaultHookForTest([&] {
            if (hookRuns.fetch_add(1) == 0) {
                // 只在首轮注入：故障后的重试必须能真正执行任务。
                throw std::runtime_error("injected worker-loop callback fault");
            }
        });

        // 失败路径收尾：清除捕获栈变量的钩子并 join worker。必须最后声明。
        ServiceContractCleanupGuard guard{
            &service, {},  nullptr, [&] { FlowExecutionServiceTestAccess(service).clearWorkerLoopFaultHookForTest(); },
            nullptr,  true};

        // 故障当次：startSingle() 的握手必须被放行并返回 false（启动未成立）。
        QVERIFY2(!service.startSingle(), "a worker-loop fault must not report a successful start");
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.count(), 0); // 一次 job 只允许一个终态信号
        QCOMPARE(hookRuns.load(), 1);

        const QString error = failedSpy.first().at(1).toString();
        QVERIFY2(error.contains(QStringLiteral("service_worker_exception")), qPrintable(error));

        // 真实恢复：state 复位不能替代资源释放与命令清理。
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QCOMPARE(service.currentJob(), FlowExecutionService::JobType::None);
        QVERIFY2(!service.isActive(), "a worker-loop fault must not leave a pending command or an active job behind");
        QVERIFY2(!engine.isBusy(), "a worker-loop fault must not leave the engine leased");
        QCOMPARE(engine.displayJobId(), quint64(0));

        // 核心判据：**不调用 shutdown()**，直接再次执行任务。
        QSignalSpy nextFinished(&service, &FlowExecutionService::jobFinished);
        QVERIFY2(service.startSingle(),
                 "the service must restart without a manual shutdown() after a worker-loop fault");
        QTRY_COMPARE_WITH_TIMEOUT(nextFinished.count(), 1, 5000);
        QCOMPARE(probe.runs.load(), 1);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY2(!service.isActive(), "the service must return to idle after the retried job");
        QVERIFY2(!engine.isBusy(), "the retried job must release the engine lease");

        guard.finish();
    }

    // ==== T2.5 P1 第二轮回（2026-11-10 复核裁定）：故障通知后立即重试的窗口 ====
    //
    // 缺陷前提：jobFailed 是在 worker 线程的 run() 内部（handleWorkerLoopFault）发出的，
    // 那一刻 run() 尚未返回、QThread::isFinished() 仍为 false。上一轮修复（9bd3aad）只用
    // 一次 isFinished() 快照判断是否回收线程，因此「收到失败通知后立即重试」可能跳过回收，
    // 把命令写进一个再也不会消费队列的旧线程，最终只能等到启动握手超时后撤回失败。
    // 评审独立复现：标准异常 19/200、非标准 3/200、标准异常加诊断复跑 28/200。
    //
    // 窗口如何确定性撑开（不靠碰运气）：WorkerThread::run() 在 handleWorkerLoopFault()
    // 发出 jobFailed **之后**、run() 返回**之前**调用退出屏障钩子。本用例用该屏障把
    // worker 停在窗口正中——jobFailed 已发出而 isFinished() 仍为 false——主线程此时重试，
    // 100% 落在评审描述的窗口内。
    //
    // 为什么不由 jobFailed 的直连槽发起重试：直连槽执行在正在退出的 worker 线程自己身上，
    // 槽内同步重试会让 enqueue() 去 join 自己（QThread::wait 报 "Thread tried to wait on
    // itself"），最终 "Destroyed while thread is still running" 直接 abort。这是试跑来
    // 的真实结论，已记录在案；故槽内只做状态观测，重试由主线程发起。
    //
    // 纪律：直连槽在本用例中只作**状态观测**，绝不抛异常（槽内抛异常属 Qt 未定义行为，
    // 不得作为缺陷证据）。全部判据落在主线程 startSingle() 的返回值与 probe.runs() 上。
    //
    // **2026-11-11 复核 P1：验收口径修正（本条判据整体重写）**
    //
    // 上一版判据是「窗口内重试必须成功（retryFailed == 0）」，而它是靠 enqueue() 里
    // 无时限的 joinWorkerThread() 达成的：那段等待发生在启动计时之前，旧线程的退出
    // 耗时被直接转嫁给 GUI 线程。评审独立观测：退出阶段延迟 500ms → startSingle()
    // 阻塞 500ms，超出既有 100ms 预算。因此「窗口内重试必成功」是错误的验收口径。
    //
    // 现判据（①~⑤ 见上方注释；此处逐条落实）：
    //   ① 单次 startSingle() 阻塞 ≤ StartHandshakeTimeoutMs（回收+握手共用一份预算）；
    //   ②③④ 窗口内允许明确拒绝，但拒绝后必须 Idle、无 pending、且事后绝不执行；
    //   ⑤ 旧线程真正退出后再次 startSingle() 必须成功，无需手动 shutdown()。
    void testRetryDuringWorkerRetirementIsBoundedAndNotDeferred() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        constexpr int kRounds = 10;
        // 判据①的调度误差许可：计时精度、线程调度与 processEvents 抖动，不能把预算当硬实时。
        // 60ms 远小于「退出延迟 500ms」的量级，仍能可靠区分「有界」与「跟着退出延迟走」。
        constexpr int kSchedulingSlackMs = 60;

        std::atomic_int armed{0};             // 本轮是否已注入待触发的故障
        std::atomic_int notified{0};          // worker 已发出 jobFailed 的次数
        std::atomic_int barrierEntries{0};    // 退出屏障进入次数（证明窗口已被撑开）
        std::atomic_int windowRetries{0};     // 在窗口内发起重试的次数
        std::atomic_int windowRetryOk{0};     // 窗口内重试被接受次数
        std::atomic_int windowRetryFailed{0}; // 窗口内重试被**明确拒绝**次数（修正后属允许行为）
        std::atomic_int retryNotExecuted{0};  // 被接受却未真正执行任务的次数
        std::atomic_int rejectedLeftResidue{0}; // 被拒绝却留下 pending/非 Idle 的次数（缺陷）
        std::atomic_int rejectedButExecuted{0}; // 被拒绝却事后执行的次数（迟到执行，缺陷）
        std::atomic_int restartOk{0};           // 旧线程退出后再次启动成功次数
        std::atomic_int restartNotExecuted{0};  // 再次启动被接受却未执行次数（缺陷）
        int maxRetryBlockMs = 0;                // 窗口内单次 startSingle() 的最大阻塞时长

        QSemaphore releaseBarrier; // 主线程放行退出屏障
        QSemaphore barrierReached; // worker 已停在退出屏障上
        std::atomic_bool holdBarrier{false};
        std::atomic_bool failureNotified{false};

        // 故障注入：每轮只在「已布防」时抛出一次，抛出后自动解除布防。
        FlowExecutionServiceTestAccess(service).setWorkerLoopFaultHookForTest([&] {
            if (armed.load(std::memory_order_acquire) != 0 && armed.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                throw std::runtime_error("injected worker-loop fault");
            }
        });

        // 退出屏障：位于 handleWorkerLoopFault() 发出 jobFailed **之后**、run() 返回
        // **之前**——正是本轮缺陷的窗口本体。主线程在这里稳定地拿到「通知已发出、
        // 旧线程尚未结束」的状态，不必靠碰运气去撞短窗口。
        FlowExecutionServiceTestAccess(service).setWorkerExitBarrierForTest([&] {
            if (!holdBarrier.load(std::memory_order_acquire)) {
                return;
            }
            ++barrierEntries;
            barrierReached.release();
            // 有界等待：任何失败路径下 worker 都能自行退出，绝不 hang。
            releaseBarrier.tryAcquire(1, 5000);
        });

        // 通知监听：只作**状态观测**，不在槽内做任何阻塞动作（槽内 join 自己属未定义
        // 行为，见下）。判据全部落在主线程的 startSingle() 返回值与 probe.runs() 上。
        QMetaObject::Connection failConn = QObject::connect(
            &service, &FlowExecutionService::jobFailed, &service,
            [&](FlowExecutionService::JobType, const QString&) {
                ++notified;
                failureNotified.store(true, std::memory_order_release);
            },
            Qt::DirectConnection);

        // 失败路径收尾：放行屏障、清钩子并 join worker。必须最后声明。
        // 参数顺序：service, connection, barrierRelease, clearHooks, starter, armed。
        ServiceContractCleanupGuard guard{&service,
                                          failConn,
                                          [&] {
                                              holdBarrier.store(false, std::memory_order_release);
                                              releaseBarrier.release();
                                          },
                                          [&] {
                                              FlowExecutionServiceTestAccess(service).clearWorkerLoopFaultHookForTest();
                                              FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();
                                          },
                                          nullptr,
                                          true};

        for (int round = 0; round < kRounds; ++round) {
            QElapsedTimer idle;
            idle.start();
            while (service.isActive() && idle.elapsed() < 1500) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
            if (service.isActive()) {
                break; // 收尾由 guard 负责
            }

            const int runsBefore = probe.runs.load();

            failureNotified.store(false, std::memory_order_release);
            holdBarrier.store(true, std::memory_order_release);
            armed.store(1, std::memory_order_release);

            // 首启必然被注入打断：启动握手被放行并返回 false。
            const bool firstStart = service.startSingle();
            // worker 停在退出屏障上（jobFailed 已发出、run() 尚未返回）。
            const bool atWindow = barrierReached.tryAcquire(1, 3000);
            if (firstStart || !atWindow) {
                holdBarrier.store(false, std::memory_order_release);
                releaseBarrier.release();
                continue; // 本轮不计入判别样本
            }

            // —— 判据①②：窗口内重试。此刻旧线程尚未结束，必须**有界**返回 ——
            // 无论接受或拒绝，阻塞都不得超过启动预算（回收与握手共用一份）。
            bool inWindowOk = false;
            int inWindowElapsed = -1;
            if (failureNotified.load(std::memory_order_acquire)) {
                QElapsedTimer retryTimer;
                retryTimer.start();
                inWindowOk = service.startSingle();
                inWindowElapsed = static_cast<int>(retryTimer.elapsed());
                ++windowRetries;
                if (inWindowOk) {
                    ++windowRetryOk;
                } else {
                    ++windowRetryFailed;
                }
                maxRetryBlockMs = qMax(maxRetryBlockMs, inWindowElapsed);
            }

            // —— 判据③④：被拒绝时不得入队、不得迟到执行 ——
            const bool rejected = (inWindowElapsed >= 0 && !inWindowOk);
            if (rejected) {
                // 拒绝必须留下干净的 Idle：没有 pending、没有 current job。
                const bool leftPending = (service.state() != FlowExecutionService::State::Idle) || service.isActive();
                if (leftPending) {
                    ++rejectedLeftResidue;
                }
            }

            // 放行屏障，让旧线程真正退出。
            holdBarrier.store(false, std::memory_order_release);
            releaseBarrier.release();

            // 等稳定：被接受的重试应跑完；被拒绝的必须**什么都不发生**。
            QElapsedTimer settle;
            settle.start();
            while (settle.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                if (!service.isActive()) {
                    break;
                }
                QThread::msleep(1);
            }
            releaseBarrier.tryAcquire(1, 2000); // 排水：防止多放的令牌串轮

            if (inWindowOk) {
                if (probe.runs.load() <= runsBefore) {
                    ++retryNotExecuted; // 接受却空转 = 命令进黑洞
                }
            } else if (probe.runs.load() > runsBefore) {
                ++rejectedButExecuted; // 拒绝却执行 = 迟到执行，最严重
            }

            // —— 判据⑤：旧线程已退出，再次启动必须成功且无需手动 shutdown() ——
            const int runsBeforeRestart = probe.runs.load();
            const bool restarted = service.startSingle();
            if (restarted) {
                ++restartOk;
            }
            QElapsedTimer restartSettle;
            restartSettle.start();
            while (restartSettle.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                if (!service.isActive()) {
                    break;
                }
                QThread::msleep(1);
            }
            if (restarted && probe.runs.load() <= runsBeforeRestart) {
                ++restartNotExecuted;
            }
        }

        QObject::disconnect(failConn);
        FlowExecutionServiceTestAccess(service).clearWorkerLoopFaultHookForTest();
        FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();

        qInfo("[P1-WINDOW] rounds=%d barrierEntries=%d windowRetries=%d retryOk=%d retryFailed=%d "
              "retryNotExecuted=%d maxRetryBlockMs=%d rejectedResidue=%d rejectedExecuted=%d restartOk=%d "
              "restartNotExecuted=%d",
              kRounds, barrierEntries.load(), windowRetries.load(), windowRetryOk.load(), windowRetryFailed.load(),
              retryNotExecuted.load(), maxRetryBlockMs, rejectedLeftResidue.load(), rejectedButExecuted.load(),
              restartOk.load(), restartNotExecuted.load());

        // 判别性前提：窗口必须真的被撑开并被重试覆盖若干次。
        QVERIFY2(windowRetries.load() >= 5,
                 qPrintable(QStringLiteral("the retirement window must be exercised (windowRetries=%1, "
                                           "barrierEntries=%2)")
                                .arg(windowRetries.load())
                                .arg(barrierEntries.load())));

        // 判据①：窗口内 startSingle() 的阻塞必须落在启动预算内。
        // 这是本轮 P1 的核心——此前该调用会一直等到旧线程退出（评审实测 500ms），
        // 而预算只有 100ms。允许少量调度误差，但绝不允许随退出延迟线性增长。
        const int budget = FlowExecutionService::StartHandshakeTimeoutMs;
        QVERIFY2(maxRetryBlockMs <= budget + kSchedulingSlackMs,
                 qPrintable(QStringLiteral("an in-window startSingle() must stay within the start budget "
                                           "(maxBlock=%1ms, budget=%2ms, slack=%3ms): an unbounded reclaim "
                                           "wait transfers the retiring thread's exit latency to the GUI thread")
                                .arg(maxRetryBlockMs)
                                .arg(budget)
                                .arg(kSchedulingSlackMs)));

        // 判据②③：允许明确拒绝，但拒绝不得留下 pending / 非 Idle 残留。
        QVERIFY2(rejectedLeftResidue.load() == 0,
                 qPrintable(QStringLiteral("a rejected start must leave the service idle with no pending job "
                                           "(rejectedResidue=%1, retryFailed=%2)")
                                .arg(rejectedLeftResidue.load())
                                .arg(windowRetryFailed.load())));

        // 判据④：被拒绝的那次绝不能事后执行（迟到执行是最严重的错误收口）。
        QVERIFY2(rejectedButExecuted.load() == 0,
                 qPrintable(QStringLiteral("a rejected start must never execute later (rejectedExecuted=%1): "
                                           "deferring the job would mean the rejection was a lie")
                                .arg(rejectedButExecuted.load())));

        // 判据④补：被接受的重试必须真的执行过任务——接受但空转等于命令进了黑洞。
        QVERIFY2(retryNotExecuted.load() == 0,
                 qPrintable(QStringLiteral("an accepted in-window retry must really execute the job "
                                           "(retryNotExecuted=%1)")
                                .arg(retryNotExecuted.load())));

        // 判据⑤：旧线程退出后再次启动必须成功，且无需手动 shutdown()。
        QVERIFY2(restartOk.load() >= 5,
                 qPrintable(QStringLiteral("the service must restart after the retiring thread is gone without "
                                           "a manual shutdown() (restartOk=%1, rounds=%2)")
                                .arg(restartOk.load())
                                .arg(kRounds)));
        QVERIFY2(restartNotExecuted.load() == 0,
                 qPrintable(QStringLiteral("an accepted restart must really execute the job "
                                           "(restartNotExecuted=%1)")
                                .arg(restartNotExecuted.load())));

        guard.finish();
    }

    // ==== T2.3 行为契约／回归保护（2026-11-06 评审裁定）====
    // 性质：基线已绿、非 NF、无生产修复提交。原计划「commitState 持锁 emit 导致
    // DirectConnection 回调死锁」的前提不成立：commitState 一直是 atomic store +
    // 锁外 emit，continueRun 的恢复请求由锁内谓词保存，不存在丢恢复的提交窗口。
    // 本用例固化该契约，防止未来重构（如 T2.7 等待者拆分）重新引入持锁通知。
    void testContinueImmediatelyAfterPauseIsNotLost() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        std::atomic<int> pausedSignals{0};
        std::atomic<int> continueResult{-1};
        // Paused 的 DirectConnection 回调恰落在「Paused 已提交、worker 尚未进入
        // m_wake.wait」的窗口：若通知在 m_mutex 内发出，槽内 continueRun() 会因
        // 非递归互斥量自死锁；若窗口丢失恢复，continueResult 会是 0。
        const QMetaObject::Connection conn = connect(
            &service, &FlowExecutionService::jobStateChanged, this,
            [&service, &pausedSignals, &continueResult](FlowExecutionService::State s, FlowExecutionService::JobType) {
                if (s == FlowExecutionService::State::Paused) {
                    ++pausedSignals;
                    continueResult.store(service.continueRun() ? 1 : 0, std::memory_order_release);
                }
            },
            Qt::DirectConnection);
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        // 失败路径收尾：必须最后声明，保证最先析构（此时上面所有栈变量仍有效）。
        ServiceContractCleanupGuard guard{&service, conn, nullptr, nullptr, nullptr, true};

        QVERIFY(service.startCycle());
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() >= 2, 5000);
        QVERIFY(service.pauseRun());
        QTRY_VERIFY_WITH_TIMEOUT(pausedSignals.load() >= 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(continueResult.load(), 1, 5000);
        QVERIFY2(continueResult.load() == 1,
                 "continueRun in the pause-commit window must be accepted (no deadlock, no lost resume)");
        const int runsAtResume = probe.runs.load();
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() > runsAtResume, 5000);
        service.stop();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        // 正常路径也以统一的安全收尾为准：确认实际任务结束且 worker 已被 join，
        // 再允许本用例栈上的 probe / 原子变量 / spy / 连接销毁。
        // finish() 返回即代表「实际任务结束 + shutdown() 成功 join」；
        // 未 join 时它不会返回（继续等待或终止进程），故无需恒真断言。
        guard.finish();
    }

    void testPauseResumeDoesNotDeadlockUnderDirectSignal() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        std::atomic<int> pausedSignals{0};
        const QMetaObject::Connection conn = connect(
            &service, &FlowExecutionService::jobStateChanged, this,
            [&service, &pausedSignals](FlowExecutionService::State s, FlowExecutionService::JobType) {
                if (s == FlowExecutionService::State::Paused) {
                    ++pausedSignals;
                    service.stop();
                }
            },
            Qt::DirectConnection);
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        ServiceContractCleanupGuard guard{&service, conn, nullptr, nullptr, nullptr, true};

        QVERIFY(service.startCycle());
        QTRY_VERIFY_WITH_TIMEOUT(probe.runs.load() >= 2, 5000);
        QVERIFY(service.pauseRun());
        QTRY_VERIFY_WITH_TIMEOUT(pausedSignals.load() >= 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        // 正常路径也以统一的安全收尾为准：确认实际任务结束且 worker 已被 join，
        // 再允许本用例栈上的 probe / 原子变量 / spy / 连接销毁。
        // finish() 返回即代表「实际任务结束 + shutdown() 成功 join」；
        // 未 join 时它不会返回（继续等待或终止进程），故无需恒真断言。
        guard.finish();
    }

    // ==== T2.4 启动握手超时（Service 层行为契约，2026-11-06 评审裁定方案二）====
    // 性质：三项预期行为已满足，尚未证实计划中的缺陷；基线已绿、非 NF、不做生产修复。
    // 用「进入 / 放行 / 完成」屏障替代固定 600ms 延迟，不靠 sleep 去撞 100ms 握手上限。
    // 交付链覆盖 enqueue():119 withdrawn=false（job 已出队 → stop requested）路径；
    // pending withdrawn（尚未出队）分支由独立工作树探针以 test-only 注入确定性覆盖，
    // 交付链不新增生产注入点（见证据文档第 7 节）。
    void testStartHandshakeTimeoutStopsDequeuedJobBeforeModulesRun() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        QSemaphore entered;
        QSemaphore release;
        QSemaphore completed;
        QSemaphore startReturned;
        std::atomic<int> startResult{-1};
        std::atomic<int> hookRuns{0};
        std::thread starter;
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);

        FlowExecutionServiceTestAccess(service).setStartHandshakeHookForTest([&] {
            ++hookRuns;
            entered.release();   // 进入：worker 已出队、处于启动协议内
            release.acquire();   // 放行：由测试线程在握手超时返回后释放
            completed.release(); // 完成：钩子已离开
        });

        // 失败路径收尾：先放行屏障、清钩子、join starter，再 stop/join worker。必须最后声明。
        ServiceContractCleanupGuard guard{
            &service,
            {},
            [&] {
                release.release();
                startReturned.release();
            },
            [&] { FlowExecutionServiceTestAccess(service).clearStartHandshakeHookForTest(); },
            &starter,
            true};

        QElapsedTimer startTimer;
        startTimer.start();
        starter = std::thread([&] {
            startResult.store(service.startSingle() ? 1 : 0, std::memory_order_release);
            startReturned.release();
        });

        // 进入屏障：job 已出队（currentJob 可见），但尚未 commitState(Running)。
        QVERIFY2(entered.tryAcquire(1, 5000), "job must be dequeued into the start handshake hook");
        QCOMPARE(service.currentJob(), FlowExecutionService::JobType::Single);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(service.isActive());
        QCOMPARE(hookRuns.load(), 1);

        // 放行前不释放钩子：握手到达 100ms 上限后必须返回 false，不阻塞调用线程。
        QVERIFY2(startReturned.tryAcquire(1, 5000), "startSingle must return at the handshake bound");
        QCOMPARE(startResult.load(), 0);
        QVERIFY2(startTimer.elapsed() < 2000,
                 qPrintable(QString("handshake must not block the caller, took %1 ms").arg(startTimer.elapsed())));
        QCOMPARE(probe.runs.load(), 0);

        // 完成屏障：放行后 job 必须在首个模块边界前协作停止，旧任务不执行任何模块。
        release.release();
        QVERIFY2(completed.tryAcquire(1, 5000), "start hook must be released");
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
        QCOMPARE(probe.runs.load(), 0);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(!service.isActive());

        // 恢复启动：清除钩子后下一次启动必须正常执行并完成。
        FlowExecutionServiceTestAccess(service).clearStartHandshakeHookForTest();
        QSignalSpy restartSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QTRY_COMPARE_WITH_TIMEOUT(restartSpy.count(), 1, 5000);
        QCOMPARE(probe.runs.load(), 1);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(!service.isActive());

        // finish() 返回即代表「实际任务结束 + shutdown() 成功 join」；
        // 未 join 时它不会返回（继续等待或终止进程），故无需恒真断言。
        guard.finish();
    }

    void testHandshakeTimeoutDoesNotLeaveResidualPendingState() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        QSemaphore entered;
        QSemaphore release;
        QSemaphore completed;
        QSemaphore startReturned;
        std::atomic<int> startResult{-1};
        std::thread starter;
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);

        FlowExecutionServiceTestAccess(service).setStartHandshakeHookForTest([&] {
            entered.release();
            release.acquire();
            completed.release();
        });

        ServiceContractCleanupGuard guard{
            &service,
            {},
            [&] {
                release.release();
                startReturned.release();
            },
            [&] { FlowExecutionServiceTestAccess(service).clearStartHandshakeHookForTest(); },
            &starter,
            true};

        starter = std::thread([&] {
            startResult.store(service.startSingle() ? 1 : 0, std::memory_order_release);
            startReturned.release();
        });

        QVERIFY2(entered.tryAcquire(1, 5000), "job must be dequeued into the start handshake hook");
        QVERIFY2(startReturned.tryAcquire(1, 5000), "startSingle must return at the handshake bound");
        QCOMPARE(startResult.load(), 0);

        // 撤回：超时后不得留下「稍后执行」的 pending。旧 job 仍在途（已出队），
        // 第二次启动必须被拒绝，而不是被排队等待旧任务迟到执行。
        QVERIFY2(!service.startSingle(), "a timed-out start must not leave a queued pending job");
        QCOMPARE(probe.runs.load(), 0);

        release.release();
        QVERIFY2(completed.tryAcquire(1, 5000), "start hook must be released");
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QCOMPARE(finishedSpy.first().at(1).toBool(), true);
        QCOMPARE(probe.runs.load(), 0); // 旧任务不迟到执行
        QCOMPARE(service.currentJob(), FlowExecutionService::JobType::None);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);
        QVERIFY(!service.isActive());

        // 恢复启动：超时遗留的 m_startSettled / m_startOk 不得污染后续握手，
        // 连续两次正常启动都必须成功并各自完成。
        FlowExecutionServiceTestAccess(service).clearStartHandshakeHookForTest();
        for (int i = 1; i <= 2; ++i) {
            QSignalSpy restartSpy(&service, &FlowExecutionService::jobFinished);
            QVERIFY2(service.startSingle(), qPrintable(QString("restart #%1 must succeed").arg(i)));
            QTRY_COMPARE_WITH_TIMEOUT(restartSpy.count(), 1, 5000);
            QCOMPARE(probe.runs.load(), i);
            QCOMPARE(service.currentJob(), FlowExecutionService::JobType::None);
            QVERIFY(!service.isActive());
        }

        // finish() 返回即代表「实际任务结束 + shutdown() 成功 join」；
        // 未 join 时它不会返回（继续等待或终止进程），故无需恒真断言。
        guard.finish();
    }

    // ==== T2.5 P1 第三轮（2026-11-11 二次复核 P1）：shutdown 必须覆盖所有未退出 worker ====
    //
    // 缺陷前提：启动路径的有界回收（retireWorkerThreadBounded）在预算用尽时把旧 worker
    // 交给一个 detached std::thread 等待并 delete，而 m_thread 早已被 enqueue() 清空。
    // 于是该 worker **不在任何 Service 侧结构中**：shutdown() 的 Idle 分支只取 m_thread，
    // 直接返回 true（评审独立复现：retry=0 retryMs=100、shutdownOk=1 shutdownMs=0、
    // oldWorkerStillRunning=1），而在跑的 worker 仍会访问本服务与模块，清理 guard 也可能
    // 在它仍活跃时释放被捕获的栈对象。后台线程最终 wait() 不能替代 Service 销毁前的同步。
    //
    // 本用例固定该契约：**shutdown() 返回 true 时，所有 worker 必须已真正退出。**
    //
    // 时序（与上一用例同一个窗口，不靠碰运气）：workerLoop 故障 → jobFailed → 退出屏障
    // 撑开窗口 → 主线程在窗口内重试 → 预算用尽被明确拒绝 → 立刻 shutdown()。此时旧 worker
    // 仍停在屏障上（run() 未返回），正确实现必须阻塞在 join 上，直到控制线程放行。
    void testShutdownCoversWorkerHandedOffByRejectedStart() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        constexpr int kRounds = 3;
        // 「提前返回」观测窗口：缺陷实现下 shutdown() 立即返回（shutdownMs≈0）而旧 worker
        // 仍在 run()；本用例以「shutdown() 返回那一刻是否仍有 worker 在跑」作为直接判据，
        // 不依赖固定等待窗口，避免给服务套件叠加延迟。

        std::atomic_int armed{0};
        std::atomic_int notified{0};
        std::atomic_int barrierEntries{0}; // 屏障进入次数（证明窗口真的被撑开）
        std::atomic_int shutdownRounds{0}; // 实际完成 shutdown 观测的轮数
        std::atomic_int earlyReturns{0};   // shutdown() 在旧 worker 退出前返回 true 的轮数（缺陷）
        std::atomic_bool oldWorkerRunning{false};

        QSemaphore releaseBarrier;
        QSemaphore barrierReached;
        std::atomic_bool holdBarrier{false};

        FlowExecutionServiceTestAccess(service).setWorkerLoopFaultHookForTest([&] {
            if (armed.load(std::memory_order_acquire) != 0 && armed.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                throw std::runtime_error("injected worker-loop fault");
            }
        });

        // 退出屏障：位于 handleWorkerLoopFault() 发出 jobFailed 之后、run() 返回之前。
        FlowExecutionServiceTestAccess(service).setWorkerExitBarrierForTest([&] {
            if (!holdBarrier.load(std::memory_order_acquire)) {
                return;
            }
            oldWorkerRunning.store(true, std::memory_order_release);
            ++barrierEntries;
            barrierReached.release();
            releaseBarrier.tryAcquire(1, 5000); // 有界：任何失败路径下 worker 都能自行退出
            oldWorkerRunning.store(false, std::memory_order_release);
        });

        // 槽内只做状态观测，不发起重试、不阻塞（见上一用例的说明）。
        QMetaObject::Connection failConn = QObject::connect(
            &service, &FlowExecutionService::jobFailed, &service,
            [&](FlowExecutionService::JobType, const QString&) { ++notified; }, Qt::DirectConnection);

        // 失败路径收尾：放行屏障、清钩子、join worker。必须最后声明。
        ServiceContractCleanupGuard guard{&service,
                                          failConn,
                                          [&] {
                                              holdBarrier.store(false, std::memory_order_release);
                                              releaseBarrier.release();
                                          },
                                          [&] {
                                              FlowExecutionServiceTestAccess(service).clearWorkerLoopFaultHookForTest();
                                              FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();
                                          },
                                          nullptr,
                                          true};

        for (int round = 0; round < kRounds; ++round) {
            // 每轮从干净 Idle 开始（上一轮 shutdown 已销毁线程）。
            QElapsedTimer idle;
            idle.start();
            while (service.isActive() && idle.elapsed() < 1500) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
            if (service.isActive()) {
                break; // 收尾由 guard 负责
            }

            holdBarrier.store(true, std::memory_order_release);
            armed.store(1, std::memory_order_release);

            const bool firstStart = service.startSingle();
            const bool atWindow = barrierReached.tryAcquire(1, 3000);
            if (firstStart || !atWindow) {
                holdBarrier.store(false, std::memory_order_release);
                releaseBarrier.release();
                continue; // 本轮不计入判别样本
            }

            // 窗口内重试：预算内等不到旧 worker 退出 → 必须明确拒绝（不阻塞、不入队）。
            QElapsedTimer retryTimer;
            retryTimer.start();
            const bool retryOk = service.startSingle();
            const qint64 retryMs = retryTimer.elapsed();
            const bool rejected = !retryOk;

            // 拒绝后应立即是干净 Idle：没有 pending、没有 current job。
            const bool cleanIdle = !service.isActive() && service.state() == FlowExecutionService::State::Idle;

            // 时序设计（关键，勿改成「主线程自己放行」）：
            //   缺陷态：shutdown() 立即返回 true（旧 worker 仍在 run()）→ 采样到 stillRunning=1；
            //   修复态：shutdown() 阻塞在 join 上，直到控制线程放行旧 worker 才返回。
            // 因此放行**必须由控制线程在「确认 shutdown 正在阻塞或已返回」之后**执行：
            //   - 若主线程在放行前就已返回（只有缺陷实现会这样），控制线程的稍后放行只是
            //     解除屏障，不影响已采样的判据；
            //   - 修复态下控制线程在固定短延时后放行，shutdown() 随即完成 join。
            // 放行延时 kReleaseDelayMs 必须**小于**启动预算的观测意义下限、又足以让缺陷态
            // 完成一次「立即返回」——缺陷态实测 shutdownMs≈0，故 50ms 足够。
            constexpr int kReleaseDelayMs = 50;
            std::atomic_bool shutdownReturned{false};
            std::atomic_bool released{false};
            std::thread control([&] {
                // 等到 shutdown() 返回（缺陷态：几乎立刻）或短延时到期，然后放行屏障。
                QElapsedTimer waitTimer;
                waitTimer.start();
                while (!shutdownReturned.load(std::memory_order_acquire) && waitTimer.elapsed() < kReleaseDelayMs) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                // 若 shutdown() 仍阻塞（修复态），再等一个短延时后放行，让它完成 join。
                if (!shutdownReturned.load(std::memory_order_acquire)) {
                    QElapsedTimer grace;
                    grace.start();
                    while (grace.elapsed() < kReleaseDelayMs) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                if (!released.exchange(true)) {
                    holdBarrier.store(false, std::memory_order_release);
                    releaseBarrier.release();
                }
            });

            QElapsedTimer shutdownTimer;
            shutdownTimer.start();
            const bool shutdownOk = service.shutdown();
            const qint64 shutdownMs = shutdownTimer.elapsed();
            shutdownReturned.store(true, std::memory_order_release);
            // 采样：shutdown() 返回的**那一刻**旧 worker 是否仍在 run() 内。
            const bool stillRunning = oldWorkerRunning.load(std::memory_order_acquire);

            // 回收控制线程（其内部等待均有界）；再兜底放行一次，避免任何残留令牌。
            control.join();
            if (!released.exchange(true)) {
                holdBarrier.store(false, std::memory_order_release);
                releaseBarrier.release();
            }
            releaseBarrier.tryAcquire(1, 2000); // 排水：防止多放的令牌串轮

            if (shutdownOk && stillRunning) {
                qInfo("[P1-OWNER] round %d: shutdown returned true after %lld ms while the retired worker "
                      "was still running",
                      round, static_cast<long long>(shutdownMs));
            }

            if (retryMs > FlowExecutionService::StartHandshakeTimeoutMs + 60) {
                // 重试本身超出预算属上一轮 P1 的问题，此处不计入判别样本，但记录以便诊断。
                qWarning("[P1-OWNER] round %d: in-window retry blocked %lld ms", round,
                         static_cast<long long>(retryMs));
            }
            if (!rejected || !cleanIdle) {
                continue; // 前提不成立（未走到「拒绝」分支），不计入判别样本
            }

            ++shutdownRounds;
            if (shutdownOk && stillRunning) {
                ++earlyReturns;
            }

            // 为下一轮准备：重新布防一个可退出的 Idle 状态。
            QElapsedTimer settle;
            settle.start();
            while (service.isActive() && settle.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
        }

        QObject::disconnect(failConn);
        FlowExecutionServiceTestAccess(service).clearWorkerLoopFaultHookForTest();
        FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();

        qInfo("[P1-OWNER] rounds=%d barrierEntries=%d shutdownRounds=%d earlyReturns=%d notified=%d", kRounds,
              barrierEntries.load(), shutdownRounds.load(), earlyReturns.load(), notified.load());

        // 判别性前提：必须真的走到「拒绝启动后立即 shutdown」若干轮，且窗口确实被撑开。
        QVERIFY2(barrierEntries.load() >= 1,
                 qPrintable(QStringLiteral("the worker-exit barrier must be entered at least once "
                                           "(barrierEntries=%1)")
                                .arg(barrierEntries.load())));
        QVERIFY2(shutdownRounds.load() >= 1,
                 qPrintable(QStringLiteral("the case must exercise 'rejected start then immediate shutdown' "
                                           "(shutdownRounds=%1, barrierEntries=%2)")
                                .arg(shutdownRounds.load())
                                .arg(barrierEntries.load())));

        // 核心判据：shutdown() 返回 true 时绝不能还有 worker 在跑。
        // 缺陷态实测 shutdownMs≈0 且仍返回 true —— 这里必然命中。
        QVERIFY2(earlyReturns.load() == 0,
                 qPrintable(QStringLiteral("shutdown() must not report success while a worker handed off by a "
                                           "rejected start is still running (earlyReturns=%1/%2): the retired "
                                           "worker must stay owned by the service until shutdown() joins it")
                                .arg(earlyReturns.load())
                                .arg(shutdownRounds.load())));

        // 二次判据：失败路径收尾无法清 armed 时 finish() 不会返回，故显式调用。
        guard.finish();
    }

    // ==== T2.5 P1 第四次整改（2026-11-11 复核 P1）：回收等待中的并发 shutdown ====
    //
    // 缺陷前提：enqueue() 先在锁内把旧 worker 从 m_thread 摘下（`:273`），再到**锁外**
    // 执行 `thread->wait(budget)`，直到等待超时才在 `:157` 把它登记进 retiring 列表。
    // 这段锁外等待期间，该 worker **只存在于回收线程的栈上**：并发 shutdown() 的
    // detachLiveWorkerThreadsLocked() 拿到的是空的 m_thread + 空的 retiring，于是
    // 立即返回 true，而旧 worker 仍在 run() 内。评审独立复现：
    //   shutdownOk=1 / returnedBeforeWaitReleased=1 / oldWorkerRunningAtReturn=1
    //
    // 与上一用例（`testShutdownCoversWorkerHandedOffByRejectedStart`）的区别：那条是
    // 「启动被拒绝**返回之后**再 shutdown」，此时线程已经在 retiring 里；本用例是
    // 「回收**等待尚未结束**时并发 shutdown」——正是本轮新增的窗口。
    //
    // 窗口如何确定性撑开（不靠碰运气）：退出屏障把旧 worker 停在「jobFailed 已发出、
    // run() 未返回」处，回收线程必然进入锁外 wait 且等满预算；本用例在**另一线程**
    // 上发起 startSingle()（回收发生在那条线程里），主线程此时调 shutdown()，并断言
    // shutdown() 返回那一刻旧 worker 仍在 run() 且回收线程尚未结束。
    void testShutdownDuringInflightReclaimMustNotReportSuccess() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        // 用例轮数少而稳：每轮都要等满一次启动预算，3 轮足以判别且不拖长套件。
        constexpr int kRounds = 3;
        // 回收线程在屏障放行前至少会阻塞 StartHandshakeTimeoutMs（100ms）。为可靠地
        // 让主线程在「锁外等待中」发起 shutdown()，屏障额外保持一小段时间。
        constexpr int kInflightHoldMs = 30;

        std::atomic_int armed{0};
        std::atomic_int notified{0};
        std::atomic_int inflightRounds{0}; // 成功构造出「回收等待中」窗口的轮数
        std::atomic_int earlyReturns{0};   // shutdown 在旧 worker 退出前返回 true（缺陷）
        std::atomic_bool oldWorkerRunning{false};
        std::atomic_bool barrierEntered{false};

        QSemaphore releaseBarrier;
        QSemaphore barrierReached;
        std::atomic_bool holdBarrier{false};

        FlowExecutionServiceTestAccess(service).setWorkerLoopFaultHookForTest([&] {
            if (armed.load(std::memory_order_acquire) != 0 && armed.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                throw std::runtime_error("injected worker-loop fault");
            }
        });

        // 退出屏障：旧 worker 在此停住（run() 未返回）。
        FlowExecutionServiceTestAccess(service).setWorkerExitBarrierForTest([&] {
            if (!holdBarrier.load(std::memory_order_acquire)) {
                return;
            }
            oldWorkerRunning.store(true, std::memory_order_release);
            barrierEntered.store(true, std::memory_order_release);
            barrierReached.release();
            releaseBarrier.tryAcquire(1, 5000); // 有界：任何失败路径下 worker 都能自行退出
            oldWorkerRunning.store(false, std::memory_order_release);
        });

        QMetaObject::Connection failConn = QObject::connect(
            &service, &FlowExecutionService::jobFailed, &service,
            [&](FlowExecutionService::JobType, const QString&) { ++notified; }, Qt::DirectConnection);

        // 失败路径收尾：放行屏障、清钩子、join starter 与 worker。必须最后声明。
        std::thread starter;
        ServiceContractCleanupGuard guard{&service,
                                          failConn,
                                          [&] {
                                              holdBarrier.store(false, std::memory_order_release);
                                              releaseBarrier.release();
                                          },
                                          [&] {
                                              FlowExecutionServiceTestAccess(service).clearWorkerLoopFaultHookForTest();
                                              FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();
                                          },
                                          &starter,
                                          true};

        for (int round = 0; round < kRounds; ++round) {
            QElapsedTimer idle;
            idle.start();
            while (service.isActive() && idle.elapsed() < 1500) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
            if (service.isActive()) {
                break; // 收尾由 guard 负责
            }

            holdBarrier.store(true, std::memory_order_release);
            barrierEntered.store(false, std::memory_order_release);
            armed.store(1, std::memory_order_release);

            // 首启：注入故障，等启动握手被放行后 worker 停在退出屏障上。
            const bool firstStart = service.startSingle();
            const bool atBarrier = barrierReached.tryAcquire(1, 3000);
            if (firstStart || !atBarrier) {
                holdBarrier.store(false, std::memory_order_release);
                releaseBarrier.release();
                continue; // 本轮不计入判别样本
            }

            // 在**另一线程**上发起重试：回收（含锁外 wait）发生在该线程内，
            // 主线程得以在回收进行中并发调用 shutdown()。
            std::atomic_bool retryReturned{false};
            std::atomic_bool retryOk{false};
            std::atomic_bool released{false};
            starter = std::thread([&] {
                retryOk.store(service.startSingle(), std::memory_order_release);
                retryReturned.store(true, std::memory_order_release);
                // 缺陷态下 shutdown() 会先返回并把线程收归自己/或漏掉；修复态下 shutdown()
                // 需要本线程放弃所有权后才能 join 完成。这里在有界延迟内放行屏障，
                // 保证任何实现都能收敛，不构成自锁。
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < 3000 && !released.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (!released.exchange(true)) {
                    holdBarrier.store(false, std::memory_order_release);
                    releaseBarrier.release();
                }
            });

            // 等 retry 线程真正进入「锁外等待」：屏障已进入 + retry 尚未返回即证明它正在
            // 回收预算内等待（这是可判别的前置条件，不是睡眠撞窗口）。
            QElapsedTimer spin;
            spin.start();
            bool inflightObserved = false;
            while (spin.elapsed() < 50) {
                if (barrierEntered.load(std::memory_order_acquire) && !retryReturned.load(std::memory_order_acquire)) {
                    inflightObserved = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (inflightObserved) {
                // 屏障额外保持，确保 shutdown() 落在回收的锁外等待期间。
                QThread::msleep(kInflightHoldMs);
            }

            // —— 核心：回收等待中并发 shutdown() ——
            QElapsedTimer shutdownTimer;
            shutdownTimer.start();
            const bool shutdownOk = service.shutdown();
            const qint64 shutdownMs = shutdownTimer.elapsed();
            const bool stillRunning = oldWorkerRunning.load(std::memory_order_acquire);
            const bool retryStillRunning = !retryReturned.load(std::memory_order_acquire);

            if (inflightObserved) {
                ++inflightRounds;
                if (shutdownOk && (stillRunning || retryStillRunning)) {
                    ++earlyReturns;
                    qInfo("[P1-INFLIGHT] round %d: shutdown returned true after %lld ms while the retired "
                          "worker was still running",
                          round, static_cast<long long>(shutdownMs));
                }
            }

            // 放行并回收 worker/starter，为下一轮准备。
            if (!released.exchange(true)) {
                holdBarrier.store(false, std::memory_order_release);
                releaseBarrier.release();
            }
            if (starter.joinable()) {
                starter.join();
            }
            releaseBarrier.tryAcquire(1, 2000); // 排水

            QElapsedTimer settle;
            settle.start();
            while (service.isActive() && settle.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
        }

        if (starter.joinable()) {
            starter.join();
        }
        QObject::disconnect(failConn);
        FlowExecutionServiceTestAccess(service).clearWorkerLoopFaultHookForTest();
        FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();

        qInfo("[P1-INFLIGHT] rounds=%d inflightRounds=%d earlyReturns=%d notified=%d", kRounds, inflightRounds.load(),
              earlyReturns.load(), notified.load());

        // 正向前提：必须真的构造出「回收等待中」的窗口，否则本用例没有判别力。
        QVERIFY2(inflightRounds.load() >= 1,
                 qPrintable(QStringLiteral("the case must observe an in-flight reclaim window before calling "
                                           "shutdown() (inflightRounds=%1, rounds=%2)")
                                .arg(inflightRounds.load())
                                .arg(kRounds)));

        // 核心判据：shutdown() 返回 true 时不得还有 worker 在跑，也不得还有回收线程
        // 持着尚未交出的 worker。
        QVERIFY2(earlyReturns.load() == 0,
                 qPrintable(QStringLiteral("shutdown() must not report success while a bounded reclaim is still "
                                           "waiting for the retired worker (earlyReturns=%1/%2): the thread must "
                                           "be visible to the service *before* the off-lock wait")
                                .arg(earlyReturns.load())
                                .arg(inflightRounds.load())));

        guard.finish();
    }

    // T2.5 P1 第六次整改（2026-11-11 第五段复核 P1）：**删除权唯一 ≠ 对象已无人使用**。
    //
    // 第四次整改让 shutdown() 能「接管」在途回收的线程并成为唯一删除者，但接管时那条
    // 线程可能正被回收调用借用在 `thread->wait()` 内。shutdown() 移走指针、join 完、
    // delete 掉之后，回收线程仍会经由已释放对象完成等待收尾 —— use-after-free。
    // 评审独立探针（此时线程尚在 wait 内即被删除）：
    //   shutdownOk=1 / shutdownBeforeReclaimReturned=1 / workerDeletedWhileWaitActive=1
    //
    // 本用例把这三项翻译成可在套件里复跑的判据：**在 shutdown() 返回的那一刻**采样
    // 「回收调用是否尚未返回」。若为真，说明 worker 在仍被使用时就进入了删除流程。
    void testShutdownMustWaitForActiveReclaimBorrowBeforeDeleting() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"),
                                                                      ExecutionAffinity::Worker);

        constexpr int kRounds = 3;
        // worker 停在退出屏障里；屏障由用例在放行点显式释放，固定上限只作兜底。
        // 这样「回收调用正在 wait」的时段可以被稳定命中，而不是靠 sleep 撞窗口。
        constexpr int kBarrierHoldMs = 20000;

        std::atomic_int armed{0};
        std::atomic_int windowHits{0};          // shutdown 返回时回收调用仍在 wait（判别性采样）
        std::atomic_int earlyDeletes{0};        // 其中 shutdown 还报告成功（缺陷）
        std::atomic_int invariantViolations{0}; // 「在 inflight 但借用为 0」被抓到的次数
        std::atomic_int shutdownOkRounds{0};
        std::atomic_int completedRounds{0};
        std::atomic_bool oldWorkerRunning{false};
        std::atomic_bool barrierEntered{false};

        std::atomic_bool holdBarrier{false};
        QSemaphore releaseBarrier;
        QSemaphore barrierReached;
        // 回收等待钩子：把回收调用停在「借用已登记、thread->wait() 尚未开始」处。
        QSemaphore reclaimHookEntered;
        QSemaphore releaseReclaimHook;
        std::atomic_bool reclaimHookEnabled{false};
        // 临界区内钩子：此刻持锁且借用已登记，用于确认「登记与移交同临界区」。
        std::atomic_int inCriticalSectionSeen{0};

        FlowExecutionServiceTestAccess(service).setReclaimWaitHookForTest([&] {
            if (!reclaimHookEnabled.load(std::memory_order_acquire)) {
                return;
            }
            reclaimHookEnabled.store(false, std::memory_order_release);
            reclaimHookEntered.release();
            releaseReclaimHook.tryAcquire(1, 5000);
        });

        FlowExecutionServiceTestAccess(service).setInCriticalSectionHookForTest([&] {
            // 持锁调用：不得再取锁、不得阻塞。
            // **这是本用例最关键的判据点**：此刻仍在 enqueue() 搬移线程的那个临界区内。
            // 不变量要求：「凡已进入 m_inflightReclaim 的线程，必须已有活跃借用」。
            // 若借用登记被推迟到锁外（缺陷态），这个窗口内就能被直接看到违规。
            if (!reclaimHookEnabled.load(std::memory_order_acquire)) {
                return;
            }
            ++inCriticalSectionSeen;
            // inflightWithoutBorrowCountForTest() 自己会取锁 —— 这里已持锁，不能调用。
            // 因此改由外部在**同一临界区结束后立刻**采样（见下面的探针）。
        });

        FlowExecutionServiceTestAccess(service).setWorkerLoopFaultHookForTest([&] {
            if (armed.load(std::memory_order_acquire) != 0 && armed.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                throw std::runtime_error("injected worker-loop fault");
            }
        });

        FlowExecutionServiceTestAccess(service).setWorkerExitBarrierForTest([&] {
            if (!holdBarrier.load(std::memory_order_acquire)) {
                return;
            }
            oldWorkerRunning.store(true, std::memory_order_release);
            barrierEntered.store(true, std::memory_order_release);
            barrierReached.release();
            // 有界：任何失败路径下 worker 都能自行退出，不会让用例挂死。
            releaseBarrier.tryAcquire(1, kBarrierHoldMs);
            oldWorkerRunning.store(false, std::memory_order_release);
        });

        QMetaObject::Connection failConn = QObject::connect(
            &service, &FlowExecutionService::jobFailed, &service, [](FlowExecutionService::JobType, const QString&) {},
            Qt::DirectConnection);

        // 异步启动线程：只负责发起 startSingle() 并记录结果，**不承担任何放行职责**。
        std::thread starter;
        ServiceContractCleanupGuard guard{
            &service,
            failConn,
            [&] {
                holdBarrier.store(false, std::memory_order_release);
                releaseBarrier.release();
                releaseReclaimHook.release();
            },
            [&] {
                FlowExecutionServiceTestAccess(service).clearWorkerLoopFaultHookForTest();
                FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();
                FlowExecutionServiceTestAccess(service).clearReclaimWaitHookForTest();
                FlowExecutionServiceTestAccess(service).clearInCriticalSectionHookForTest();
            },
            &starter,
            true};

        for (int round = 0; round < kRounds; ++round) {
            // 每轮从 Idle 开始，否则上一轮收尾未清干净会让本轮前置条件失真。
            QElapsedTimer idle;
            idle.start();
            while (service.isActive() && idle.elapsed() < 1500) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
            if (service.isActive()) {
                break; // 收尾由 guard 负责
            }

            armed.store(0, std::memory_order_release);
            barrierEntered.store(false, std::memory_order_release);
            oldWorkerRunning.store(false, std::memory_order_release);
            reclaimHookEnabled.store(false, std::memory_order_release); // 预热阶段不得触发回收钩子

            // **P2 整改（2026-11-12 第六段复核）：worker 退出与借用归还必须独立受控。**
            //
            // 缺陷态必须落在「旧 worker 仍在退出屏障内、回收调用仍持有借用」这个窗口里，
            // 才谈得上「shutdown 不等待借用就删除」。因此：
            //   控制点 A = releaseReclaimHook  → 回收调用是否归还借用；
            //   控制点 B = releaseBarrier      → 旧 worker 是否退出。
            // 两者都不由 startSingle() 的返回驱动。
            //
            // 上一版把「放行 B」写在 starter 线程里 `startSingle()` 之后，隐含了
            // 「startSingle() 已返回 ⇒ worker 已退出」这一**测试自己制造的**顺序，
            // 于是即便把生产代码里等待借用的助手换成空操作，用例仍然全绿（复核实测 3/3），
            // 完全没有判别力。
            //
            // 现在只构造一次「借用仍活跃、worker 尚未退出」的窗口，并在窗口正中对**一次**
            // shutdown() 取证 —— 同一进程只允许一次真正的销毁，重复 shutdown 无法复现缺陷。
            //
            // **顺序要点**：先把屏障**关掉**建好常驻 worker（否则首启的启动握手会撞上
            // 回收调用等 worker 退出、而 worker 正卡在屏障里 → 「wait on itself」），
            // 再武装屏障、让 worker 在下一次退出时停住。
            holdBarrier.store(false, std::memory_order_release);
            // 预热阶段必须**关掉**故障注入：armed=1 会让 worker 第一轮就抛错退役，
            // 于是在屏障未启用时那次启动仍会走回收路径，预热本身就会失败。
            armed.store(0, std::memory_order_release);
            if (starter.joinable()) {
                starter.join();
            }
            {
                QSignalSpy warmupSpy(&service, &FlowExecutionService::jobFinished);
                // 预热可能需要重试：上一轮留下的旧 worker 刚被放行，仍在退出途中，
                // 首次启动会被「旧线程尚未退出」的预算拒绝。重试只在预热阶段发生，
                // **不参与任何判据**（判据只看关闭期间那一次启动）。
                bool warmed = false;
                QElapsedTimer warmup;
                warmup.start();
                while (!warmed && warmup.elapsed() < 5000) {
                    if (service.startSingle()) {
                        warmed = true;
                        break;
                    }
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(5);
                }
                QVERIFY2(warmed, "warm-up start must eventually succeed so the case has a resident worker");
                QTRY_COMPARE_WITH_TIMEOUT(warmupSpy.count(), 1, 5000);
                QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Idle, 5000);
            }
            // 预热后**再**武装：此刻 worker 处于空闲等待。
            holdBarrier.store(true, std::memory_order_release);

            // **第一步：让旧 worker 退役并卡在退出屏障上。**
            // armed=1 使下一次出队的 job 在 workerLoop() 入口抛错 → 线程置 retiring →
            // workerLoop() 返回 → 进入退出屏障（holdBarrier=true）→ 就此停住。
            // 这一步必须发生在**回收调用之前**，否则回收调用根本没有「要等它退出」的对象。
            armed.store(1, std::memory_order_release);
            barrierEntered.store(false, std::memory_order_release);
            {
                // 注意：**不能用 startSingle() 的返回值判断这一步是否成功**。
                // armed=1 的故障注入让本 job 必失败，启动握手拿不到 Running，
                // startSingle() 因此返回 false —— 但 worker 确实出队、抛错、退役、
                // 并停在退出屏障上，正是我们要的状态。判据只看 barrierReached。
                (void) service.startSingle();
                if (!barrierReached.tryAcquire(1, 3000)) {
                    armed.store(0, std::memory_order_release);
                    holdBarrier.store(false, std::memory_order_release);
                    releaseBarrier.release();
                    continue;
                }
            }
            // 到此：旧 worker 停在退出屏障内（oldWorkerRunning=true），下一次启动必然走回收。
            reclaimHookEnabled.store(true, std::memory_order_release);

            // P3 更正：本变量**只**用于下方 join 的有界兜底，绝不参与任何判据。
            // 此前它初始化为 false 后从不被写入，导致三轮各空等 5s、诊断恒真。
            std::atomic_bool reclaimReturned{false};
            starter = std::thread([&] {
                // **回收调用所在的线程**：它进入 retireWorkerThreadBounded() 后停在
                // 回收钩子上，借用由它持有。放行 worker 退出、放行回收调用这两件事
                // 都由主线程控制，本线程不做任何放行——这正是 P2 要求的「独立受控」。
                (void) service.startSingle();
                reclaimReturned.store(true, std::memory_order_release);
            });

            // **确定性前置**：回收调用已进入钩子，即「借用已生效、thread->wait() 尚未开始」。
            // 等不到就放弃本轮，不计入判别样本——不制造假阴性。
            if (!reclaimHookEntered.tryAcquire(1, 3000)) {
                reclaimHookEnabled.store(false, std::memory_order_release);
                releaseReclaimHook.release();
                if (starter.joinable()) {
                    starter.join();
                }
                continue;
            }

            // —— 核心判据之一：不变量探针（在回收调用整个生命周期内高频采样）——
            // 缺陷态下，enqueue() 解锁之后到 retireWorkerThreadBounded() 登记借用之前，
            // 该线程已在 m_inflightReclaim、借用却为 0。探针在锁内读，必能抓到这一瞬间。
            // 这不需要构造时序窗口，是**直接**判据。
            {
                QElapsedTimer probe;
                probe.start();
                while (probe.elapsed() < 20) {
                    if (FlowExecutionServiceTestAccess(service).inflightWithoutBorrowCountForTest() > 0) {
                        ++invariantViolations;
                        break;
                    }
                    std::this_thread::yield();
                }
            }

            // **判据是借用何时归还，而不是 startSingle() 何时返回**：
            // 正确实现完全可以在归还借用之后继续走握手、稍后才让 startSingle() 返回，
            // 把「startSingle() 是否已返回」当判据会把正确实现误判为缺陷。
            //
            // 采样点放在 shutdown() 返回的那一刻：>0 表示 shutdown 在仍有人使用该对象
            // 时就完成了（缺陷）；==0 表示它确实等到了借用归还（正确）。
            //
            // **控制点 B：先放行旧 worker 退出，再发起 shutdown。这一步是判别力的关键。**
            //
            // 上一版让 worker 一直卡在退出屏障上，结果 shutdown() 里的
            // joinWorkerThread() -> thread->wait() 恰好被这条 worker 挡住，于是即便
            // 「等待借用归还」被换成空操作，shutdown() 也一样晚返回、判据读到 0
            // （实测 borrows=1 guards=1，但 at return: liveGuards=0，缺陷态仍全绿）。
            // 换句话说：等 worker 退出与等借用归还，在「worker 还没退出」时**恰好同效**，
            // 掩盖了差异。
            //
            // 因此顺序改为：放行 worker 退出并**确认它已真正结束** -> 此时
            // joinWorkerThread() 的 thread->wait() 会立即返回，shutdown() 唯一的
            // 阻挡就只剩「等待借用归还」。回收调用仍停在钩子上（控制点 A 未放行），
            // 借用因此确定处于持有状态。
            holdBarrier.store(false, std::memory_order_release);
            releaseBarrier.release();
            {
                QElapsedTimer exitWait;
                exitWait.start();
                while (oldWorkerRunning.load(std::memory_order_acquire) && exitWait.elapsed() < 5000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            // 此刻状态（全部由本用例显式控制，与 startSingle() 无关）：
            //   - 旧 worker **已退出**（oldWorkerRunning=false）→ thread->wait() 不再挡路；
            //   - 回收调用仍停在钩子上 → 借用已登记且未归还；
            //   - 服务处于 Idle（未入队任何 job）→ shutdown() 会走**成功关闭路径**。
            std::atomic_bool shutdownDone{false};
            std::atomic_bool shutdownOkFlag{false};
            std::atomic_int liveGuardsAtReturn{-1};
            std::atomic_bool workerRunningAtReturn{false};
            std::thread shutdownThread([&] {
                const bool ok = service.shutdown();
                // **判据采样点必须紧贴 shutdown() 返回的那一行，且读「守卫存活数」而非「计数表」。**
                //
                // 为什么不能读计数表（activeReclaimBorrowCountForTest）：
                // 计数表就是 shutdown() 的等待条件。正确的 shutdown() 在
                // waitForReclaimBorrowsReturned() 之后才继续，因此它返回时计数表**必然**为 0；
                // 而缺陷态的 shutdown() 不等借用就返回，此时计数表同样可能为 0
                // （回收守卫已被放行并归还）。于是「计数表 > 0」在两种实现下都读不到，
                // 上一版判据因此毫无判别力（缺陷态 3/3 全绿，第 17 章记录）。
                //
                // 守卫存活数（liveReclaimGuardCountForTest）数的是「回收调用是否仍在作用域内」，
                // 不是 shutdown() 的等待条件。缺陷态下 shutdown() 不等就返回时，回收守卫仍
                // 停在钩子上（作用域内）→ 必然 > 0；正确实现下 shutdown() 先等它归还 → 必为 0。
                liveGuardsAtReturn.store(FlowExecutionServiceTestAccess(service).liveReclaimGuardCountForTest(),
                                         std::memory_order_release);
                workerRunningAtReturn.store(oldWorkerRunning.load(std::memory_order_acquire),
                                            std::memory_order_release);
                shutdownOkFlag.store(ok, std::memory_order_release);
                shutdownDone.store(true, std::memory_order_release);
            });

            // 给 shutdown 线程足够时间真正进入（取锁、搬列表、开始等借用）。
            //
            // **这一步是判别力的关键，不是「让步」。** 缺陷态与修复态的区别就在于：
            //   - 修复态：shutdown() 阻塞在 waitForReclaimBorrowsReturned() 上，不会返回
            //     → 循环在 300ms 后自然退出，arrivedWhileBorrowActive=true，随后放行；
            //   - 缺陷态：shutdown() 不等借用就返回 → shutdownDone 立刻为真，此时采样点
            //     已经读到守卫仍在作用域内。
            // 因此 300ms 只是「确认它没有提前返回」的上限，不参与判据本身。
            {
                QElapsedTimer spin;
                spin.start();
                while (!shutdownDone.load(std::memory_order_acquire) && spin.elapsed() < 300) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            // 关键正向断言：放行前 shutdown **必须尚未返回**，否则本轮没有构造出窗口。
            const bool arrivedWhileBorrowActive = !shutdownDone.load(std::memory_order_acquire);
            releaseReclaimHook.release();

            // **收尾 join 的有界兜底**：等回收调用线程的完成通知（P3 更正：此前该
            // 变量从不被写入，三轮各空等 5s，后面的「仍在回收」诊断也恒真）。它**只**
            // 用于避免主线程在 join 上永久阻塞，绝不参与判据——判据始终只挂在
            // liveGuardsAtShutdownReturn / workerRunningAtShutdownReturn 上。
            {
                QElapsedTimer joinWait;
                joinWait.start();
                while (!reclaimReturned.load(std::memory_order_acquire) && joinWait.elapsed() < 5000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            shutdownThread.join();

            const bool shutdownOk = shutdownOkFlag.load(std::memory_order_acquire);
            const int liveGuardsAtShutdownReturn = liveGuardsAtReturn.load(std::memory_order_acquire);
            const bool workerRunningAtShutdownReturn = workerRunningAtReturn.load(std::memory_order_acquire);

            ++completedRounds;
            // 正向前提：进入过「移交指针 + 登记借用」的临界区（证明被测路径确实执行），
            // **并且** shutdown 到达时回收调用确定尚未返回（否则本轮没测到那个窗口）。
            if (inCriticalSectionSeen.load() > 0 && arrivedWhileBorrowActive) {
                ++windowHits;
            }
            // **本用例只对「成功关闭路径」取证**（第六段复核 P2，第 17.4 节更正）。
            //
            // 既定契约里 shutdown() 返回 false 只表示「非 Idle：已请求停止，未销毁任何对象」，
            // 这种情形下**允许**仍有回收借用和仍在跑的 worker——它们本来就该留着。
            // 因此「shutdown() 返回 false 时仍有借用」根本不是缺陷，把它算作缺陷等于把
            // false 的语义扩大成「也已安全关闭」。上一版判据正是这样写的，已更正。
            //
            // 现在只在 shutdownOk 为真时判定，并把前提前移：本用例必须构造出
            // 「成功关闭 + 回收仍活跃」这一组合，否则说明前置条件没搭好（或服务非 Idle），
            // 与产品行为无关。用例里首启不入队任何 job、服务处于 Idle，所以正常实现
            // 必然走成功关闭路径。
            if (shutdownOk) {
                ++shutdownOkRounds;
                if (liveGuardsAtShutdownReturn > 0 || workerRunningAtShutdownReturn) {
                    ++earlyDeletes;
                    qInfo("[P1-BORROW] round %d: shutdown() reported success (ok=1) while %d reclaim guard(s) were "
                          "still in scope and workerRunning=%d",
                          round, liveGuardsAtShutdownReturn, workerRunningAtShutdownReturn ? 1 : 0);
                }
            } else {
                qInfo("[P1-BORROW] round %d: shutdown() returned false (not the success path this case asserts on, "
                      "borrows are permitted to be live here): liveGuards=%d workerRunning=%d",
                      round, liveGuardsAtShutdownReturn, workerRunningAtShutdownReturn ? 1 : 0);
            }

            // 收尾：放行屏障并 join starter，为下一轮准备。
            holdBarrier.store(false, std::memory_order_release);
            releaseBarrier.release();
            if (starter.joinable()) {
                starter.join();
            }
            releaseBarrier.tryAcquire(1, 2000);

            // 借用最终必须全部归还（不含 shutdown 期间的短暂持有）。
            QElapsedTimer drain;
            drain.start();
            while (FlowExecutionServiceTestAccess(service).activeReclaimBorrowCountForTest() > 0 &&
                   drain.elapsed() < 3000) {
                QThread::msleep(1);
            }

            QElapsedTimer settle;
            settle.start();
            while (service.isActive() && settle.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
        }

        if (starter.joinable()) {
            starter.join();
        }
        QObject::disconnect(failConn);
        FlowExecutionServiceTestAccess(service).clearWorkerLoopFaultHookForTest();
        FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();

        qInfo("[P1-BORROW] rounds=%d completed=%d windowHits=%d earlyDeletes=%d shutdownOkRounds=%d", kRounds,
              completedRounds.load(), windowHits.load(), earlyDeletes.load(), shutdownOkRounds.load());

        // 正向前提：确实进入了「借用已登记、仍在同一临界区」的位置。
        // 缺这一条，earlyDeletes==0 可能只是「没测到」而不是「没缺陷」。
        QVERIFY2(windowHits.load() >= 1,
                 qPrintable(QStringLiteral("the case must observe the worker being moved into m_inflightReclaim "
                                           "with its reclaim borrow registered in the same critical section "
                                           "(windowHits=%1, rounds=%2)")
                                .arg(windowHits.load())
                                .arg(kRounds)));

        // 正向前提②（第六段复核 P2）：本用例只对**成功关闭路径**取证。
        //
        // shutdown() 返回 false 是「非 Idle：只请求停止」的既定语义，此时不销毁任何对象，
        // 借用与 worker 本来就该留着。所以只有在真正进入销毁路径的那一轮，
        // 「返回时仍有借用」才构成缺陷。缺这一条，earlyDeletes==0 也可能只是
        // 「一次都没走过成功关闭路径」。
        QVERIFY2(shutdownOkRounds.load() >= 1,
                 qPrintable(QStringLiteral("the case must exercise the successful shutdown path (shutdown() == true) "
                                           "at least once before its wait-for-borrow assertion means anything "
                                           "(shutdownOkRounds=%1, rounds=%2, completed=%3)")
                                .arg(shutdownOkRounds.load())
                                .arg(kRounds)
                                .arg(completedRounds.load())));

        // 核心判据①：不得存在「线程已在 m_inflightReclaim、借用却为 0」的瞬间。
        // 这是「借用登记与指针移交同临界区」的直接判据，不依赖任何时序窗口。
        QVERIFY2(invariantViolations.load() == 0,
                 qPrintable(QStringLiteral("no worker may sit in m_inflightReclaim without a registered reclaim "
                                           "borrow (violations=%1): the borrow must be registered in the same "
                                           "critical section that moves the worker out of m_thread")
                                .arg(invariantViolations.load())));

        // 核心判据②：shutdown() **返回 true** 的那一刻，活跃回收借用必须已经归零，
        // 且被借用的 worker 不得仍在运行。
        //
        // 判据只挂在成功关闭路径上（见上一条正向前提）：false 分支按契约允许两者都还在。
        QVERIFY2(earlyDeletes.load() == 0,
                 qPrintable(QStringLiteral("a successful shutdown() must wait for active reclaim borrows to be "
                                           "returned before returning (earlyDeletes=%1/%2, shutdownOkRounds=%3): a "
                                           "unique deleter is not the same as no remaining users")
                                .arg(earlyDeletes.load())
                                .arg(windowHits.load())
                                .arg(shutdownOkRounds.load())));

        guard.finish();
    }

    // T2.5 P1 第八次整改（2026-11-12 第六段复核 P1）：**关闭期间不得启动新任务**。
    //
    // 缺陷机理（复核实测 3/3）：
    //   shutdown() 的 Idle 路径置 m_quitRequested = true，但 enqueue() 的最终入队
    //   会调 ensureThreadLocked()，那里把 m_quitRequested **复位成 false**（那是为
    //   「shutdown 后可重新启动」准备的）。于是关闭期间到达的启动请求照样重建 worker
    //   并执行任务：shutdown=true 但新任务在跑、引擎仍忙。
    //   另一道 m_reclaimAborted 只在 detachLiveWorkerThreadsLocked() 发现**存在在途
    //   回收**时才置位，普通 Idle 关闭根本不置位，入口检查形同虚设。
    //
    // 本用例不依赖任何时序窗口：用回收等待钩子把 shutdown() 停在
    // 「关闭标记已置位、worker 尚未 join」的中间段，此时从主线程发起一次启动，
    // 断言它被**明确拒绝**且模块一次都没执行。
    //
    // 与第 17 章的「非 Idle 早退」区分：本用例的服务在 shutdown 期间处于 Idle
    // （没有在途 job），所以走的是**真正的关闭路径**，判据不与 false 语义混淆。
    void testStartDuringShutdownMustBeRejected() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe(QStringLiteral("ShutdownWindowProbe"));
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ShutdownWindowProbe"),
                                                                      ExecutionAffinity::Worker);

        constexpr int kRounds = 3;

        std::atomic_int rejectedRounds{0};     // 关闭期间启动被拒绝的次数（正向前提）
        std::atomic_int windowsEntered{0};     // 成功把 shutdown 停在关闭段中间的次数
        std::atomic_int startedDuringClose{0}; // 关闭期间启动被接受（缺陷）
        std::atomic_int runsDuringClose{0};    // 关闭期间模块真的执行了（缺陷，更直接）
        std::atomic_int shutdownOkRounds{0};

        // 停靠点用 **worker 退出屏障**，不用回收等待钩子。
        //
        // 理由（第一版实测 windowsEntered=0）：Idle 关闭路径下 m_thread 直接进 retiring，
        // 根本不经过 retireWorkerThreadBounded()，回收钩子不会触发，用例就永远等不到
        // 「关闭段中间」这个状态。退出屏障则位于 WorkerThread::run() 末尾
        // （workerLoop() 已返回、线程尚未结束），Idle 关闭必须经 joinWorkerThread()
        // 等它——因此它一定能停住 shutdown()，且屏障等待不持 m_mutex。
        QSemaphore workerHeldAtExit;
        QSemaphore releaseWorkerExit;
        std::atomic_bool holdWorkerExit{false};
        std::atomic_int barrierEnters{0};

        FlowExecutionServiceTestAccess(service).setWorkerExitBarrierForTest([&] {
            if (!holdWorkerExit.load(std::memory_order_acquire)) {
                return;
            }
            ++barrierEnters;
            workerHeldAtExit.release();
            // 有界：任何失败路径下 worker 都能自行退出，不会让用例挂死。
            releaseWorkerExit.tryAcquire(1, 10000);
        });

        std::thread shutdownThread;
        std::atomic_bool shutdownDone{false};
        std::atomic_bool shutdownOkFlag{false};

        ServiceContractCleanupGuard guard{
            &service,
            QMetaObject::Connection(),
            [&] {
                holdWorkerExit.store(false, std::memory_order_release);
                releaseWorkerExit.release();
            },
            [&] { FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest(); },
            &shutdownThread,
            true};

        for (int round = 0; round < kRounds; ++round) {
            QElapsedTimer idle;
            idle.start();
            while (service.isActive() && idle.elapsed() < 1500) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
            if (service.isActive()) {
                break;
            }

            // 先建立一个常驻 worker：跑一次正常 job 并等它回到 Idle。
            // 这样 shutdown() 的 Idle 路径才有 worker 要 join（也就有退出屏障可停）。
            const int runsBefore = probe.runs.load();
            {
                QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
                QVERIFY(service.startSingle());
                QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
                QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Idle, 5000);
            }
            QCOMPARE(probe.runs.load(), runsBefore + 1);

            shutdownDone.store(false, std::memory_order_release);
            shutdownOkFlag.store(false, std::memory_order_release);
            holdWorkerExit.store(true, std::memory_order_release);
            if (shutdownThread.joinable()) {
                shutdownThread.join();
            }
            shutdownThread = std::thread([&] {
                const bool ok = service.shutdown();
                shutdownOkFlag.store(ok, std::memory_order_release);
                shutdownDone.store(true, std::memory_order_release);
            });

            // 等待「关闭已开始且 worker 被停在退出屏障上」这一确定性状态。
            // 等不到说明本轮没有关闭段，不计入判别样本（不制造假阴性）。
            const bool heldInClose = workerHeldAtExit.tryAcquire(1, 3000);
            if (heldInClose) {
                ++windowsEntered;
            }

            // **核心动作**：此刻 shutdown() 正在执行（标记已置位、worker 尚未销毁），
            // 从主线程发起一次启动。约定行为：明确拒绝，绝不执行。
            if (heldInClose) {
                const int runsAtProbe = probe.runs.load();
                const bool accepted = service.startSingle();
                const bool engineBusy = service.isActive();
                // 给可能的执行留一点时间：若被错误接受，模块一定有执行机会。
                // 这不是「撞窗口」——拒绝路径下这里什么都不会发生。
                QElapsedTimer grace;
                grace.start();
                while (grace.elapsed() < 150 && engineBusy) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(1);
                }
                if (accepted) {
                    ++startedDuringClose;
                } else {
                    ++rejectedRounds;
                }
                if (probe.runs.load() != runsAtProbe) {
                    ++runsDuringClose;
                }
                qInfo("[P1-SHUTDOWN-START] round %d: accepted=%d engineBusy=%d runsDelta=%d", round, accepted ? 1 : 0,
                      service.isActive() ? 1 : 0, probe.runs.load() - runsAtProbe);
            }

            // 无论上面结果如何，都要放行 worker 并收敛 shutdown。
            holdWorkerExit.store(false, std::memory_order_release);
            releaseWorkerExit.release();
            {
                QElapsedTimer waitDone;
                waitDone.start();
                while (!shutdownDone.load(std::memory_order_acquire) && waitDone.elapsed() < 15000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (shutdownThread.joinable()) {
                shutdownThread.join();
            }
            if (shutdownOkFlag.load(std::memory_order_acquire)) {
                ++shutdownOkRounds;
            }

            QElapsedTimer settle;
            settle.start();
            while (service.isActive() && settle.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
        }

        if (shutdownThread.joinable()) {
            shutdownThread.join();
        }
        FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();

        qInfo("[P1-SHUTDOWN-START] rounds=%d windowsEntered=%d rejected=%d accepted=%d runsDuringClose=%d "
              "shutdownOkRounds=%d",
              kRounds, windowsEntered.load(), rejectedRounds.load(), startedDuringClose.load(), runsDuringClose.load(),
              shutdownOkRounds.load());

        // 正向前提：确实把 shutdown() 停在了关闭段中间，且启动请求确实是在那时发出的。
        QVERIFY2(windowsEntered.load() >= 1,
                 qPrintable(QStringLiteral("the case must catch shutdown() inside its close section before it "
                                           "returns (windowsEntered=%1, rounds=%2)")
                                .arg(windowsEntered.load())
                                .arg(kRounds)));

        // 核心判据①：关闭期间不得接受任何新启动。
        QVERIFY2(startedDuringClose.load() == 0,
                 qPrintable(QStringLiteral("startSingle() must be rejected while shutdown() is in progress "
                                           "(accepted=%1/%2): clearing m_quitRequested in ensureThreadLocked() "
                                           "lets a closing service run a brand-new job")
                                .arg(startedDuringClose.load())
                                .arg(windowsEntered.load())));

        // 核心判据②（更直接）：关闭期间模块一次都不能执行。
        QVERIFY2(runsDuringClose.load() == 0,
                 qPrintable(QStringLiteral("no module may run while shutdown() is in progress (runs=%1/%2)")
                                .arg(runsDuringClose.load())
                                .arg(windowsEntered.load())));

        guard.finish();
    }
    // T2.5 P1 第九次整改（2026-11-12 第八段复核 P1）：**第二次 shutdown() 不得提前
    // 解除第一次关闭的保护**。
    //
    // 缺陷机理（复核独立探针 3/3）：shutdown() 入口只置 m_shutdownInProgress=true，
    // 没有检查是否已有一次关闭持有者。第一次关闭者正持有旧 worker 等在
    // joinWorkerThread() 的 thread->wait() 里时，第二次调用看到 m_thread 已被摘走、
    // retiring/inflight 均为空，便一路走到成功结尾：返回 true、清掉标记、报告
    // 「清理已完成」。随后新启动被接受并真正执行，而第一次关闭者仍持旧 worker。
    //
    // 契约：**关闭权是排他的**。重入调用既不得清除原关闭者的标记，也不得报告完成；
    // 只有取得关闭权的那次调用可以在收尾后清除标记并恢复服务。
    //
    // 本用例用 worker 退出屏障把第一次 shutdown() 确定性停在关闭段中间，再叠加第二次
    // shutdown() + 一次启动请求，断言：第二次不得返回 true、启动必须被拒绝、模块不得
    // 执行。判据只依赖屏障这一确定性停靠点，不靠 sleep 撞时序窗口。
    void testSecondShutdownMustNotReleaseFirstShutdownGuard() {
        RunEngine& engine = RunEngine::instance();
        FlowExecutionService& service = FlowExecutionService::instance();
        ServiceProbeModule probe(QStringLiteral("ShutdownOwnerProbe"));
        QVERIFY(probe.initialize());
        engine.addModule(&probe);
        RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("ShutdownOwnerProbe"),
                                                                      ExecutionAffinity::Worker);

        constexpr int kRounds = 3;
        std::atomic_int windowsEntered{0};   // 第一次 shutdown 被停在关闭段中间的次数
        std::atomic_int secondShutdownOk{0}; // 第二次 shutdown 谎报完成（缺陷）
        std::atomic_int restartAccepted{0};  // 保护被提前解除后启动被接受（缺陷）
        std::atomic_int runsDuringGuard{0};  // 关闭期间模块真的执行（缺陷，更直接）
        std::atomic_int firstShutdownOkRounds{0};

        QSemaphore workerHeldAtExit;
        QSemaphore releaseWorkerExit;
        std::atomic_bool holdWorkerExit{false};

        FlowExecutionServiceTestAccess(service).setWorkerExitBarrierForTest([&] {
            if (!holdWorkerExit.load(std::memory_order_acquire)) {
                return;
            }
            workerHeldAtExit.release();
            // 有界：任何失败路径下 worker 都能自行退出，不会让用例挂死。
            releaseWorkerExit.tryAcquire(1, 10000);
        });

        std::thread shutdownThread;
        std::atomic_bool shutdownDone{false};
        std::atomic_bool shutdownOkFlag{false};

        ServiceContractCleanupGuard guard{
            &service,
            QMetaObject::Connection(),
            [&] {
                holdWorkerExit.store(false, std::memory_order_release);
                releaseWorkerExit.release();
            },
            [&] { FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest(); },
            &shutdownThread,
            true};

        for (int round = 0; round < kRounds; ++round) {
            // 每轮从 Idle 起，并先建立一个常驻 worker，使第一次 shutdown() 的 Idle
            // 路径确实有 worker 要 join（也就有退出屏障可停）。
            QElapsedTimer idle;
            idle.start();
            while (service.isActive() && idle.elapsed() < 1500) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
            if (service.isActive()) {
                break;
            }
            const int runsBefore = probe.runs.load();
            {
                QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
                QVERIFY(service.startSingle());
                QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
                QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Idle, 5000);
            }
            QCOMPARE(probe.runs.load(), runsBefore + 1);

            shutdownDone.store(false, std::memory_order_release);
            shutdownOkFlag.store(false, std::memory_order_release);
            holdWorkerExit.store(true, std::memory_order_release);
            if (shutdownThread.joinable()) {
                shutdownThread.join();
            }
            shutdownThread = std::thread([&] {
                const bool ok = service.shutdown();
                shutdownOkFlag.store(ok, std::memory_order_release);
                shutdownDone.store(true, std::memory_order_release);
            });

            // 第一次 shutdown 已进入关闭段中间：worker 停在退出屏障、等待 join。
            const bool heldInClose = workerHeldAtExit.tryAcquire(1, 3000);
            if (heldInClose) {
                ++windowsEntered;

                // **核心动作 1**：第一次关闭者仍持有旧 worker 时，叠加第二次 shutdown()。
                // 契约：它不是关闭权持有者，既不得清除保护，也不得报告清理完成。
                const bool secondOk = service.shutdown();
                if (secondOk) {
                    ++secondShutdownOk;
                }

                // **核心动作 2**：紧接着发起一次启动。若第二次 shutdown 提前清掉了
                // 保护，这次启动会被接受并真的执行模块。
                const int runsAtProbe = probe.runs.load();
                const bool accepted = service.startSingle();
                if (accepted) {
                    ++restartAccepted;
                }
                const bool engineBusy = service.isActive();
                QElapsedTimer grace;
                grace.start();
                while (grace.elapsed() < 150 && engineBusy) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(1);
                }
                if (probe.runs.load() != runsAtProbe) {
                    ++runsDuringGuard;
                }
                qInfo("[P1-SHUTDOWN-OWNER] round %d: secondShutdownOk=%d restartAccepted=%d runsDelta=%d", round,
                      secondOk ? 1 : 0, accepted ? 1 : 0, probe.runs.load() - runsAtProbe);
            }

            // 放行旧 worker，让第一次 shutdown 收敛；确认它最终真正完成。
            holdWorkerExit.store(false, std::memory_order_release);
            releaseWorkerExit.release();
            {
                QElapsedTimer waitDone;
                waitDone.start();
                while (!shutdownDone.load(std::memory_order_acquire) && waitDone.elapsed() < 15000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (shutdownThread.joinable()) {
                shutdownThread.join();
            }
            if (shutdownOkFlag.load(std::memory_order_acquire)) {
                ++firstShutdownOkRounds;
            }

            // 收尾：把缺陷态下可能已被接受并执行的 job 收敛掉，保证下一轮前置干净。
            service.stop();
            QElapsedTimer settle;
            settle.start();
            while (service.isActive() && settle.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
        }

        if (shutdownThread.joinable()) {
            shutdownThread.join();
        }
        FlowExecutionServiceTestAccess(service).clearWorkerExitBarrierForTest();

        // 正向前提：服务在关闭完成后恢复可用（防止修复把排他保护标记永久置位）。
        service.stop();
        {
            QElapsedTimer settle;
            settle.start();
            while (service.isActive() && settle.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
        }
        const bool restartAfterRelease = service.startSingle();
        QVERIFY2(restartAfterRelease, "the service must be usable again after the first shutdown completes "
                                      "(the exclusive shutdown guard must be released by its owner)");

        qInfo("[P1-SHUTDOWN-OWNER] rounds=%d windowsEntered=%d secondShutdownOk=%d restartAccepted=%d "
              "runsDuringGuard=%d firstShutdownOkRounds=%d",
              kRounds, windowsEntered.load(), secondShutdownOk.load(), restartAccepted.load(), runsDuringGuard.load(),
              firstShutdownOkRounds.load());

        QVERIFY2(windowsEntered.load() >= 1,
                 qPrintable(QStringLiteral("the case must catch the first shutdown inside its close section "
                                           "before it returns (windowsEntered=%1, rounds=%2)")
                                .arg(windowsEntered.load())
                                .arg(kRounds)));
        QVERIFY2(firstShutdownOkRounds.load() >= 1,
                 qPrintable(QStringLiteral("the first shutdown must eventually complete successfully "
                                           "(firstShutdownOkRounds=%1)")
                                .arg(firstShutdownOkRounds.load())));
        QVERIFY2(secondShutdownOk.load() == 0,
                 qPrintable(QStringLiteral("a reentrant shutdown() must not report completed cleanup while the "
                                           "first shutdown still owns the close (secondShutdownOk=%1/%2)")
                                .arg(secondShutdownOk.load())
                                .arg(windowsEntered.load())));
        QVERIFY2(restartAccepted.load() == 0,
                 qPrintable(QStringLiteral("a second shutdown must not release the first shutdown's guard "
                                           "(restartAccepted=%1/%2)")
                                .arg(restartAccepted.load())
                                .arg(windowsEntered.load())));
        QVERIFY2(runsDuringGuard.load() == 0,
                 qPrintable(QStringLiteral("no module may run while the first shutdown is still in progress "
                                           "(runs=%1/%2)")
                                .arg(runsDuringGuard.load())
                                .arg(windowsEntered.load())));

        guard.finish();
    }
};

QTEST_MAIN(TestFlowExecutionService)
#include "test_flowexecutionservice.moc"
