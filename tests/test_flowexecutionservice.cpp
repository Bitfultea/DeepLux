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
            RunEngine::instance().setModuleExecutionAffinityForTest(name, ExecutionAffinity::Worker);
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
        service->clearWorkerExitBarrierForTest();
        service->setShutdownDeadlineForTest(originalDeadlineMs);
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
        engine.setModuleExecutionAffinityForTest(QStringLiteral("DeadlineProbe"), ExecutionAffinity::Worker);

        // 先跑一次，确保常驻 worker 已创建并回到空闲等待。
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QVERIFY(service.startSingle());
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Idle, 5000);

        const int baselineHits = service.shutdownDeadlineHitCountForTest();
        const int originalDeadlineMs = 10000;
        service.setShutdownDeadlineForTest(0);

        QSemaphore entered; // worker 已到达退出屏障
        QSemaphore release; // 独立控制线程放行
        std::atomic<bool> released{false};
        std::atomic<int> workerRunFinished{0};     // 屏障返回、worker 即将真正退出
        std::atomic<bool> shutdownReturned{false}; // 主线程在 shutdown() 返回后置位
        std::atomic<int> returnedBeforeRelease{-1};
        std::atomic<int> workerFinishedBeforeRelease{-1};
        std::atomic<int> workerAtBarrier{-1};

        service.setWorkerExitBarrierForTest([&]() {
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
            while (service.shutdownDeadlineHitCountForTest() <= baselineHits && poll.elapsed() < 2000) {
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
        service.clearWorkerExitBarrierForTest();
        service.setShutdownDeadlineForTest(originalDeadlineMs);

        // —— 断言（顺序即 NF 判据顺序）——
        QVERIFY2(shutdownResult, "idle shutdown must join the worker and return true");
        QVERIFY2(elapsedMs < 5000, "shutdown must not hang waiting for the worker");
        QCOMPARE(workerAtBarrier.load(std::memory_order_acquire), 1);
        QVERIFY2(service.shutdownDeadlineHitCountForTest() >= baselineHits + 1,
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
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
        QSignalSpy failedSpy(&service, &FlowExecutionService::jobFailed);
        QVERIFY2(failedSpy.isValid(), "jobFailed must be observable");

        service.setStartHandshakeHookForTest([] { throw std::runtime_error("injected startup callback fault"); });

        // 失败路径收尾：清除捕获栈变量的钩子并 join worker。必须最后声明。
        ServiceContractCleanupGuard guard{&service, {},  nullptr, [&] { service.clearStartHandshakeHookForTest(); },
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
        service.clearStartHandshakeHookForTest();
        QSignalSpy nextFinished(&service, &FlowExecutionService::jobFinished);
        QVERIFY2(service.startSingle(), "the service must remain usable after a startup fault");
        QTRY_COMPARE_WITH_TIMEOUT(nextFinished.count(), 1, 5000);
        QCOMPARE(probe.runs.load(), 1);
        QCOMPARE(service.state(), FlowExecutionService::State::Idle);

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
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

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
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

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
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        QSemaphore entered;
        QSemaphore release;
        QSemaphore completed;
        QSemaphore startReturned;
        std::atomic<int> startResult{-1};
        std::atomic<int> hookRuns{0};
        std::thread starter;
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);

        service.setStartHandshakeHookForTest([&] {
            ++hookRuns;
            entered.release();   // 进入：worker 已出队、处于启动协议内
            release.acquire();   // 放行：由测试线程在握手超时返回后释放
            completed.release(); // 完成：钩子已离开
        });

        // 失败路径收尾：先放行屏障、清钩子、join starter，再 stop/join worker。必须最后声明。
        ServiceContractCleanupGuard guard{&service,
                                          {},
                                          [&] {
                                              release.release();
                                              startReturned.release();
                                          },
                                          [&] { service.clearStartHandshakeHookForTest(); },
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
        service.clearStartHandshakeHookForTest();
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
        engine.setModuleExecutionAffinityForTest(QStringLiteral("ServiceProbe"), ExecutionAffinity::Worker);

        QSemaphore entered;
        QSemaphore release;
        QSemaphore completed;
        QSemaphore startReturned;
        std::atomic<int> startResult{-1};
        std::thread starter;
        QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);

        service.setStartHandshakeHookForTest([&] {
            entered.release();
            release.acquire();
            completed.release();
        });

        ServiceContractCleanupGuard guard{&service,
                                          {},
                                          [&] {
                                              release.release();
                                              startReturned.release();
                                          },
                                          [&] { service.clearStartHandshakeHookForTest(); },
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
        service.clearStartHandshakeHookForTest();
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
};

QTEST_MAIN(TestFlowExecutionService)
#include "test_flowexecutionservice.moc"
