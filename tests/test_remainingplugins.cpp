#include "core/base/ModuleBase.h"
#include "core/model/ImageData.h"
#include "plugins/detection/ColorRecognition/ColorRecognitionPlugin.h"
#include "plugins/detection/JiErHanDefectsDet/JiErHanDefectsDetPlugin.h"
#include "plugins/image_processing/DisplayData/DisplayDataPlugin.h"
#include "plugins/system/TableOutPut/TableOutPutPlugin.h"

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
    // 阶段 2：枚举校验 + 红色覆盖 HSV 高色相端
    void testColorRecognitionHighHueRedDetected();
    void testColorRecognitionInvalidColorRejected();

    // JiErHanDefectsDet（阶段 2：阈值真实参与过滤）
    void testJiErHanThresholdFiltersDefects();
    void testJiErHanInvalidThresholdRejected();

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

static ExecutionResult runModule(ModuleBase& plugin, const QJsonObject& params, const ImageData& input,
                                 ImageData& output) {
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

void TestRemainingPlugins::testColorRecognitionHighHueRedDetected() {
#ifdef DEEPLUX_HAS_OPENCV
    // 阶段 2：红色必须同时覆盖 HSV 色相环两端。
    // BGR(40,20,255) → OpenCV H≈177（靠近 180 边界的高色相红），旧实现只查 [0,10] 会漏检。
    ColorRecognitionPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input = makeSolidColorImage(40, 20, 255);
    QJsonObject params{{"targetColor", QStringLiteral("红色")}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QVERIFY2(output.hasData("color_center_x"), "high-hue red must be detected");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestRemainingPlugins::testColorRecognitionInvalidColorRejected() {
    // 阶段 2：颜色枚举严格校验，非法值失败关闭，不得静默回退
    ColorRecognitionPlugin plugin;
    QString error;

    QJsonObject bad{{"targetColor", QStringLiteral("粉色")}};
    QVERIFY2(!plugin.validateParams(bad, error), "invalid color enum must be rejected");

    // setParams 失败关闭：非法值不得覆盖已设置的合法值
    plugin.setParams(QJsonObject{{"targetColor", QStringLiteral("蓝色")}});
    plugin.setParams(QJsonObject{{"targetColor", QStringLiteral("粉色")}});
    QCOMPARE(plugin.currentParams().value("targetColor").toString(), QString("蓝色"));
}

// ===== JiErHanDefectsDet =====

#ifdef DEEPLUX_HAS_OPENCV
static ImageData makeDefectImage() {
    // 确定性图像：浅灰背景 + 一个大暗块（置信度≈1.0）+ 一个小暗块（置信度≈0.3）
    cv::Mat mat(240, 240, CV_8UC3, cv::Scalar(200, 200, 200));
    cv::rectangle(mat, cv::Rect(30, 30, 60, 60), cv::Scalar(20, 20, 20), cv::FILLED);
    cv::rectangle(mat, cv::Rect(150, 150, 18, 18), cv::Scalar(20, 20, 20), cv::FILLED);
    ImageData data;
    data.setMat(mat);
    return data;
}
#endif

void TestRemainingPlugins::testJiErHanThresholdFiltersDefects() {
#ifdef DEEPLUX_HAS_OPENCV
    // 阶段 2：阈值必须真实参与结果过滤——低阈值保留全部候选，高阈值滤掉低置信度候选
    JiErHanDefectsDetPlugin plugin;
    QVERIFY(plugin.initialize());

    const ImageData input = makeDefectImage();

    ImageData outLow, outHigh;
    QJsonObject low{{"threshold", 0.1}};  // 下限：保留全部候选
    QJsonObject high{{"threshold", 0.9}}; // 高阈值：仅保留高置信度候选

    QVERIFY2(runModule(plugin, low, input, outLow).success, "low threshold must keep candidates");
    QCOMPARE(outLow.data("defect_count").toInt(), 2);

    QVERIFY2(runModule(plugin, high, input, outHigh).success, "high threshold must keep high-confidence defect");
    QCOMPARE(outHigh.data("defect_count").toInt(), 1);
#else
    QSKIP("OpenCV not available");
#endif
}

void TestRemainingPlugins::testJiErHanInvalidThresholdRejected() {
    // 阶段 2：阈值范围与 metadata（0.1–1.0, step 0.05, 2 位小数）一致，越界失败关闭
    JiErHanDefectsDetPlugin plugin;
    QString error;

    QVERIFY2(!plugin.validateParams(QJsonObject{{"threshold", 0.0}}, error), "threshold=0 must be rejected");
    QVERIFY2(!plugin.validateParams(QJsonObject{{"threshold", 0.05}}, error),
             "threshold below min 0.1 must be rejected");
    QVERIFY2(!plugin.validateParams(QJsonObject{{"threshold", 1.5}}, error),
             "threshold above max 1.0 must be rejected");

    plugin.setParams(QJsonObject{{"threshold", 0.5}});
    plugin.setParams(QJsonObject{{"threshold", 2.0}});
    QCOMPARE(plugin.currentParams().value("threshold").toDouble(), 0.5);
}

// ===== DisplayData =====

void TestRemainingPlugins::testDisplayDataOverlaysText() {
#ifdef DEEPLUX_HAS_OPENCV
    DisplayDataPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input = makeSolidColorImage(50, 50, 50);
    QJsonObject params{
        {"displayText", QStringLiteral("HELLO")}, {"positionX", 10}, {"positionY", 30}, {"fontSize", 16}};

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
    QVERIFY2(cv::countNonZero(diff.reshape(1)) > 0, "text overlay must change pixels from the solid background");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestRemainingPlugins::testDisplayDataEmptyImageFails() {
#ifdef DEEPLUX_HAS_OPENCV
    DisplayDataPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input; // 空
    QJsonObject params{{"displayText", QStringLiteral("X")}, {"positionX", 5}, {"positionY", 15}, {"fontSize", 12}};

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
    QJsonObject params{{"displayText", QStringLiteral("ABC")}, {"positionX", 7}, {"positionY", 8}, {"fontSize", 20}};
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
