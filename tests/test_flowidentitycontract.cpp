#include <core/engine/EngineTestSupport.h>
// T2.6 frame-identity behavior contract -- DELIVERY CHAIN regression tests.
//
// Nature (review ruling 2026-11-09, option 2): BASELINE ALREADY GREEN, NOT an
// NF red light, NO production fix, NO new signal parameter. These cases freeze
// the observable behavior contracts of the existing frame-identity path:
//   (a) >=5 consecutive short jobs under a delayed GUI queue keep frame
//       (jobId, runId) ownership unique; no late-frame acceptance, no
//       valid-frame drop; the last job is really rendered with its own image
//       and run-level snapshot;
//   (b) displayFrameReady's (viewportId, jobId, runId) args + the slot frame's
//       DisplayFrame::matchesIdentity() are the frame-identity source of truth.
//
// The internal read-back skew (notify serial != handling-time displayJobId) is
// RECORDED only, never asserted to a fixed count; the current signal signature
// is never frozen as a contract. Only existing public interfaces are used;
// no production source is changed.
//
// Method mirrors the isolated T2.6 prerequisite probe (v3): GUI thread does not
// pump its event queue while a driver thread runs 5 short Single jobs; enter /
// release / completion barriers (QSemaphore) construct the ordering, NO fixed
// sleep. DirectConnection records emit-time truth; QueuedConnection slots
// reproduce MainWindow.cpp:755-766 / :772-782 / :799-808 (+ finishRunUi's
// clearPending / clearExpectedIdentity). After the release barrier we
// DETERMINISTICALLY wait for all 5 jobs to finish and consume every final
// queued notification BEFORE the guard disconnects.

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QThread>
#include <QtGlobal>
#include <QtTest/QtTest>
#include <atomic>
#include <core/base/ModuleBase.h>
#include <core/engine/DisplayFrame.h>
#include <core/engine/FlowExecutionService.h>
#include <core/engine/RunEngine.h>
#include <core/model/ImageData.h>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <ui/display/ImageRefreshCoalescer.h>
#include <vector>

using namespace DeepLux;

namespace {

constexpr int kJobs = 5;
constexpr int kHoldJob = 5;

void contractLog(const QString& line) {
    const QByteArray bytes = line.toUtf8();
    fprintf(stderr, "%s\n", bytes.constData());
    fflush(stderr);
}

struct ContractBarrier {
    QSemaphore entered[kJobs + 1];
    QSemaphore release[kJobs + 1];
    QSemaphore allDone;
    std::atomic_int completed{0};
    std::atomic_int enteredCount{0};
    std::atomic_bool abort{false};
};

struct ContractRender {
    quint64 jobId = 0;
    QString runId;
    quint64 frameId = 0;
    QRgb pixel0 = 0;
    int snapshotK = -1;
};

} // namespace

class ContractProbeModule : public ModuleBase {
    Q_OBJECT

public:
    explicit ContractProbeModule(const QString& name, ContractBarrier* barrier, bool hold)
        : m_barrier(barrier), m_hold(hold) {
        m_moduleId = QStringLiteral("com.deeplux.test.flowidentitycontract");
        m_name = QStringLiteral("FlowIdentityContractProbe");
        m_category = QStringLiteral("test");
        setInstanceName(name);
    }

    std::atomic_int runs{0};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        const int k = m_barrier->enteredCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (k <= kJobs) {
            m_barrier->entered[k].release(); // ENTER barrier
        }
        if (m_hold && k == kHoldJob) {
            m_barrier->release[k].acquire(); // RELEASE barrier (holds the job open)
        }
        ++runs;
        QImage image(24, 18, QImage::Format_RGB32);
        image.fill(QColor(qMin(255, 20 * k), 40, 60));
        output = ImageData(image);
        output.setData(QStringLiteral("contract_k"), k);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    ContractBarrier* m_barrier = nullptr;
    bool m_hold = false;
};

// Approved invariant: NEVER return while the worker still references captured
// stack objects. finish() clears `armed` only after a real shutdown()/join.
// Bounded diagnostics only warn; the outer watchdog (CTest TIMEOUT / timeout
// -k) bounds the unbounded wait; a 60s hard cap aborts the process (no unwind).
class ContractCleanupGuard {
public:
    FlowExecutionService* service = nullptr;
    ContractBarrier* barrier = nullptr;
    std::thread* driver = nullptr;
    ImageRefreshCoalescer* coalescer = nullptr;
    std::vector<QMetaObject::Connection> connections;
    bool armed = true;

    bool finish() {
        if (!service || !armed) {
            return true;
        }
        // 1) disconnect first: no later emit may enter a slot capturing locals.
        for (const auto& c : connections) {
            QObject::disconnect(c);
        }
        connections.clear();
        if (coalescer) {
            coalescer->setRenderCallback(nullptr);
        }
        // 2) unconditionally release every barrier and abort the driver loop.
        if (barrier) {
            barrier->abort.store(true, std::memory_order_release);
            for (int i = 1; i <= kJobs; ++i) {
                barrier->release[i].release();
            }
        }
        // 3) stop the task so the driver's idle-wait can progress, then join.
        service->stop();
        if (driver && driver->joinable()) {
            driver->join();
        }
        // 4) never unwind before the worker really exited.
        const auto pump = [] {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        };
        QElapsedTimer timer;
        timer.start();
        while (service->isActive() && timer.elapsed() < 10000) {
            pump();
        }
        bool joined = false;
        while (timer.elapsed() < 15000) {
            if (service->shutdown()) {
                joined = true;
                break;
            }
            service->stop();
            pump();
        }
        if (!joined || service->isActive()) {
            qWarning("ContractCleanupGuard: bounded diagnostics expired; continuing to wait "
                     "for the real worker exit (outer watchdog bounds this wait)");
            for (;;) {
                service->stop();
                if (service->shutdown() && !service->isActive()) {
                    joined = true;
                    break;
                }
                pump();
                if (timer.elapsed() >= 60000) {
                    qCritical("ContractCleanupGuard: worker did not exit; terminating the test "
                              "process instead of unwinding the stack while the worker still "
                              "references captured locals");
                    std::abort();
                }
            }
        }
        armed = false;
        return true;
    }

    ~ContractCleanupGuard() {
        if (armed) {
            (void) finish();
        }
    }
};

class TestFlowIdentityContract : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void testDelayedQueueAtNextJobKeepsLastJobFrame();    // read-back = serial of a LATER job
    void testDelayedQueueAfterAllJobsKeepsLastJobFrame(); // read-back = 0 (all jobs finished)
    void testFrameIdentityArgsRejectLateNoticeAndKeepRenderedFrame();

private:
    void runContract(bool hold, const QString& variant);
};

void TestFlowIdentityContract::init() {
    FlowExecutionService& service = FlowExecutionService::instance();
    RunEngine& engine = RunEngine::instance();
    service.stop();
    engine.stop();
    engine.clearModules();
    engine.clearOutputs();
    engine.setDisplayCaptureEnabled(false);
    engine.clearDisplayJobId();
    engine.clearLatestDisplayFrames();
}

void TestFlowIdentityContract::cleanup() {
    FlowExecutionService& service = FlowExecutionService::instance();
    RunEngine& engine = RunEngine::instance();
    service.stop();
    engine.stop();
    engine.clearModules();
    engine.clearOutputs();
    engine.setDisplayCaptureEnabled(false);
    engine.clearDisplayJobId();
    engine.clearLatestDisplayFrames();
}

void TestFlowIdentityContract::runContract(bool hold, const QString& variant) {
    FlowExecutionService& service = FlowExecutionService::instance();
    RunEngine& engine = RunEngine::instance();

    // All captured stack objects are declared BEFORE the guard, so on a failing
    // assertion the guard (destructed first) has already joined the worker.
    ContractBarrier barrier;
    ContractProbeModule module(QStringLiteral("contract_probe"), &barrier, hold);
    QVERIFY(module.initialize());
    engine.addModule(&module);
    RunEngineTestAccess(engine).setModuleExecutionAffinityForTest(QStringLiteral("contract_probe"),
                                                                  ExecutionAffinity::Worker);
    engine.setDisplayCaptureEnabled(true);

    ImageRefreshCoalescer coalescer;
    QObject ctx; // GUI-thread receiver => explicit QueuedConnection below.

    QMutex emitMutex;
    std::vector<quint64> emitSerials;
    std::vector<quint64> emitDisplayIds;
    std::vector<quint64> renderedJobs;
    std::vector<ContractRender> renderedFrames;
    int runNotifyCount = 0;
    int frameNotifyCount = 0;
    int finishedNotifyCount = 0;
    quint64 latestNotifySerial = 0;
    quint64 lastReadBack = 0;
    int mismatch = 0;
    int hwWrong = 0;
    int lateAccepted = 0;
    int validDropped = 0;
    int identityDrops = 0;
    int highwaterDrops = 0;

    coalescer.setRenderCallback([&](const DisplayFrame& frame) {
        renderedJobs.push_back(frame.jobId);
        ContractRender rec;
        rec.jobId = frame.jobId;
        rec.runId = frame.runId;
        rec.frameId = frame.frameId;
        rec.pixel0 = frame.image.isNull() ? QRgb(0) : frame.image.pixel(0, 0);
        const QVariant kVar = frame.moduleDataSnapshot.value(module.instanceName()).value(QStringLiteral("contract_k"));
        rec.snapshotK = kVar.isValid() ? kVar.toInt() : -1;
        renderedFrames.push_back(rec);
        contractLog(QStringLiteral("CONTRACT-RENDER variant=%1 jobId=%2 runId=%3 frameId=%4 "
                                   "pixel0=%5 snapshotK=%6")
                        .arg(variant)
                        .arg(frame.jobId)
                        .arg(frame.runId)
                        .arg(frame.frameId)
                        .arg(QString::number(rec.pixel0, 16))
                        .arg(rec.snapshotK));
    });

    // Emit-time truth: DirectConnection runs synchronously on the worker thread.
    auto c1 = QObject::connect(
        &service, &FlowExecutionService::jobStateChanged, &ctx,
        [&](FlowExecutionService::State state, FlowExecutionService::JobType job) {
            Q_UNUSED(job)
            if (state != FlowExecutionService::State::Running) {
                return;
            }
            QMutexLocker locker(&emitMutex);
            emitSerials.push_back(service.currentJobSerial());
            emitDisplayIds.push_back(engine.displayJobId());
        },
        Qt::DirectConnection);

    // Reproduce MainWindow.cpp:772-782 (Running branch) with a queued slot.
    auto c2 = QObject::connect(
        &service, &FlowExecutionService::jobStateChanged, &ctx,
        [&](FlowExecutionService::State state, FlowExecutionService::JobType job) {
            Q_UNUSED(job)
            if (state != FlowExecutionService::State::Running) {
                return;
            }
            const int k = runNotifyCount++;
            quint64 notifySerial = 0;
            quint64 emitDisplay = 0;
            {
                QMutexLocker locker(&emitMutex);
                if (static_cast<size_t>(k) < emitSerials.size()) {
                    notifySerial = emitSerials[static_cast<size_t>(k)];
                    emitDisplay = emitDisplayIds[static_cast<size_t>(k)];
                }
            }
            if (notifySerial > latestNotifySerial) {
                latestNotifySerial = notifySerial;
            }
            const quint64 readBack = engine.displayJobId(); // MainWindow.cpp:781
            lastReadBack = readBack;
            coalescer.clearPending();                           // MainWindow.cpp:780
            coalescer.setExpectedIdentity(readBack, QString()); // MainWindow.cpp:781
            const quint64 highWater = coalescer.currentJobId();
            const int mis = (notifySerial != readBack) ? 1 : 0;
            const int hw = (highWater != notifySerial) ? 1 : 0;
            mismatch += mis;
            hwWrong += hw;
            // Internal read-back skew is RECORDED, not asserted to a fixed count.
            contractLog(QStringLiteral("CONTRACT-RUN variant=%1 k=%2 notifySerial=%3 "
                                       "emitDisplayJobId=%4 readBack=%5 highWater=%6 "
                                       "mismatch=%7 hwWrong=%8")
                            .arg(variant)
                            .arg(k + 1)
                            .arg(notifySerial)
                            .arg(emitDisplay)
                            .arg(readBack)
                            .arg(highWater)
                            .arg(mis)
                            .arg(hw));
        },
        Qt::QueuedConnection);

    // Reproduce MainWindow.cpp:755-766 (displayFrameReady slot) with a queued slot.
    auto c3 = QObject::connect(
        &engine, &RunEngine::displayFrameReady, &ctx,
        [&](const QString& viewportId, quint64 jobId, const QString& runId) {
            ++frameNotifyCount;
            const DisplayFrame slotFrame = engine.latestDisplayFrame(viewportId);
            const bool matched = slotFrame.matchesIdentity(jobId, runId);
            const quint64 liveSerial = service.currentJobSerial();
            const quint64 before = coalescer.currentJobId();
            const int beforePending = coalescer.hasPending(viewportId) ? 1 : 0;
            int dropByHW = 0;
            int scheduled = 0;
            int pendingAfter = 0;
            if (matched) {
                if (slotFrame.jobId != 0 && before != 0 && slotFrame.jobId < before) {
                    dropByHW = 1; // accepts() would reject this frame
                }
                coalescer.schedule(slotFrame);
                scheduled = 1;
                pendingAfter = coalescer.hasPending(viewportId) ? 1 : 0;
                const bool accepted = (beforePending == 0 && pendingAfter == 1) ||
                                      (beforePending == 1 && pendingAfter == 1 && dropByHW == 0);
                if (accepted && jobId < liveSerial) {
                    ++lateAccepted; // old-job frame reached the renderer
                }
                if (!accepted && jobId >= liveSerial) {
                    ++validDropped; // current-job frame was dropped
                }
                if (dropByHW) {
                    ++highwaterDrops;
                }
            } else {
                ++identityDrops; // rejected by matchesIdentity (design 7.3)
            }
            contractLog(QStringLiteral("CONTRACT-FRAME variant=%1 k=%2 argJobId=%3 argRunId=%4 "
                                       "slotJobId=%5 slotRunId=%6 slotFrameIsNull=%7 liveSerial=%8 "
                                       "matched=%9 before=%10 dropByHW=%11 scheduled=%12 "
                                       "pendingBefore=%13 pendingAfter=%14")
                            .arg(variant)
                            .arg(frameNotifyCount)
                            .arg(jobId)
                            .arg(runId)
                            .arg(slotFrame.jobId)
                            .arg(slotFrame.runId)
                            .arg(slotFrame.jobId == 0 ? 1 : 0)
                            .arg(liveSerial)
                            .arg(matched ? 1 : 0)
                            .arg(before)
                            .arg(dropByHW)
                            .arg(scheduled)
                            .arg(beforePending)
                            .arg(pendingAfter));
        },
        Qt::QueuedConnection);

    // Reproduce MainWindow.cpp:799-810 + finishRunUi():6187-6193 with a queued slot.
    auto c4 = QObject::connect(
        &service, &FlowExecutionService::jobFinished, &ctx,
        [&](FlowExecutionService::JobType job, bool stopped) {
            Q_UNUSED(job)
            Q_UNUSED(stopped)
            ++finishedNotifyCount;
            coalescer.forceFlush();
            coalescer.clearPending();          // finishRunUi()
            coalescer.clearExpectedIdentity(); // finishRunUi(): keeps m_jobId high-water
        },
        Qt::QueuedConnection);

    std::thread driver([&]() {
        const quint64 base = service.currentJobSerial();
        for (int i = 1; i <= kJobs && !barrier.abort.load(std::memory_order_acquire); ++i) {
            while ((service.isActive() || service.state() != FlowExecutionService::State::Idle) &&
                   !barrier.abort.load(std::memory_order_acquire)) {
                QThread::yieldCurrentThread();
            }
            if (barrier.abort.load(std::memory_order_acquire)) {
                return;
            }
            const bool ok = service.startSingle();
            contractLog(QStringLiteral("CONTRACT-DRIVER variant=%1 i=%2 startSingle=%3 base=%4")
                            .arg(variant)
                            .arg(i)
                            .arg(ok ? 1 : 0)
                            .arg(base));
            const quint64 expect = base + static_cast<quint64>(i);
            while (service.lastFinishedJobSerial() < expect && !barrier.abort.load(std::memory_order_acquire)) {
                QThread::yieldCurrentThread();
            }
            barrier.completed.fetch_add(1, std::memory_order_release);
        }
        barrier.allDone.release();
    });

    ContractCleanupGuard guard;
    guard.service = &service;
    guard.barrier = &barrier;
    guard.driver = &driver;
    guard.coalescer = &coalescer;
    guard.connections = {c1, c2, c3, c4};

    // ENTER / COMPLETION barrier: GUI event queue is NOT pumped until the
    // background has finished >=4 short jobs and entered the 5th (variant A),
    // or finished all 5 (variant B). No fixed sleep.
    if (hold) {
        QVERIFY2(barrier.entered[kHoldJob].tryAcquire(1, 30000),
                 "variant A: background must reach the 5th job before GUI handles old notices");
    } else {
        QVERIFY2(barrier.allDone.tryAcquire(1, 30000),
                 "variant B: background must finish all 5 jobs before GUI handles old notices");
    }
    QCOMPARE(barrier.completed.load(std::memory_order_acquire), hold ? kJobs - 1 : kJobs);

    const quint64 readBackAtHandling = engine.displayJobId();
    contractLog(QStringLiteral("CONTRACT-BARRIER variant=%1 completed=%2 "
                               "displayJobIdAtHandling=%3 hold=%4")
                    .arg(variant)
                    .arg(barrier.completed.load(std::memory_order_acquire))
                    .arg(readBackAtHandling)
                    .arg(hold ? 1 : 0));

    // Delayed GUI dispatch: drain the accumulated queue in bounded passes.
    for (int pass = 0; pass < 40 && runNotifyCount < kJobs; ++pass) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 0);

    // Health assertions (NOT defect assertions): the harness itself must be valid.
    QCOMPARE(runNotifyCount, kJobs);
    QVERIFY2(frameNotifyCount >= kJobs, "every short job must publish at least one frame notice");
    QVERIFY2(barrier.enteredCount.load(std::memory_order_acquire) >= kJobs,
             "at least 5 short jobs must have executed the probe module");

    // RELEASE barrier: let the held job finish, then DETERMINISTICALLY wait for
    // all 5 jobs to complete (no fixed sleep) and consume every final queued
    // notification BEFORE the guard disconnects. This is what makes
    // validDropped == 0 a real statement about the 5th job's valid frame.
    for (int i = 1; i <= kJobs; ++i) {
        barrier.release[i].release();
    }
    QElapsedTimer drainTimer;
    drainTimer.start();
    while (barrier.completed.load(std::memory_order_acquire) < kJobs && drainTimer.elapsed() < 30000) {
        QThread::yieldCurrentThread();
    }
    QCOMPARE(barrier.completed.load(std::memory_order_acquire), kJobs);

    drainTimer.restart();
    while ((finishedNotifyCount < kJobs || renderedFrames.empty()) && drainTimer.elapsed() < 30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }

    contractLog(QStringLiteral("CONTRACT-SUMMARY variant=%1 batch=2 runNotifies=%2 "
                               "frameNotifies=%3 finishedNotifies=%4 renders=%5 "
                               "lateAccepted=%6 validDropped=%7 completed=%8 lastRenderedJob=%9 "
                               "mismatch=%10 hwWrong=%11 identityDrops=%12")
                    .arg(variant)
                    .arg(runNotifyCount)
                    .arg(frameNotifyCount)
                    .arg(finishedNotifyCount)
                    .arg(static_cast<int>(renderedJobs.size()))
                    .arg(lateAccepted)
                    .arg(validDropped)
                    .arg(barrier.completed.load(std::memory_order_acquire))
                    .arg(renderedFrames.empty() ? quint64(0) : renderedFrames.back().jobId)
                    .arg(mismatch)
                    .arg(hwWrong)
                    .arg(identityDrops));

    // ---- Behavior-contract ASSERTIONS (not fixed counts, not signal signature) ----
    QCOMPARE(runNotifyCount, kJobs);
    QCOMPARE(finishedNotifyCount, kJobs);
    QVERIFY2(!renderedFrames.empty(), "the last short job must actually reach the renderer");
    const quint64 expectedLastSerial = service.lastFinishedJobSerial();
    QVERIFY2(expectedLastSerial > 0, "at least one job must have finished");
    const QRgb expectedPixel = QColor(qMin(255, 20 * kJobs), 40, 60).rgb();
    bool snapshotMatched = false;
    for (const ContractRender& rec : renderedFrames) {
        // No stale/old-job frame may ever reach the renderer: displayed frame
        // identity (jobId, runId) is unique to the last job.
        QCOMPARE(rec.jobId, expectedLastSerial);
        QCOMPARE(rec.pixel0, expectedPixel);
        QVERIFY2(!rec.runId.isEmpty(), "every rendered frame must carry its runId");
        if (rec.snapshotK == kJobs) {
            snapshotMatched = true;
        }
    }
    QVERIFY2(snapshotMatched, "the last job's run-level snapshot must reach the renderer");
    QCOMPARE(lateAccepted, 0);
    QCOMPARE(validDropped, 0);

    // finish() returns only after a real join; no constant-true assertion needed.
    guard.finish();
}

void TestFlowIdentityContract::testDelayedQueueAtNextJobKeepsLastJobFrame() {
    runContract(true, QStringLiteral("A_at_next_job"));
}

void TestFlowIdentityContract::testDelayedQueueAfterAllJobsKeepsLastJobFrame() {
    runContract(false, QStringLiteral("B_after_all_jobs"));
}

void TestFlowIdentityContract::testFrameIdentityArgsRejectLateNoticeAndKeepRenderedFrame() {
    // Contract (b): the identity source of truth is the displayFrameReady ARGS
    // checked against the slot frame via DisplayFrame::matchesIdentity, NOT a
    // handling-time read-back of RunEngine::displayJobId(). A late old-job
    // notice must not be scheduled and must not rewrite the rendered frame.
    ImageRefreshCoalescer coalescer;
    std::vector<DisplayFrame> rendered;
    coalescer.setRenderCallback([&](const DisplayFrame& frame) { rendered.push_back(frame); });

    DisplayFrame frame;
    frame.jobId = 5;
    frame.runId = QStringLiteral("run-5");
    frame.viewportId = QStringLiteral("main");
    frame.image = QImage(8, 8, QImage::Format_RGB32);
    frame.image.fill(QColor(100, 40, 60));
    QVariantMap snap;
    snap.insert(QStringLiteral("contract_k"), 5);
    frame.moduleDataSnapshot.insert(QStringLiteral("contract_probe"), snap);

    // MainWindow.cpp:762 -- the notice args are checked against the slot frame.
    QVERIFY(frame.matchesIdentity(5, QStringLiteral("run-5")));
    QVERIFY(!frame.matchesIdentity(1, QStringLiteral("run-old")));
    QVERIFY(!frame.matchesIdentity(5, QStringLiteral("run-other")));

    // Late old-job notice: upstream check rejects it -> no render at all.
    const bool oldDelivered = frame.matchesIdentity(1, QStringLiteral("run-old"));
    if (oldDelivered) {
        coalescer.schedule(frame);
    }
    coalescer.forceFlush();
    QCOMPARE(static_cast<int>(rendered.size()), 0);

    // Current notice: accepted from the ARGS (not from a handling-time read-back).
    coalescer.setExpectedIdentity(5, QStringLiteral("run-5"));
    const bool currentDelivered = frame.matchesIdentity(5, QStringLiteral("run-5"));
    QVERIFY(currentDelivered);
    if (currentDelivered) {
        coalescer.schedule(frame);
    }
    coalescer.forceFlush();
    QCOMPARE(static_cast<int>(rendered.size()), 1);
    QCOMPARE(rendered.back().jobId, quint64(5));
    QCOMPARE(rendered.back().runId, QStringLiteral("run-5"));
    QCOMPARE(rendered.back().image.pixel(0, 0), QColor(100, 40, 60).rgb());
    QCOMPARE(rendered.back()
                 .moduleDataSnapshot.value(QStringLiteral("contract_probe"))
                 .value(QStringLiteral("contract_k"))
                 .toInt(),
             5);

    // A second late notice must not rewrite the already rendered frame.
    const bool lateAgain = frame.matchesIdentity(4, QStringLiteral("run-4"));
    if (lateAgain) {
        coalescer.schedule(frame);
    }
    coalescer.forceFlush();
    QCOMPARE(static_cast<int>(rendered.size()), 1);
}

QTEST_MAIN(TestFlowIdentityContract)
#include "test_flowidentitycontract.moc"
