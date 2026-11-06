#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/engine/DisplayFrame.h>
#include <core/engine/FlowExecutionService.h>
#include <core/engine/RunEngine.h>
#include <core/model/ImageData.h>
#include <ui/display/DisplayManager.h>
#include <ui/display/ImageRefreshCoalescer.h>
#include <ui/widgets/HImageWidget.h>
#include <ui/widgets/ViewportWidget.h>

using namespace DeepLux;

/// B8.1 探针：每轮输出一张可辨识的图像，metadata 带 value/tag 供快照比对。
class FrameProbeModule : public ModuleBase {
    Q_OBJECT

public:
    explicit FrameProbeModule(const QString& instanceId = QStringLiteral("frame_probe")) {
        m_moduleId = QStringLiteral("com.deeplux.test.frameprobe");
        m_name = QStringLiteral("FrameProbe");
        m_category = QStringLiteral("test");
        setInstanceName(instanceId);
    }

    int value = 1;
    QImage lastImage;
    std::function<void()> duringProcess;
    std::atomic_int runs{0};

protected:
    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        ++runs;
        if (duringProcess) {
            duringProcess();
        }
        QImage image(24, 18, QImage::Format_RGB32);
        image.fill(QColor(qMin(255, 10 * value), 40, 60));
        lastImage = image;
        output = ImageData(image);
        output.setData(QStringLiteral("value"), value);
        output.setData(QStringLiteral("tag"), QStringLiteral("frame"));
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

namespace {

DisplayFrame frameWith(quint64 frameId, quint64 jobId = 1, const QString& runId = QStringLiteral("run-1"),
                       const QString& viewportId = QStringLiteral("main")) {
    DisplayFrame frame;
    frame.jobId = jobId;
    frame.runId = runId;
    frame.frameId = frameId;
    frame.moduleId = QStringLiteral("probe");
    frame.viewportId = viewportId;
    frame.image = QImage(4, 4, QImage::Format_RGB32);
    frame.image.fill(Qt::green);
    return frame;
}

// 确定性等待：处理事件直到条件满足（只等待条件，不用 sleep 构造顺序）。
bool waitForCondition(const std::function<bool()>& condition, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (condition()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qWait(1);
    }
    return condition();
}

} // namespace

class TestImageRefreshCoalescer : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // B8.1 DisplayFrame 快照与帧身份（设计 §7.1）
    void testFrameCarriesIdentityAndSnapshot();
    void testIntermediateFrameCarriesNoSnapshot();
    void testFrameImageIsDeepCopy();
    void testFrameSnapshotSurvivesNextRun();
    void testCaptureDisabledPublishesNothing();
    void testServiceInjectsJobIdIntoFrames();

    // B8.2 UI 合并器（设计 §7.2/§7.3）
    void testLatestWinsWithinThrottleWindow();
    void testContinuousNotificationsKeepThrottleTimebase();
    void testForceFlushRendersLatestPending();
    void testDropsMismatchedIdentity();
    void testNewJobClearsPending();
    void testClearPendingDiscardsFrames();
    // t17/F2 加固：解除身份锁定后仍按“最近 job”高水位丢弃更旧 job 的迟到帧（设计 §7.3）
    void testDropsOldJobFramesAfterIdentityCleared();

    // B8.3 视图保持（设计 §7.4）
    void testPreserveViewKeepsZoomAndOffset();
    void testPreserveViewKeepsRoiAndOverlay();
    void testFirstDisplayAndExplicitFitStillFit();
    void testViewportAndDisplayManagerPropagatePreserveView();
};

void TestImageRefreshCoalescer::init() {
    FlowExecutionService::instance().stop();
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
    RunEngine::instance().setDisplayCaptureEnabled(false);
    RunEngine::instance().clearDisplayJobId();
    RunEngine::instance().clearLatestDisplayFrames();
}

void TestImageRefreshCoalescer::cleanup() {
    FlowExecutionService::instance().stop();
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
    RunEngine::instance().setDisplayCaptureEnabled(false);
    RunEngine::instance().clearDisplayJobId();
    RunEngine::instance().clearLatestDisplayFrames();
}

// ---------------------------------------------------------------------------
// B8.1 DisplayFrame 快照与帧身份（设计 §7.1）
// ---------------------------------------------------------------------------

void TestImageRefreshCoalescer::testFrameCarriesIdentityAndSnapshot() {
    RunEngine& engine = RunEngine::instance();
    FrameProbeModule first(QStringLiteral("frame_a"));
    FrameProbeModule second(QStringLiteral("frame_b"));
    first.value = 1;
    second.value = 2;
    QVERIFY(first.initialize());
    QVERIFY(second.initialize());
    engine.addModule(&first);
    engine.addModule(&second);
    engine.setDisplayCaptureEnabled(true);
    engine.setDisplayJobId(42);
    QSignalSpy readySpy(&engine, &RunEngine::displayFrameReady);

    engine.runOnce();

    QVERIFY2(readySpy.count() >= 2, "intermediate frame + final snapshot frame must be published");
    const DisplayFrame frame = engine.latestDisplayFrame(QStringLiteral("main"));
    QVERIFY(frame.hasImage());
    QVERIFY(frame.hasSnapshot());
    QCOMPARE(frame.jobId, quint64(42));
    QVERIFY(!frame.runId.isEmpty());
    QCOMPARE(frame.moduleId, second.instanceName());
    QCOMPARE(frame.frameId, quint64(1)); // 该 run 内第二个模块的帧序号
    QCOMPARE(frame.moduleDataSnapshot.value(first.instanceName()).value(QStringLiteral("value")).toInt(), 1);
    QCOMPARE(frame.moduleDataSnapshot.value(second.instanceName()).value(QStringLiteral("value")).toInt(), 2);
    QCOMPARE(frame.moduleDataSnapshot.value(second.instanceName()).value(QStringLiteral("tag")).toString(),
             QStringLiteral("frame"));

    // displayFrameReady 是轻量通知：参数带 (viewportId, jobId, runId)，与槽位帧身份一致。
    const QList<QVariant> last = readySpy.last();
    QCOMPARE(last.at(0).toString(), QStringLiteral("main"));
    QCOMPARE(last.at(1).toULongLong(), quint64(42));
    QCOMPARE(last.at(2).toString(), frame.runId);
}

void TestImageRefreshCoalescer::testIntermediateFrameCarriesNoSnapshot() {
    RunEngine& engine = RunEngine::instance();
    FrameProbeModule first(QStringLiteral("frame_first"));
    FrameProbeModule second(QStringLiteral("frame_second"));
    first.value = 1;
    second.value = 2;
    QVERIFY(first.initialize());
    QVERIFY(second.initialize());
    engine.addModule(&first);
    engine.addModule(&second);
    engine.setDisplayCaptureEnabled(true);
    engine.setDisplayJobId(7);

    DisplayFrame seenDuringRun;
    second.duringProcess = [&engine, &seenDuringRun]() {
        seenDuringRun = engine.latestDisplayFrame(QStringLiteral("main"));
    };

    engine.runOnce();

    // 运行中的帧只带图像（快照在 run 结束时一次性抓取），且归属产生该帧的模块。
    QVERIFY(seenDuringRun.hasImage());
    QVERIFY(!seenDuringRun.hasSnapshot());
    QCOMPARE(seenDuringRun.moduleId, first.instanceName());
    // 收尾帧补齐同轮快照。
    QVERIFY(engine.latestDisplayFrame(QStringLiteral("main")).hasSnapshot());
}

void TestImageRefreshCoalescer::testFrameImageIsDeepCopy() {
    RunEngine& engine = RunEngine::instance();
    FrameProbeModule probe;
    QVERIFY(probe.initialize());
    engine.addModule(&probe);
    engine.setDisplayCaptureEnabled(true);

    engine.runOnce();
    const DisplayFrame frame = engine.latestDisplayFrame(QStringLiteral("main"));
    QVERIFY(frame.hasImage());
    const QColor before = frame.image.pixelColor(0, 0);
    QVERIFY(before.isValid());

    // 模块继续持有并修改自己的输出图像（与 ImageData 共享缓冲）：帧内快照不得受影响。
    probe.lastImage.fill(QColor("#FF0000"));
    QCOMPARE(frame.image.pixelColor(0, 0), before);
    QVERIFY(before != QColor("#FF0000"));
}

void TestImageRefreshCoalescer::testFrameSnapshotSurvivesNextRun() {
    RunEngine& engine = RunEngine::instance();
    FrameProbeModule probe;
    QVERIFY(probe.initialize());
    engine.addModule(&probe);
    engine.setDisplayCaptureEnabled(true);

    probe.value = 1;
    engine.runOnce();
    const DisplayFrame roundA = engine.latestDisplayFrame(QStringLiteral("main"));

    // 下一轮开始会清空引擎的模块输出；已发布的旧帧必须仍是旧数据（设计 §7.1/§7.3）。
    probe.value = 2;
    engine.runOnce();
    const DisplayFrame roundB = engine.latestDisplayFrame(QStringLiteral("main"));

    QCOMPARE(roundA.moduleDataSnapshot.value(probe.instanceName()).value(QStringLiteral("value")).toInt(), 1);
    QCOMPARE(roundB.moduleDataSnapshot.value(probe.instanceName()).value(QStringLiteral("value")).toInt(), 2);
    QVERIFY(roundA.runId != roundB.runId);
    QVERIFY(roundA.hasImage());
    QVERIFY(roundB.hasImage());
}

void TestImageRefreshCoalescer::testCaptureDisabledPublishesNothing() {
    RunEngine& engine = RunEngine::instance();
    FrameProbeModule probe;
    QVERIFY(probe.initialize());
    engine.addModule(&probe);
    engine.setDisplayCaptureEnabled(false);
    QSignalSpy readySpy(&engine, &RunEngine::displayFrameReady);

    engine.runOnce();

    QCOMPARE(readySpy.count(), 0);
    QVERIFY(engine.latestDisplayFrame(QStringLiteral("main")).image.isNull());
    QVERIFY(!engine.latestDisplayFrame(QStringLiteral("main")).hasSnapshot());
}

void TestImageRefreshCoalescer::testServiceInjectsJobIdIntoFrames() {
    RunEngine& engine = RunEngine::instance();
    FrameProbeModule probe;
    QVERIFY(probe.initialize());
    engine.addModule(&probe);
    engine.setDisplayCaptureEnabled(true);

    FlowExecutionService& service = FlowExecutionService::instance();
    QSignalSpy finishedSpy(&service, &FlowExecutionService::jobFinished);
    QVERIFY(service.startSingle());
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(service.state(), FlowExecutionService::State::Idle, 5000);

    const DisplayFrame frame = engine.latestDisplayFrame(QStringLiteral("main"));
    QVERIFY(frame.hasImage());
    QVERIFY(frame.hasSnapshot());
    QVERIFY2(frame.jobId > 0, "service job must inject its jobId into display frames");
    QCOMPARE(frame.jobId, service.lastFinishedJobSerial());
    // job 结束后帧上下文清空，避免下一轮沿用旧 jobId。
    QCOMPARE(engine.displayJobId(), quint64(0));
}

// ---------------------------------------------------------------------------
// B8.2 UI 合并器（设计 §7.2/§7.3）
// ---------------------------------------------------------------------------

void TestImageRefreshCoalescer::testLatestWinsWithinThrottleWindow() {
    QSemaphore rendered;
    int renders = 0;
    quint64 lastRenderedFrameId = 0;
    ImageRefreshCoalescer coalescer;
    coalescer.setRenderCallback([&](const DisplayFrame& frame) {
        ++renders;
        lastRenderedFrameId = frame.frameId;
        rendered.release();
    });

    for (quint64 i = 1; i <= 100; ++i) {
        coalescer.schedule(frameWith(i));
    }

    QVERIFY(waitForCondition([&rendered]() { return rendered.available() > 0; }, 1000));
    QCOMPARE(renders, 1);
    QCOMPARE(lastRenderedFrameId, quint64(100));
}

void TestImageRefreshCoalescer::testContinuousNotificationsKeepThrottleTimebase() {
    ImageRefreshCoalescer coalescer;
    int renders = 0;
    quint64 lastRenderedFrameId = 0;
    coalescer.setRenderCallback([&](const DisplayFrame& frame) {
        ++renders;
        lastRenderedFrameId = frame.frameId;
    });

    quint64 frameId = 0;
    QTimer producer;
    producer.setInterval(5);
    QObject::connect(&producer, &QTimer::timeout, &coalescer, [&]() { coalescer.schedule(frameWith(++frameId)); });
    bool windowClosed = false;
    QTimer stopTimer;
    stopTimer.setSingleShot(true);
    QObject::connect(&stopTimer, &QTimer::timeout, &coalescer, [&]() {
        producer.stop();
        windowClosed = true;
    });
    QElapsedTimer elapsed;
    elapsed.start();
    producer.start();
    stopTimer.start(500);
    while (!windowClosed && elapsed.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QTest::qWait(1);
    }
    QVERIFY(windowClosed);
    const int rendersInWindow = renders;
    // 收尾：处理最后排定的刷新时点（不计入 500ms 窗口内的渲染次数）。
    QTest::qWait(ImageRefreshCoalescer::ThrottleMs);

    QVERIFY2(frameId >= 80, qPrintable(QString("producer must keep notifying, frames=%1").arg(frameId)));
    // 50ms 是频率上限而非防抖：500ms 内约 10 次渲染（计划允许 ±2）。
    QVERIFY2(rendersInWindow >= 8 && rendersInWindow <= 12,
             qPrintable(QString("renders=%1 must stay within throttle bound 8..12").arg(rendersInWindow)));
    QVERIFY(lastRenderedFrameId > 0);
    QVERIFY(lastRenderedFrameId <= frameId);
}

void TestImageRefreshCoalescer::testForceFlushRendersLatestPending() {
    int renders = 0;
    quint64 lastRenderedFrameId = 0;
    ImageRefreshCoalescer coalescer;
    coalescer.setRenderCallback([&](const DisplayFrame& frame) {
        ++renders;
        lastRenderedFrameId = frame.frameId;
    });

    coalescer.schedule(frameWith(1));
    coalescer.schedule(frameWith(2));
    QVERIFY(coalescer.hasPending());
    coalescer.forceFlush();
    QCOMPARE(renders, 1);
    QCOMPARE(lastRenderedFrameId, quint64(2));
    QVERIFY(!coalescer.hasPending());

    // 无 pending 时 force flush 不重复渲染。
    coalescer.forceFlush();
    QCOMPARE(renders, 1);
}

void TestImageRefreshCoalescer::testDropsMismatchedIdentity() {
    int renders = 0;
    quint64 lastRenderedFrameId = 0;
    ImageRefreshCoalescer coalescer;
    coalescer.setRenderCallback([&](const DisplayFrame& frame) {
        ++renders;
        lastRenderedFrameId = frame.frameId;
    });
    coalescer.setExpectedIdentity(7, QStringLiteral("run-7-a"));

    coalescer.schedule(frameWith(1, 7, QStringLiteral("run-7-a")));
    coalescer.schedule(frameWith(2, 7, QStringLiteral("run-7-b"))); // 同 job、runId 失配 → 丢弃
    coalescer.schedule(frameWith(3, 6, QStringLiteral("run-6")));   // 旧 job 迟到帧 → 丢弃
    coalescer.forceFlush();

    QCOMPARE(renders, 1);
    QCOMPARE(lastRenderedFrameId, quint64(1));
}

void TestImageRefreshCoalescer::testNewJobClearsPending() {
    int renders = 0;
    quint64 lastRenderedFrameId = 0;
    ImageRefreshCoalescer coalescer;
    coalescer.setRenderCallback([&](const DisplayFrame& frame) {
        ++renders;
        lastRenderedFrameId = frame.frameId;
    });
    coalescer.setExpectedIdentity(1, QString());

    coalescer.schedule(frameWith(1, 1, QStringLiteral("run-1")));
    QVERIFY(coalescer.hasPending());
    QCOMPARE(coalescer.pendingFrameId(), quint64(1));

    coalescer.schedule(frameWith(2, 2, QStringLiteral("run-2")));
    QCOMPARE(coalescer.pendingFrameId(), quint64(2));
    QCOMPARE(coalescer.currentJobId(), quint64(2));

    coalescer.forceFlush();
    QCOMPARE(renders, 1);
    QCOMPARE(lastRenderedFrameId, quint64(2));
}

void TestImageRefreshCoalescer::testClearPendingDiscardsFrames() {
    int renders = 0;
    quint64 lastRenderedFrameId = 0;
    ImageRefreshCoalescer coalescer;
    coalescer.setRenderCallback([&](const DisplayFrame& frame) {
        ++renders;
        lastRenderedFrameId = frame.frameId;
    });

    coalescer.schedule(frameWith(1));
    QVERIFY(coalescer.hasPending());
    coalescer.clearPending();
    QVERIFY(!coalescer.hasPending());
    coalescer.forceFlush();
    QCOMPARE(renders, 0);

    // 清空后新帧仍按合并器调度渲染。
    coalescer.schedule(frameWith(2));
    QVERIFY(waitForCondition([&renders]() { return renders == 1; }, 1000));
    QCOMPARE(lastRenderedFrameId, quint64(2));
}

void TestImageRefreshCoalescer::testDropsOldJobFramesAfterIdentityCleared() {
    // 设计 §7.3：解除身份锁定（如 job 结束/工程切换）后仍保留“最近 job”高水位——
    // 更旧 job 的迟到帧必须丢弃，不得渲染到当前画面；新 job（jobId 更大）照常接管。
    int renders = 0;
    quint64 lastRenderedFrameId = 0;
    ImageRefreshCoalescer coalescer;
    coalescer.setRenderCallback([&](const DisplayFrame& frame) {
        ++renders;
        lastRenderedFrameId = frame.frameId;
    });

    coalescer.setExpectedIdentity(5, QStringLiteral("run-5"));
    coalescer.schedule(frameWith(1, 5, QStringLiteral("run-5")));
    QVERIFY(coalescer.hasPending());

    coalescer.clearExpectedIdentity();
    QCOMPARE(coalescer.currentJobId(), quint64(5)); // 高水位保留
    coalescer.clearPending();

    // 更旧 job 的迟到帧：无身份锁定时也必须丢弃。
    coalescer.schedule(frameWith(2, 4, QStringLiteral("run-4")));
    QVERIFY(!coalescer.hasPending());
    coalescer.clearPending();

    // 同一最近 job 的帧：不锁定 runId 时接受。
    coalescer.schedule(frameWith(4, 5, QStringLiteral("run-5-b")));
    QCOMPARE(coalescer.pendingFrameId(), quint64(4));

    // 新 job：接管并清空旧 pending。
    coalescer.schedule(frameWith(5, 6, QStringLiteral("run-6")));
    QCOMPARE(coalescer.pendingFrameId(), quint64(5));
    QCOMPARE(coalescer.currentJobId(), quint64(6));

    coalescer.forceFlush();
    QCOMPARE(renders, 1);
    QCOMPARE(lastRenderedFrameId, quint64(5));
}

// ---------------------------------------------------------------------------
// B8.3 视图保持（设计 §7.4）
// ---------------------------------------------------------------------------

void TestImageRefreshCoalescer::testPreserveViewKeepsZoomAndOffset() {
    HImageWidget widget;
    widget.resize(400, 300);
    widget.show();
    QCoreApplication::processEvents();

    QImage first(200, 100, QImage::Format_RGB32);
    first.fill(Qt::green);
    widget.setImage(first); // 首次显示 → fit
    QVERIFY(widget.zoom() > 0.0);

    widget.setZoom(2.5);
    const double zoomBefore = widget.zoom();
    const QPointF offsetBefore = widget.imageToWidget(QPointF(0, 0));

    QImage second(200, 100, QImage::Format_RGB32);
    second.fill(Qt::blue);
    widget.setImage(second, /*preserveView=*/true);

    QCOMPARE(widget.zoom(), zoomBefore);
    QCOMPARE(widget.imageToWidget(QPointF(0, 0)), offsetBefore);
}

void TestImageRefreshCoalescer::testPreserveViewKeepsRoiAndOverlay() {
    HImageWidget widget;
    widget.resize(400, 300);
    widget.show();
    QCoreApplication::processEvents();

    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::green);
    widget.setImage(image);
    widget.setMeasurementOverlay({MeasurementOverlayPoint{QPointF(10, 10), QStringLiteral("P")}},
                                 {MeasurementOverlayLine{QPointF(0, 0), QPointF(20, 20), QStringLiteral("L")}});

    widget.setImage(image, /*preserveView=*/true);
    QCOMPARE(widget.measurementPoints().size(), 1);
    QCOMPARE(widget.measurementLines().size(), 1);
    QCOMPARE(widget.measurementPoints().first().label, QStringLiteral("P"));
}

void TestImageRefreshCoalescer::testFirstDisplayAndExplicitFitStillFit() {
    HImageWidget widget;
    widget.resize(400, 300);
    widget.show();
    QCoreApplication::processEvents();

    // 首次显示即使请求保持视图也必须 fit（没有旧视图可保持）。
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::green);
    widget.setImage(image, /*preserveView=*/true);
    const double expectedFit = qMin(400.0 / 200.0, 300.0 / 100.0) * 0.95;
    QVERIFY2(qAbs(widget.zoom() - expectedFit) < 0.01,
             qPrintable(QString("first display must fit: zoom=%1 expected=%2").arg(widget.zoom()).arg(expectedFit)));

    widget.actualSize();
    QCOMPARE(widget.zoom(), 1.0);
    widget.fitToWindow();
    QVERIFY(qAbs(widget.zoom() - expectedFit) < 0.01);

    // preserveView=false 的显式刷新重新 fit（点击模块选择/显式适配路径）。
    widget.setZoom(1.0);
    QImage other(100, 100, QImage::Format_RGB32);
    other.fill(Qt::blue);
    widget.setImage(other, /*preserveView=*/false);
    const double otherFit = qMin(400.0 / 100.0, 300.0 / 100.0) * 0.95;
    QVERIFY2(qAbs(widget.zoom() - otherFit) < 0.01,
             qPrintable(QString("explicit refresh must fit: zoom=%1 expected=%2").arg(widget.zoom()).arg(otherFit)));
}

void TestImageRefreshCoalescer::testViewportAndDisplayManagerPropagatePreserveView() {
    DisplayManager manager;
    manager.centralDisplay()->resize(480, 360);
    manager.centralDisplay()->show();
    QCoreApplication::processEvents();

    const QVector<ViewportWidget*> viewports = manager.allViewports();
    QVERIFY(!viewports.isEmpty());
    ViewportWidget* viewport = viewports.first();
    QVERIFY(viewport != nullptr);
    HImageWidget* imageWidget = viewport->imageWidget();
    QVERIFY(imageWidget != nullptr);
    QVERIFY(imageWidget->width() > 0);
    QVERIFY(imageWidget->height() > 0);

    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::green);
    viewport->displayImage(image); // 首次显示 → fit
    const double fitZoom = imageWidget->zoom();
    QVERIFY(fitZoom > 0.0);

    imageWidget->setZoom(2.0);
    viewport->displayImage(image, /*preserveView=*/true);
    QCOMPARE(imageWidget->zoom(), 2.0);

    viewport->displayImage(image, /*preserveView=*/false);
    QVERIFY(qAbs(imageWidget->zoom() - fitZoom) < 0.01);

    // DisplayManager::displayImage 同样传递 preserveView（合并器渲染入口）。
    imageWidget->setZoom(2.0);
    manager.displayImage(image, QString(), /*preserveView=*/true);
    QCOMPARE(imageWidget->zoom(), 2.0);
    manager.displayImage(image, QString(), /*preserveView=*/false);
    QVERIFY(qAbs(imageWidget->zoom() - fitZoom) < 0.01);
}

QTEST_MAIN(TestImageRefreshCoalescer)
#include "test_imagerefreshcoalescer.moc"
