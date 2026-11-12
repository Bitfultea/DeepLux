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
#include <stdexcept>
#include <thread>

using namespace DeepLux;

/**
 * 执行收尾探针（后台执行设计 §5.3）：
 * - BlockingProbe：进入 process 后阻塞，直到测试释放 —— 用于把 stop() 精确落在
 *   收尾窗口（模块已返回、finalizer 尚未提交终态）。
 * - CountingProbe：非阻塞、每次执行释放一次信号量并计数 —— 用于证明原生 start()
 *   循环至少执行两轮（每轮 Completed 后必须保持 Running，后续 tick 才被接受）。
 * - LogProbe：记录执行顺序 —— 用于证明 Paused 收尾保留续跑上下文。
 */
class BlockingProbe : public ModuleBase {
public:
    BlockingProbe() {
        m_moduleId = QStringLiteral("BlockingProbe");
        m_name = QStringLiteral("BlockingProbe");
        m_category = QStringLiteral("test");
    }
    QSemaphore entered;
    QSemaphore release;

protected:
    bool process(const ImageData& input, ImageData& output) override {
        entered.release();
        release.acquire();
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class CountingProbe : public ModuleBase {
public:
    CountingProbe() {
        m_moduleId = QStringLiteral("CountingProbe");
        m_name = QStringLiteral("CountingProbe");
        m_category = QStringLiteral("test");
    }
    QSemaphore entered;
    std::atomic<int> calls{0};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        ++calls;
        entered.release();
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class LogProbe : public ModuleBase {
public:
    LogProbe(const QString& name, QStringList* log) : m_log(log) {
        m_moduleId = QStringLiteral("com.deeplux.test.log.") + name;
        m_name = name;
        m_category = QStringLiteral("test");
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
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

class TestRunEngineFinalizer : public QObject {
    Q_OBJECT
private slots:
    void init() {
        RunEngine::instance().stop();
        // 清除故障注入/收尾钩子，避免失败用例把指向栈对象的回调留给后续用例。
        RunEngineTestAccess(RunEngine::instance()).clearFinalizeHookForTest();
        RunEngineTestAccess(RunEngine::instance()).clearExecutionFaultInjectionForTest();
        RunEngine::instance().clearModules();
    }

    // 设计 §5.3 关键语义 6：原生 start() 循环每轮 Completed 后必须保持 Running，
    // 否则后续 tick 会被 tryBeginForRun(CycleTick) 拒绝（只执行一轮）。
    void testNativeCycleKeepsRunningAfterRound() {
        CountingProbe probe;
        QVERIFY(probe.initialize());
        RunEngine::instance().addModule(&probe);
        RunEngine::instance().setParallelThreadCount(1);

        RunEngine::instance().start();
        // 主线程自旋处理事件循环，原生定时器才会 tick；屏障是执行计数（信号量），
        // 超时仅防挂死。
        QTRY_VERIFY_WITH_TIMEOUT(probe.entered.available() >= 2, 3000);
        QVERIFY(probe.entered.tryAcquire(2, 0));
        QCOMPARE(RunEngine::instance().state(), RunState::Running);

        RunEngine::instance().stop();
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 2000);
        QCOMPARE(RunEngine::instance().state(), RunState::Stopped);
    }

    // 设计 §5.3 关键语义 6：收尾窗口内并发 stop 必须获胜（终态 Stopped，
    // 不得被 Completed 覆盖为 Running/Idle）。
    void testStopWinsInFinalizeWindow() {
        BlockingProbe probe;
        QVERIFY(probe.initialize());
        RunEngine::instance().addModule(&probe);
        // 探针未注册插件（默认 gui），此处走后台线程执行，标记 worker 以保持内联语义。
        RunEngineTestAccess(RunEngine::instance())
            .setModuleExecutionAffinityForTest(QStringLiteral("BlockingProbe"), ExecutionAffinity::Worker);
        RunEngineTestAccess(RunEngine::instance()).setFinalizeHookForTest([]() { RunEngine::instance().stop(); });

        std::thread runner([]() { RunEngine::instance().runOnce(); });
        QVERIFY(probe.entered.tryAcquire(1, 2000));
        probe.release.release();
        runner.join();

        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 2000);
        QCOMPARE(RunEngine::instance().state(), RunState::Stopped);
        RunEngineTestAccess(RunEngine::instance()).clearFinalizeHookForTest();
    }

    // 设计 §5.3 关键语义 6：Paused 必须显式提交——只释放本段执行权、不发 runFinished、
    // 保留续跑上下文（暂停模块/流水线），继续后暂停模块恰好执行一次。
    void testPausedFinalizeKeepsResumeContext() {
        RunEngine& engine = RunEngine::instance();
        Project project;
        ModuleInstance first;
        first.id = QStringLiteral("A");
        first.moduleId = QStringLiteral("A");
        first.breakpoint = true;
        ModuleInstance second;
        second.id = QStringLiteral("B");
        second.moduleId = QStringLiteral("B");
        project.addModule(first);
        project.addModule(second);
        ModuleConnection control;
        control.fromModuleId = QStringLiteral("A");
        control.toModuleId = QStringLiteral("B");
        control.fromPort = QStringLiteral("next");
        control.toPort = QStringLiteral("control");
        control.edgeType = QStringLiteral("control");
        project.addConnection(control);

        QStringList log;
        QVERIFY(engine.loadProject(
            &project, [&log](const ModuleInstance& inst) -> ModuleBase* { return new LogProbe(inst.id, &log); }));

        QSignalSpy finishedSpy(&engine, &RunEngine::runFinished);
        engine.runOnce();

        QVERIFY(engine.isPausedAtBreakpoint());
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(finishedSpy.count(), 0);
        QVERIFY(log.isEmpty()); // 断点暂停发生在模块执行前

        engine.resume();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 2000);
        QCOMPARE(log, QStringList({QStringLiteral("A"), QStringLiteral("B")}));
        QCOMPARE(engine.state(), RunState::Idle);
    }

    // 设计 §5.3 关键语义 6：引擎层故障注入（executeAcquiredRun 内部抛异常，而非仅插件
    // process()）必须由 RAII finalizer 释放租约、状态回落，后续 run 仍可启动。
    void testExceptionReleasesLeaseAndAllowsNextRun() {
        RunEngine& engine = RunEngine::instance();
        CountingProbe probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);

        RunEngineTestAccess(engine).setExecutionFaultInjectionForTest(
            []() { throw std::runtime_error("injected engine fault"); });
        engine.runOnce();
        QVERIFY2(!engine.isBusy(), "exception must release the execution lease");
        QCOMPARE(engine.state(), RunState::Idle);
        QCOMPARE(probe.calls.load(), 0); // 注入点在模块执行前
        RunEngineTestAccess(engine).clearExecutionFaultInjectionForTest();

        QSignalSpy finishedSpy(&engine, &RunEngine::runFinished);
        engine.runOnce();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 2000);
        QCOMPARE(probe.calls.load(), 1);
        QCOMPARE(engine.state(), RunState::Idle);
    }
};

QTEST_MAIN(TestRunEngineFinalizer)
#include "test_runenginefinalizer.moc"
