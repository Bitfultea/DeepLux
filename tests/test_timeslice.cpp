#include "core/base/ModuleBase.h"
#include "core/model/ImageData.h"
#include "plugins/system/TimeSlice/TimeSlicePlugin.h"

#include <QJsonObject>
#include <QThread>
#include <QVariant>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestTimeSlice : public QObject {
    Q_OBJECT

private slots:
    void testStartOutputsStartTime();
    void testStopWithValidStartTimeProducesElapsed();
    void testStopWithoutStartTimeFails();
    void testStartStopRoundTripElapsedIsReasonable();
    // G2-fix1 新增
    void testStopNonNumericStartTimeFails();
    void testStopFutureStartTimeFails();
    void testUnknownModeFails();
    void testValidateRejectsBadMode();
};

// 辅助：用 ABI v2 execute() 运行 TimeSlice，返回结果与输出载体
static ExecutionResult runSlice(TimeSlicePlugin& plugin, const QJsonObject& params,
                                const ImageData& input, ImageData& output) {
    plugin.setParams(params);
    PortValueMap inputs;
    inputs.insert(QStringLiteral("image"), QVariant::fromValue(input));
    // 将载体中的命名数据键显式化为端口（模拟 ModuleBase::execute 的 hydrate 行为）
    const QMap<QString, QVariant> carrierData = input.allData();
    for (auto it = carrierData.constBegin(); it != carrierData.constEnd(); ++it) {
        if (it.key() != QLatin1String("image"))
            inputs.insert(it.key(), it.value());
    }
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = plugin.execute(inputs, outputs, ctx);
    if (outputs.contains(QStringLiteral("image")))
        output = outputs.value(QStringLiteral("image")).value<ImageData>();
    return result;
}

void TestTimeSlice::testStartOutputsStartTime() {
    TimeSlicePlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    QJsonObject params{{"mode", "Start"}, {"sliceName", "s1"}};
    const ExecutionResult result = runSlice(plugin, params, input, output);

    QVERIFY2(result.success, qPrintable(result.userMessage));
    // G-fix1: Start 必须输出 timeslice_start_time 供下游 Stop 读取
    QVERIFY2(output.hasData("timeslice_start_time"),
             "Start must output timeslice_start_time for downstream Stop");
    QVERIFY(output.data("timeslice_start_time").toLongLong() > 0);
    QCOMPARE(output.data("timeslice_name").toString(), QString("s1"));
}

void TestTimeSlice::testStopWithValidStartTimeProducesElapsed() {
    TimeSlicePlugin plugin;
    QVERIFY(plugin.initialize());

    // 模拟上游 Start 已输出起始时间
    ImageData input;
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch() - 120; // 120ms 前
    input.setData("timeslice_start_time", startMs);

    ImageData output;
    QJsonObject params{{"mode", "Stop"}, {"sliceName", "s1"}};
    const ExecutionResult result = runSlice(plugin, params, input, output);

    QVERIFY2(result.success, qPrintable(result.userMessage));
    QVERIFY(output.hasData("timeslice_elapsed_ms"));
    const qint64 elapsed = output.data("timeslice_elapsed_ms").toLongLong();
    // 耗时应 >= 120ms（起始时间在过去），且远小于 Unix 时间戳
    QVERIFY2(elapsed >= 120, qPrintable(QString("elapsed=%1 should be >= 120").arg(elapsed)));
    QVERIFY2(elapsed < 60000, qPrintable(QString("elapsed=%1 unreasonably large").arg(elapsed)));
}

void TestTimeSlice::testStopWithoutStartTimeFails() {
    TimeSlicePlugin plugin;
    QVERIFY(plugin.initialize());

    // Stop 但没有 timeslice_start_time 输入
    ImageData input, output;
    QJsonObject params{{"mode", "Stop"}, {"sliceName", "s1"}};
    const ExecutionResult result = runSlice(plugin, params, input, output);

    // G-fix1: 缺少起始时间必须失败，不能静默产生错误耗时
    QVERIFY2(!result.success, "Stop without start time must fail, not silently produce wrong elapsed");
    QVERIFY(!result.userMessage.isEmpty());
}

void TestTimeSlice::testStartStopRoundTripElapsedIsReasonable() {
    // 端到端：Start 输出 → 作为 Stop 输入 → 耗时合理
    TimeSlicePlugin startPlugin, stopPlugin;
    QVERIFY(startPlugin.initialize());
    QVERIFY(stopPlugin.initialize());

    ImageData startInput, startOutput;
    QJsonObject startParams{{"mode", "Start"}, {"sliceName", "rt"}};
    QVERIFY(runSlice(startPlugin, startParams, startInput, startOutput).success);

    QThread::msleep(50);

    ImageData stopOutput;
    QJsonObject stopParams{{"mode", "Stop"}, {"sliceName", "rt"}};
    const ExecutionResult stopResult = runSlice(stopPlugin, stopParams, startOutput, stopOutput);

    QVERIFY2(stopResult.success, qPrintable(stopResult.userMessage));
    const qint64 elapsed = stopOutput.data("timeslice_elapsed_ms").toLongLong();
    QVERIFY2(elapsed >= 50, qPrintable(QString("elapsed=%1 should be >= 50").arg(elapsed)));
    QVERIFY2(elapsed < 5000, qPrintable(QString("elapsed=%1 unreasonably large").arg(elapsed)));
}

void TestTimeSlice::testStopNonNumericStartTimeFails() {
    // G2-fix1: "abc" 不能被 toLongLong 静默转为 0
    TimeSlicePlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input;
    input.setData("timeslice_start_time", QStringLiteral("abc"));
    ImageData output;
    QJsonObject params{{"mode", "Stop"}, {"sliceName", "s"}};
    const ExecutionResult result = runSlice(plugin, params, input, output);

    QVERIFY2(!result.success, "non-numeric start time must fail, not silently become 0");
    QVERIFY(!result.userMessage.isEmpty());
}

void TestTimeSlice::testStopFutureStartTimeFails() {
    // G2-fix1: 未来时间戳会产生负耗时，必须拒绝
    TimeSlicePlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input;
    const qint64 futureMs = QDateTime::currentMSecsSinceEpoch() + 1000000; // 未来
    input.setData("timeslice_start_time", futureMs);
    ImageData output;
    QJsonObject params{{"mode", "Stop"}, {"sliceName", "s"}};
    const ExecutionResult result = runSlice(plugin, params, input, output);

    QVERIFY2(!result.success, "future start time must fail (would give negative elapsed)");
    QVERIFY(!result.userMessage.isEmpty());
}

void TestTimeSlice::testUnknownModeFails() {
    // G2-fix1: 未知 mode 不能静默成功
    TimeSlicePlugin plugin;
    QVERIFY(plugin.initialize());

    // 契约：validateParams 拒绝非法 mode，setParams 不应用
    QString error;
    QJsonObject bad{{"mode", "InvalidMode"}, {"sliceName", "s"}};
    QVERIFY2(!plugin.validateParams(bad, error), "validateParams must reject invalid mode");

    // 防御路径：绕过校验直接置非法 mode，process 必须失败
    plugin.setParam("mode", QStringLiteral("InvalidMode"));
    ImageData input, output;
    PortValueMap inputs;
    inputs.insert(QStringLiteral("image"), QVariant::fromValue(input));
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = plugin.execute(inputs, outputs, ctx);

    QVERIFY2(!result.success, "unknown mode must fail in process");
    QVERIFY(!result.userMessage.isEmpty());
}

void TestTimeSlice::testValidateRejectsBadMode() {
    TimeSlicePlugin plugin;
    QString error;
    QJsonObject bad{{"mode", "Bogus"}, {"sliceName", "s"}};
    QVERIFY2(!plugin.validateParams(bad, error), "validateParams must reject invalid mode");
    QVERIFY(!error.isEmpty());

    QJsonObject good{{"mode", "Start"}, {"sliceName", "s"}};
    QVERIFY(plugin.validateParams(good, error));
}

QTEST_MAIN(TestTimeSlice)
#include "test_timeslice.moc"
