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
    std::atomic<int> runs{0};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        ++runs;
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
    void testGuiDispatchTimeoutDropsLateCallback() {
        BlockingGuiProbe module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("BlockingGuiProbe"),
                                                                ExecutionAffinity::Gui);
        RunEngine::instance().setGuiDispatchTimeoutForTest(100);

        QThread worker;
        QObject context;
        context.moveToThread(&worker);
        QMetaObject::invokeMethod(
            &context, [&]() { RunEngine::instance().runOnce(); }, Qt::QueuedConnection);
        worker.start();

        // 故意不处理 GUI 事件，让回投超时；忙等而非 qWait（qWait 会处理事件）。
        QElapsedTimer starve;
        starve.start();
        while (starve.elapsed() < 300) {
            std::this_thread::yield();
        }
        worker.quit();
        if (!worker.wait(5000)) {
            // 旧实现会阻塞在 BlockingQueuedConnection：先放行迟到回调再失败，避免悬挂指针。
            QCoreApplication::processEvents();
            QFAIL("runOnce must return after the dispatch timeout");
        }

        // 现在处理事件：迟到回调执行但必须 no-op。
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(RunEngine::instance().guiDispatchLateCallbackDropCountForTest() >= 1, 2000);
        QCOMPARE(module.runs.load(), 0);
        QCOMPARE(RunEngine::instance().lastGuiDispatchErrorCodeForTest(), QStringLiteral("affinity_dispatch_timeout"));
    }

    // T2.2（NF）：回调已 Running 时超时，必须等其真正完成，不得提前复用模块。
    void testGuiDispatchTimeoutWhileRunningWaitsForCompletion() {
        BlockingGuiProbe module;
        QVERIFY(module.initialize());
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(QStringLiteral("BlockingGuiProbe"),
                                                                ExecutionAffinity::Gui);
        RunEngine::instance().setGuiDispatchTimeoutForTest(100);

        QThread worker;
        QObject context;
        context.moveToThread(&worker);
        QMetaObject::invokeMethod(
            &context, [&]() { RunEngine::instance().runOnce(); }, Qt::QueuedConnection);
        worker.start();

        // 处理事件直到回调开始执行（模块进入 process 并阻塞）。
        QTRY_VERIFY_WITH_TIMEOUT(module.entered.available() > 0, 2000);
        QVERIFY(module.entered.tryAcquire(1, 0));

        // 停止处理事件，等回投超时（此时回调已 Running）。
        QElapsedTimer starve;
        starve.start();
        while (starve.elapsed() < 300) {
            std::this_thread::yield();
        }
        QVERIFY2(!worker.isFinished(), "worker must still be waiting for the running callback");

        module.release.release();
        worker.quit();
        QVERIFY2(worker.wait(5000), "worker must finish after the running callback returns");
        QCOMPARE(module.runs.load(), 1);
        QCOMPARE(RunEngine::instance().lastGuiDispatchErrorCodeForTest(), QStringLiteral("affinity_dispatch_timeout"));
    }
};

QTEST_MAIN(TestExecutionAffinity)
#include "test_executionaffinity.moc"
