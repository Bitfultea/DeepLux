#define DEEPLUX_BUILD_TESTS 1
#include "core/base/ModuleBase.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "manager/ProjectManager.h"

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

class TestExecutionAffinity : public QObject {
    Q_OBJECT
private slots:
    void init() {
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
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
        QMetaObject::invokeMethod(&context, [&]() { RunEngine::instance().runOnce(); }, Qt::QueuedConnection);
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
        QMetaObject::invokeMethod(&context, [&]() { RunEngine::instance().runOnce(); }, Qt::QueuedConnection);
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
};

QTEST_MAIN(TestExecutionAffinity)
#include "test_executionaffinity.moc"
