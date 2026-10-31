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

/**
 * job 租约探针模块（后台执行设计 §4.2/§5.3）：非阻塞，记录执行线程与次数。
 */
class LeaseProbeModule : public ModuleBase {
public:
    LeaseProbeModule() {
        m_moduleId = QStringLiteral("LeaseProbe");
        m_name = QStringLiteral("LeaseProbe");
        m_category = QStringLiteral("test");
        setThreadSafe(true);
    }
    std::atomic<Qt::HANDLE> executedOn{nullptr};
    QSemaphore entered;
    std::atomic<int> calls{0};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        executedOn.store(QThread::currentThreadId());
        ++calls;
        entered.release();
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class TestRunEngineJobLease : public QObject {
    Q_OBJECT
private slots:
    void init() {
        RunEngine::instance().stop();
        RunEngine::instance().clearBeforeJobStartHookForTest();
        RunEngine::instance().clearExecutionFaultInjectionForTest();
        RunEngine::instance().clearModules();
    }

    // B3.1 Step 1（计划用例）：所有者可在租约内执行单次与单步。
    void testOwnerCanRunSingleAndStepInsideLease() {
        LeaseProbeModule probe;
        QVERIFY(probe.initialize());
        RunEngine::instance().addModule(&probe);

        RunEngine::JobToken token;
        QString reason;
        QVERIFY2(RunEngine::instance().beginExternalJob(&token, &reason), qPrintable(reason));
        QVERIFY(token.isValid());
        QVERIFY(RunEngine::instance().runOnceForJob(token, &reason));
        QTRY_VERIFY(!RunEngine::instance().isExecuting());
        QCOMPARE(probe.calls.load(), 1);
        QVERIFY(RunEngine::instance().stepOnceForJob(token, &reason));
        QCOMPARE(probe.calls.load(), 2);
        QVERIFY(RunEngine::instance().endExternalJob(token));
        QVERIFY(!RunEngine::instance().isExternalJobActiveForTest());
    }

    // B3.1 Step 1（计划用例）：非所有者入口在租约期间一律拒绝。
    void testNonOwnerEntriesRejectedDuringLease() {
        LeaseProbeModule probe;
        QVERIFY(probe.initialize());
        RunEngine::instance().addModule(&probe);

        RunEngine::JobToken token;
        QVERIFY(RunEngine::instance().beginExternalJob(&token, nullptr));
        QString reason;
        QVERIFY(!RunEngine::instance().runOnceChecked(&reason));
        QCOMPARE(reason, QStringLiteral("external_job_active"));
        QVERIFY(!RunEngine::instance().stepOnce());
        QVERIFY(!RunEngine::instance().runOnceAsync());
        QVERIFY(!RunEngine::instance().loadProject(nullptr));
        RunEngine::instance().endExternalJob(token);
    }

    // 设计 §4.2：租约持有期间 engine 保持 busy，维护（增删/清空/加载）与公开并行原语拒绝。
    void testLeaseMarksBusyAndBlocksOwnerlessMaintenance() {
        LeaseProbeModule probe;
        QVERIFY(probe.initialize());
        RunEngine::instance().addModule(&probe);
        const int moduleCount = RunEngine::instance().modules().size();

        RunEngine::JobToken token;
        QVERIFY(RunEngine::instance().beginExternalJob(&token, nullptr));
        QVERIFY2(RunEngine::instance().isBusy(), "external job lease must keep the engine busy");
        QVERIFY(RunEngine::instance().isExternalJobActiveForTest());

        LeaseProbeModule extra;
        QVERIFY(extra.initialize());
        RunEngine::instance().addModule(&extra);
        QCOMPARE(RunEngine::instance().modules().size(), moduleCount);
        RunEngine::instance().removeModule(QStringLiteral("LeaseProbe"));
        QCOMPARE(RunEngine::instance().modules().size(), moduleCount);
        RunEngine::instance().clearModules();
        QCOMPARE(RunEngine::instance().modules().size(), moduleCount);
        QVERIFY(!RunEngine::instance().loadProject(nullptr));

        const ExecutionResult parallel = RunEngine::instance().executeParallel({QStringLiteral("LeaseProbe")}, {});
        QVERIFY2(!parallel.success, "public parallel primitive must be rejected while a job lease is active");
        QCOMPARE(probe.calls.load(), 0);

        QVERIFY(RunEngine::instance().endExternalJob(token));
        QVERIFY(!RunEngine::instance().isBusy());
    }

    // 设计 §4.2/§5.3：endExternalJob 只接受匹配 token；*ForJob 入口在无租约/owner 不匹配时拒绝。
    void testJobEntryRequiresMatchingOwnerAndLease() {
        RunEngine& engine = RunEngine::instance();
        LeaseProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);

        RunEngine::JobToken unbound;
        unbound.id = 999;
        unbound.salt = 7;
        QString reason;
        QVERIFY(!engine.runOnceForJob(unbound, &reason));
        QCOMPARE(reason, QStringLiteral("external_job_inactive"));

        RunEngine::JobToken token;
        QVERIFY(engine.beginExternalJob(&token, nullptr));
        QVERIFY(!engine.runOnceForJob(unbound, &reason));
        QCOMPARE(reason, QStringLiteral("external_job_owner_mismatch"));
        QVERIFY(!engine.stepOnceForJob(unbound, &reason));
        QCOMPARE(reason, QStringLiteral("external_job_owner_mismatch"));
        QVERIFY(!engine.resumeForJob(unbound, &reason));
        QCOMPARE(reason, QStringLiteral("external_job_owner_mismatch"));
        QVERIFY(!engine.pauseForJob(unbound));

        QVERIFY(!engine.endExternalJob(unbound));
        QVERIFY(engine.endExternalJob(token));
        QVERIFY(!engine.endExternalJob(token)); // 重复结束无效
        QCOMPARE(probe.calls.load(), 0);

        RunEngine::JobToken next;
        QVERIFY(engine.beginExternalJob(&next, nullptr));
        QVERIFY(engine.endExternalJob(next));
    }

    // B3.2 Step 1（计划用例）：stop 落在"Service 检查完停止之后、下一轮启动之前"的窗口时，
    // job 级取消标志必须阻止下一轮启动；旧 job 结束并重新 begin 后才允许再运行。
    void testStopBetweenRoundsPreventsNextRun() {
        RunEngine& engine = RunEngine::instance();
        LeaseProbeModule probe;
        QVERIFY(probe.initialize());
        engine.addModule(&probe);

        RunEngine::JobToken token;
        QVERIFY(engine.beginExternalJob(&token, nullptr));

        // 屏障：钩子同步调 stop() 并释放信号量；信号量只用于证明窗口被执行，超时仅防挂死。
        QSemaphore reached;
        engine.setBeforeJobStartHookForTest([&]() {
            reached.release();
            RunEngine::instance().stop();
        });
        QString reason;
        const bool started = engine.runOnceForJob(token, &reason);
        QVERIFY(reached.tryAcquire(1, 2000));
        QVERIFY2(!started, "stop in the job start window must prevent the run");
        QCOMPARE(reason, QStringLiteral("external_job_cancelled"));
        QCOMPARE(probe.calls.load(), 0);
        QCOMPARE(engine.state(), RunState::Stopped);

        engine.clearBeforeJobStartHookForTest();
        QVERIFY(engine.endExternalJob(token));

        // 新 job 不被旧 job 的取消标志阻塞。
        RunEngine::JobToken next;
        QVERIFY(engine.beginExternalJob(&next, nullptr));
        QVERIFY(engine.runOnceForJob(next, nullptr));
        QCOMPARE(probe.calls.load(), 1);
        QVERIFY(engine.endExternalJob(next));
    }
};

QTEST_MAIN(TestRunEngineJobLease)
#include "test_runenginejoblease.moc"
