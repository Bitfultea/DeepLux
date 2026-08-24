#include "core/base/ModuleBase.h"
#include "core/model/ImageData.h"
#include "plugins/system/TableOutPut/TableOutPutPlugin.h"
#include "plugins/detection/ColorRecognition/ColorRecognitionPlugin.h"
#include "plugins/image_processing/DisplayData/DisplayDataPlugin.h"

#include <QJsonObject>
#include <QVariant>
#include <QtTest/QtTest>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

using namespace DeepLux;

// 阶段 G 行为级验收：TableOutPut / ColorRecognition / DisplayData

class TestRemainingPlugins : public QObject {
    Q_OBJECT

private slots:
    // TableOutPut
    void testTableOutPutFormatsData();
    void testTableOutPutEmptyDataGeneratesEmptyTable();
    void testTableOutPutRowColParamsAffectOutput();
    void testTableOutPutValidateRejectsBadDims();
    void testTableOutPutCloneIndependent();

    // ColorRecognition
    void testColorRecognitionDetectsSolidColor();
    void testColorRecognitionAbsentColorFails();
    void testColorRecognitionEmptyImageFails();
    void testColorRecognitionCloneIndependent();

    // DisplayData
    void testDisplayDataOverlaysText();
    void testDisplayDataEmptyImageFails();
    void testDisplayDataCloneIndependent();
};

#ifdef DEEPLUX_HAS_OPENCV
static ImageData makeSolidColorImage(int b, int g, int r, int width = 120, int height = 120) {
    cv::Mat mat(height, width, CV_8UC3, cv::Scalar(b, g, r));
    ImageData data;
    data.setMat(mat);
    return data;
}
#endif

static ExecutionResult runModule(ModuleBase& plugin, const QJsonObject& params,
                                 const ImageData& input, ImageData& output) {
    plugin.setParams(params);
    PortValueMap inputs;
    inputs.insert(QStringLiteral("image"), QVariant::fromValue(input));
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

// ===== TableOutPut =====

void TestRemainingPlugins::testTableOutPutFormatsData() {
    TableOutPutPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("table_data", QVariantList{1, 2, 3, 4});
    QJsonObject params{{"rowCount", 2}, {"colCount", 2}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    const QString table = output.data("table_output").toString();
    QVERIFY(table.contains("1"));
    QVERIFY(table.contains("4"));
    QCOMPARE(output.data("table_rows").toInt(), 2);
    QCOMPARE(output.data("table_cols").toInt(), 2);
}

void TestRemainingPlugins::testTableOutPutEmptyDataGeneratesEmptyTable() {
    TableOutPutPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output; // 无 table_data
    QJsonObject params{{"rowCount", 2}, {"colCount", 3}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QCOMPARE(output.data("table_rows").toInt(), 2);
    QCOMPARE(output.data("table_cols").toInt(), 3);
}

void TestRemainingPlugins::testTableOutPutRowColParamsAffectOutput() {
    TableOutPutPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input;
    input.setData("table_data", QVariantList{1, 2, 3, 4, 5, 6});

    ImageData out1, out2;
    QJsonObject p1{{"rowCount", 2}, {"colCount", 3}};
    QJsonObject p2{{"rowCount", 3}, {"colCount", 2}};

    QVERIFY(runModule(plugin, p1, input, out1).success);
    QCOMPARE(out1.data("table_rows").toInt(), 2);
    QCOMPARE(out1.data("table_cols").toInt(), 3);

    QVERIFY(runModule(plugin, p2, input, out2).success);
    QCOMPARE(out2.data("table_rows").toInt(), 3);
    QCOMPARE(out2.data("table_cols").toInt(), 2);
}

void TestRemainingPlugins::testTableOutPutValidateRejectsBadDims() {
    TableOutPutPlugin plugin;
    QString error;

    QJsonObject badRows{{"rowCount", 0}, {"colCount", 2}};
    QVERIFY2(!plugin.validateParams(badRows, error), "rowCount=0 must be rejected");

    QJsonObject badCols{{"rowCount", 2}, {"colCount", 99}};
    QVERIFY2(!plugin.validateParams(badCols, error), "colCount=99 must be rejected");
}

void TestRemainingPlugins::testTableOutPutCloneIndependent() {
    TableOutPutPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"rowCount", 5}, {"colCount", 4}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("rowCount").toInt(), 5);

    cloneBase->setParam("rowCount", 99);
    QCOMPARE(plugin.currentParams().value("rowCount").toInt(), 5);
    delete clone;
}

// ===== ColorRecognition =====

void TestRemainingPlugins::testColorRecognitionDetectsSolidColor() {
#ifdef DEEPLUX_HAS_OPENCV
    ColorRecognitionPlugin plugin;
    QVERIFY(plugin.initialize());

    // 纯蓝色图像 (BGR: 255,0,0)
    ImageData input = makeSolidColorImage(255, 0, 0);
    QJsonObject params{{"targetColor", QStringLiteral("蓝色")}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    // G2-fix2: 验证识别结果——整图蓝色，中心点应被检出
    QVERIFY2(output.hasData("color_center_x"), "must output detected center");
    QVERIFY2(output.hasData("color_center_y"), "must output detected center");
    const int cx = output.data("color_center_x").toInt();
    const int cy = output.data("color_center_y").toInt();
    QVERIFY2(cx > 0 && cy > 0, "detected center must be within image");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestRemainingPlugins::testColorRecognitionAbsentColorFails() {
#ifdef DEEPLUX_HAS_OPENCV
    ColorRecognitionPlugin plugin;
    QVERIFY(plugin.initialize());

    // 纯蓝图像但查找红色 → 未检出，应失败
    ImageData input = makeSolidColorImage(255, 0, 0); // 蓝
    QJsonObject params{{"targetColor", QStringLiteral("红色")}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "absent target color must fail");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestRemainingPlugins::testColorRecognitionEmptyImageFails() {
#ifdef DEEPLUX_HAS_OPENCV
    ColorRecognitionPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input; // 空
    QJsonObject params{{"targetColor", QStringLiteral("红色")}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "empty image must fail");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestRemainingPlugins::testColorRecognitionCloneIndependent() {
    ColorRecognitionPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"targetColor", QStringLiteral("绿色")}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("targetColor").toString(), QString("绿色"));

    cloneBase->setParam("targetColor", QStringLiteral("蓝色"));
    QCOMPARE(plugin.currentParams().value("targetColor").toString(), QString("绿色"));
    delete clone;
}

// ===== DisplayData =====

void TestRemainingPlugins::testDisplayDataOverlaysText() {
#ifdef DEEPLUX_HAS_OPENCV
    DisplayDataPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input = makeSolidColorImage(50, 50, 50);
    QJsonObject params{{"displayText", QStringLiteral("HELLO")}, {"positionX", 10},
                       {"positionY", 30}, {"fontSize", 16}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    cv::Mat outMat = output.toMat();
    QVERIFY(!outMat.empty());
    QCOMPARE(outMat.cols, 120);
    QCOMPARE(outMat.rows, 120);

    // G2-fix2: 验证文本确实绘制——原图纯色 (50,50,50)，叠加绿色文本后应有像素偏离背景
    cv::Mat bg(120, 120, CV_8UC3, cv::Scalar(50, 50, 50));
    cv::Mat diff;
    cv::absdiff(outMat, bg, diff);
    QVERIFY2(cv::countNonZero(diff.reshape(1)) > 0,
             "text overlay must change pixels from the solid background");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestRemainingPlugins::testDisplayDataEmptyImageFails() {
#ifdef DEEPLUX_HAS_OPENCV
    DisplayDataPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input; // 空
    QJsonObject params{{"displayText", QStringLiteral("X")}, {"positionX", 5},
                       {"positionY", 15}, {"fontSize", 12}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "empty image must fail");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestRemainingPlugins::testDisplayDataCloneIndependent() {
    DisplayDataPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"displayText", QStringLiteral("ABC")}, {"positionX", 7},
                       {"positionY", 8}, {"fontSize", 20}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("fontSize").toInt(), 20);

    cloneBase->setParam("fontSize", 99);
    QCOMPARE(plugin.currentParams().value("fontSize").toInt(), 20);
    delete clone;
}

QTEST_MAIN(TestRemainingPlugins)
#include "test_remainingplugins.moc"
