#define DEEPLUX_BUILD_TESTS 1
#include "core/base/ModuleBase.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "manager/ProjectManager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>
#include <atomic>
#include <chrono>
#include <thread>

using namespace DeepLux;

/**
 * 执行亲和性探针模块（后台执行设计 §6.1/§6.2/§6.3）。
 *
 * - threadSafe(true)：允许进入并行池，用于验证 worker 亲和的并行资格。
 * - entered：每次 process 释放一次，作为确定性屏障（QSemaphore）。
 * - executedOn：记录真正执行 process 的线程，用于断言分流落点。
 */
class AffinityProbeModule : public ModuleBase {
public:
    AffinityProbeModule() {
        m_moduleId = QStringLiteral("AffinityProbe");
        m_name = QStringLiteral("AffinityProbe");
        m_category = QStringLiteral("test");
        setThreadSafe(true);
    }
    std::atomic<Qt::HANDLE> executedOn{nullptr};
    QSemaphore entered;

protected:
    bool process(const ImageData& input, ImageData& output) override {
        executedOn.store(QThread::currentThreadId());
        entered.release();
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

/// T2.2：GUI 线程阻塞探针，用于验证回投超时的生命周期语义。
class BlockingGuiProbe : public ModuleBase {
public:
    BlockingGuiProbe() {
        m_moduleId = QStringLiteral("BlockingGuiProbe");
        m_name = QStringLiteral("BlockingGuiProbe");
        m_category = QStringLiteral("test");
    }
    QSemaphore entered;
    QSemaphore release;
    std::atomic<bool> started{false};
    std::atomic<int> runs{0};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        ++runs;
        started.store(true);
        entered.release();
        if (!release.tryAcquire(1, 5000)) {
            return false; // 测试失败时防止永久挂死
        }
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class TestExecutionAffinity : public QObject {
    Q_OBJECT
private slots:
    void init() {
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        RunEngine::instance().setGuiDispatchTimeoutForTest(5000);
    }

    void testGuiAffinityRunsOnGuiThread() {
        AffinityProbeModule module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("AffinityProbe"),
                                                                ExecutionAffinity::Gui);

        QThread worker;
        QObject context;
        context.moveToThread(&worker);
        QMetaObject::invokeMethod(
            &context, [&]() { RunEngine::instance().runOnce(); }, Qt::QueuedConnection);
        worker.start();

        // GUI 亲和模块必须回到 GUI 线程执行：主线程自旋处理事件以服务
        // BlockingQueuedConnection；屏障是信号量计数（available 轮询不消耗），超时仅防挂死。
        QTRY_VERIFY_WITH_TIMEOUT(module.entered.available() > 0, 2000);
        QVERIFY(module.entered.tryAcquire(1, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 2000);
        QCOMPARE(module.executedOn.load(), QThread::currentThreadId());
        worker.quit();
        QVERIFY(worker.wait(2000));
    }

    void testWorkerAffinityRunsOnCallingWorkerThread() {
        AffinityProbeModule module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("AffinityProbe"),
                                                                ExecutionAffinity::Worker);

        QThread worker;
        QObject context;
        context.moveToThread(&worker);
        QMetaObject::invokeMethod(
            &context, [&]() { RunEngine::instance().runOnce(); }, Qt::QueuedConnection);
        worker.start();

        // worker 亲和在调用线程内联执行，无需 GUI 事件循环：阻塞 tryAcquire 即确定性屏障。
        QVERIFY(module.entered.tryAcquire(1, 2000));
        QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 2000);
        QVERIFY(module.executedOn.load() != QThread::currentThreadId());
        worker.quit();
        QVERIFY(worker.wait(2000));
    }

    void testParallelRejectsGuiAffinityWithoutDeadlock() {
        AffinityProbeModule module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("AffinityProbe"),
                                                                ExecutionAffinity::Gui);

        ExecutionResult result;
        QSemaphore done;
        std::thread caller([&]() {
            result = RunEngine::instance().executeParallel({QStringLiteral("AffinityProbe")}, {});
            done.release();
        });
        QVERIFY(done.tryAcquire(1, 2000));
        caller.join();
        QCOMPARE(result.success, false);
        QVERIFY(result.errorCode == ExecError::Processing);
        QVERIFY(result.userMessage.contains(QStringLiteral("gui_affinity_not_parallelizable")));
    }

    void testWorkerAffinityIsParallelEligible() {
        AffinityProbeModule module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("AffinityProbe"),
                                                                ExecutionAffinity::Worker);
        QVERIFY(RunEngine::instance().isParallelEligibleForTest(QStringLiteral("AffinityProbe")));
    }

    void testGuiAffinityIsNotParallelEligible() {
        AffinityProbeModule module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("AffinityProbe"),
                                                                ExecutionAffinity::Gui);
        QVERIFY(!RunEngine::instance().isParallelEligibleForTest(QStringLiteral("AffinityProbe")));
    }

    // T2.2（NF）：GUI 回投超时后，尚未开始的迟到回调必须丢弃，模块不得执行。
    // 等待任务返回期间不得泵 GUI 事件（信号量等待），否则会提前放行回调。
    void testGuiDispatchTimeoutDropsLateCallback() {
        BlockingGuiProbe module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("BlockingGuiProbe"),
                                                                ExecutionAffinity::Gui);
        RunEngine::instance().setGuiDispatchTimeoutForTest(100);

        std::atomic<bool> runOnceReturned{false};
        QSemaphore taskDone;
        QThread worker;
        QObject context;
        context.moveToThread(&worker);
        QMetaObject::invokeMethod(
            &context,
            [&]() {
                RunEngine::instance().runOnce();
                runOnceReturned.store(true);
                taskDone.release();
            },
            Qt::QueuedConnection);
        worker.start();

        // 故意不处理 GUI 事件，让回投超时；忙等而非 qWait（qWait 会处理事件）。
        QElapsedTimer starve;
        starve.start();
        while (starve.elapsed() < 300) {
            std::this_thread::yield();
        }

        // 不泵事件地等待任务返回（有界）。
        const bool returned = taskDone.tryAcquire(1, 2000);

        // 先收尾再断言：放行探针、投递迟到回调、回收线程，失败路径也不遗留线程。
        module.release.release();
        QCoreApplication::processEvents();
        worker.quit();
        const bool joined = worker.wait(5000);

        const int runs = module.runs.load();
        const QString code = RunEngine::instance().lastGuiDispatchErrorCodeForTest();
        const int drops = RunEngine::instance().guiDispatchLateCallbackDropCountForTest();

        QVERIFY2(returned, "runOnce must return after the dispatch timeout");
        QVERIFY2(joined, "worker thread must be reclaimed");
        QCOMPARE(runs, 0);
        QCOMPARE(code, QStringLiteral("affinity_dispatch_timeout"));
        QVERIFY2(drops >= 1, "late callback must be dropped without executing the module");
    }

    // T2.2（NF）：回调已 Running 时超时，runOnce 必须等其真正完成才返回。
    // 控制线程以“回投超时命中计数”为屏障（非固定 sleep），再检查状态并放行。
    void testGuiDispatchTimeoutWhileRunningWaitsForCompletion() {
        BlockingGuiProbe module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("BlockingGuiProbe"),
                                                                ExecutionAffinity::Gui);
        RunEngine::instance().setGuiDispatchTimeoutForTest(100);

        std::atomic<bool> runOnceReturned{false};
        std::atomic<bool> returnedBeforeRelease{true};
        std::atomic<bool> rerunRejected{false};
        std::atomic<bool> rerunAccepted{false};
        QSemaphore taskDone;
        QThread worker;
        QObject context;
        context.moveToThread(&worker);
        QMetaObject::invokeMethod(
            &context,
            [&]() {
                RunEngine::instance().runOnce();
                runOnceReturned.store(true);
                taskDone.release();
            },
            Qt::QueuedConnection);
        worker.start();

        // 独立控制线程：以超时命中为屏障（有界），再用“任务完成信号量的有界窗口”
        // 判定 runOnce 是否在回调仍阻塞时提前返回；随后检查重跑并放行。
        std::thread control([&]() {
            QElapsedTimer waitStart;
            waitStart.start();
            while (RunEngine::instance().guiDispatchTimeoutHitCountForTest() < 1 && waitStart.elapsed() < 3000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            returnedBeforeRelease.store(taskDone.tryAcquire(1, 1000));
            const bool accepted = RunEngine::instance().runOnceAsync();
            rerunAccepted.store(accepted);
            rerunRejected.store(!accepted);
            module.release.release();
        });

        // 主线程泵事件直到回调开始（有界）；回调阻塞期间本次 processEvents 不会返回。
        QElapsedTimer pumpStart;
        pumpStart.start();
        while (!module.started.load() && pumpStart.elapsed() < 3000) {
            QCoreApplication::processEvents();
            std::this_thread::yield();
        }
        control.join();

        // 先收尾再断言：放行、回收线程、必要时停止被意外接受的重跑任务。
        module.release.release();
        worker.quit();
        const bool joined = worker.wait(5000);
        if (rerunAccepted.load()) {
            // 缺陷变体：重跑被意外接受时必须停止并等其退出，避免后台任务引用栈上探针。
            RunEngine::instance().stop();
            QTRY_VERIFY_WITH_TIMEOUT(!RunEngine::instance().isBusy(), 3000);
        }

        const bool started = module.started.load();
        const int runs = module.runs.load();
        const QString code = RunEngine::instance().lastGuiDispatchErrorCodeForTest();
        const int runningWaitHits = RunEngine::instance().guiDispatchRunningWaitEnteredCountForTest();

        QVERIFY2(started, "GUI callback must start");
        QVERIFY2(joined, "worker must finish after the running callback returns");
        QVERIFY2(!returnedBeforeRelease.load(), "runOnce must not return while the GUI callback is still running");
        QVERIFY2(runningWaitHits >= 1, "the timeout path must enter the Running-callback wait");
        QVERIFY2(rerunRejected.load(), "a second run must be rejected while the callback is running");
        QCOMPARE(runs, 1);
        QCOMPARE(code, QStringLiteral("affinity_dispatch_timeout"));
    }
};

QTEST_MAIN(TestExecutionAffinity)
#include "test_executionaffinity.moc"
